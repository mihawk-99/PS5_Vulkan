/* Copyright (C) 2026 Mihawk-99 */
// SPDX-License-Identifier: LGPL-2.1-or-later
// Owning-title GPU broker. Only trusted, application-owned clients may connect.
// Legacy gpu-wine log labels are retained for existing evidence parsers.
#include "server.h"
#include "completion.h"
#include "display.h"

#include <pthread.h>

extern "C" {
#include "radv_ps5_platform.h"
#include "protocol.h"
#include "frame_page.h"
#include "packet_io.h"
#include <ps5platform/kernel.h>
#include <ps5platform/agc.h>

int sceKernelUsleep(unsigned);
}
#include <cerrno>
#include <string>
#include <sched.h>
#include <cstdlib>

#include <new>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
namespace {
// PROTON_GPU_TRACE=1: a log line for every broker operation. Off, only events are logged (start, INIT, failures,
// errors, the first frames, closes): each line is written straight to the console's disk, and when every operation
// was logged and flushed, a submission's round trip took 160-197 ms.
const bool trace=[] { const char *v=getenv("PROTON_GPU_TRACE");return v && !strcmp(v,"1"); }();
// Waits for a message on one client's socket: 1 if one is there, 0 if not yet, -1 at the socket's end. A message is
// looked for by peeking: continuously until spin_until (the broker has just served this client, which is likely to ask
// again at once), then after a short poll that bounds the wait when idle. Adopted while poll was suspected of missing
// messages (graphics-child-gpu-async-11); the stalls then seen came from disk writes under the lock (async-18), so poll
// misbehaving is not established, but this costs a window of spinning and nothing else.
int await_message(int socket,uint64_t spin_until,bool *spun=nullptr)
{
    char peek=0;
    if(spun)*spun=true;
    for(;;) {
        const ssize_t found=recv(socket,&peek,1,MSG_PEEK|MSG_DONTWAIT);
        if(found==1)return 1;
        if(found==0 || (found<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR))return -1;
        if(radv_ps5_now_ns()>=spin_until)break;
        // Yield, not pause: the client this thread waits for may share its CPU, and a spin there keeps it off.
        sched_yield();
    }
    if(spun)*spun=false;
    pollfd waiting{socket,POLLIN,0};
    if(poll(&waiting,1,1)<0 && errno!=EINTR)return -1;
    const ssize_t found=recv(socket,&peek,1,MSG_PEEK|MSG_DONTWAIT);
    return found==1 ? 1 : found==0 || (found<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) ? -1 : 0;
}
constexpr uint64_t kServeSpinNs=1000000;
// The broker's log goes through a pipe that a thread of its own drains to the file: a line written straight to the
// console's disk under the broker's lock held every other client up to 262 ms (graphics-child-gpu-async-18), which
// showed as 2.3 ms per submission in the other client's 30-submission phases. A whole line is one write, atomic in the
// pipe, so lines from several threads do not interleave.
struct LogPipe {
    FILE *front=nullptr,*back=nullptr;int reader=-1;pthread_t writer{};bool running=false;
    bool open(FILE *file)
    {
        int ends[2];if(pipe(ends))return false;
        front=fdopen(ends[1],"w");
        if(!front) { ::close(ends[0]);::close(ends[1]);return false; }
        setvbuf(front,nullptr,_IOLBF,0);back=file;reader=ends[0];
        running=!pthread_create(&writer,nullptr,[](void *argument) -> void * {
            auto *self=static_cast<LogPipe *>(argument);char buffer[4096];
            for(;;) {
                const ssize_t got=read(self->reader,buffer,sizeof(buffer));
                if(got<0 && errno==EINTR)continue;
                if(got<=0)return nullptr;
                fwrite(buffer,1,size_t(got),self->back);fflush(self->back);
            }
        },this);
        if(!running) { fclose(front);::close(reader);front=nullptr;reader=-1;return false; }
        return true;
    }
    // Everything written so far reaches the file before this returns.
    void close()
    {
        if(!front)return;
        fclose(front);front=nullptr;
        if(running)pthread_join(writer,nullptr);
        running=false;::close(reader);reader=-1;fsync(fileno(back));
    }
};
size_t flexible_now() { size_t available=0;return sceKernelAvailableFlexibleMemorySize(&available) ? 0 : available; }
// A client buffer is host-visible (shared memory, mapped in both processes; fd set) or device-local (the owner's
// direct memory, mapped for the GPU and the owner only; memory set).
struct Buffer { uint64_t address=0,bytes=0;uint32_t token=0;int fd=-1;bool local=false,ranged=false;radv_ps5_memory memory{}; };
// The owner's side of the host-visible address ranges (PW_GPU_HOST_*): reserved whole while a broker runs, handed out
// first fit and given back coalesced, so freed addresses are used again. Without the reservation (something was there
// already), allocations fall back to reserving each buffer's own range at a cursor, as before.
struct HostRange {
    uint64_t base,bytes;bool reserved=false;
    HostRange(uint64_t at,uint64_t size):base(at),bytes(size) {}
    struct Extent { uint64_t at,bytes; } free[512];unsigned count=0;
    uint64_t high_water=0,taken=0,given=0;
    bool reserve(FILE *log)
    {
        void *at=(void *)base;
        reserved=!sceKernelReserveVirtualRange(&at,bytes,PW_GPU_RESERVE_FIXED|PW_GPU_RESERVE_NO_OVERWRITE,16384) && at==(void *)base;
        if(!reserved && at && at!=(void *)base)sceKernelMunmap(at,bytes);
        count=0;high_water=0;taken=given=0;
        if(reserved)free[count++]={base,bytes};
        fprintf(log,"gpu-broker host_range base=%#llx bytes=%llu reserved=%d\n",(unsigned long long)base,(unsigned long long)bytes,reserved);
        return reserved;
    }
    void release() { if(reserved)sceKernelMunmap((void *)base,bytes);reserved=false;count=0; }
    uint64_t take(uint64_t size,uint64_t alignment)
    {
        for(unsigned i=0;i<count;++i) {
            const uint64_t at=(free[i].at+alignment-1)&~(alignment-1),end=free[i].at+free[i].bytes;
            if(at<free[i].at || at>end || size>end-at)continue;
            const Extent before{free[i].at,at-free[i].at},after{at+size,end-at-size};
            if(before.bytes && after.bytes) {
                if(count==512)continue;
                for(unsigned j=count;j>i+1;--j)free[j]=free[j-1];
                free[i]=before;free[i+1]=after;++count;
            } else if(before.bytes)free[i]=before;
            else if(after.bytes)free[i]=after;
            else { for(unsigned j=i;j+1<count;++j)free[j]=free[j+1];--count; }
            if(at+size-base>high_water)high_water=at+size-base;
            ++taken;return at;
        }
        return 0;
    }
    void give(uint64_t at,uint64_t size)
    {
        unsigned i=0;while(i<count && free[i].at<at)++i;
        const bool join_before=i>0 && free[i-1].at+free[i-1].bytes==at,join_after=i<count && at+size==free[i].at;
        ++given;
        if(join_before && join_after) { free[i-1].bytes+=size+free[i].bytes;for(unsigned j=i;j+1<count;++j)free[j]=free[j+1];--count; }
        else if(join_before)free[i-1].bytes+=size;
        else if(join_after) { free[i].at=at;free[i].bytes+=size; }
        else if(count<512) { for(unsigned j=count;j>i;--j)free[j]=free[j-1];free[i]={at,size};++count; }
        // A free list full of fragments keeps that range reserved and unused: address space, not memory.
    }
};
HostRange host_ranges[2]{{PW_GPU_HOST_WINDOW_BASE,PW_GPU_HOST_WINDOW_BYTES},{PW_GPU_HOST_HIGH_BASE,PW_GPU_HOST_HIGH_BYTES}};
void host_ranges_reserve(FILE *log)
{
    for(auto &r:host_ranges)r.reserve(log);
}
void host_ranges_release(FILE *log)
{
    for(auto &r:host_ranges) {
        if(r.reserved)fprintf(log,"gpu-broker host_range base=%#llx high_water=%llu taken=%llu given=%llu extents=%u\n",
            (unsigned long long)r.base,(unsigned long long)r.high_water,(unsigned long long)r.taken,(unsigned long long)r.given,r.count);
        r.release();
    }
}
// Device-local memory: what a client is offered as its heap, and the direct memory always left to the owner and to
// every process's own (Wine's) allocations, whatever the clients ask.
constexpr uint64_t kLocalOffer=UINT64_C(8)<<30,kLocalFloor=UINT64_C(1)<<30;
constexpr unsigned kClientBuffers=1024;
struct Client {
    int socket=-1,service=-1,pid=-1;uint32_t sequence=0,next_token=0;
    Buffer buffers[kClientBuffers];unsigned submissions=0;
    uint64_t allocated=0,local=0,local_peak=0,tess=0;uint32_t tess_bytes=0,granularity=0,buffering=0;
    // Where submission time goes, in nanoseconds: staging allocation and copy, the native submit, the completion wait
    // and the staging release.
    uint64_t time_stage=0,time_submit=0,time_wait=0,time_release=0,waits=0,time_recv=0,recv_samples=0,poll_missed=0;
    uint64_t found[2]{},waited[4]{},lock_ns=0,handle_ns=0,handled=0,op_ns[16]{},op_count[16]{},op_max_ns[16]{};
    unsigned images=0,presentations=0,coherence=0,coherent=0,timings=0;
    int cookie=0;
    const char *allocation_stage="none";
    bool ended=false,offchip=false,done=false,passed=false,serving=false;
    pthread_t server{};
};
// A client is about 76 KiB: reset it where it lives. Client{} as a temporary would put that on the caller's stack,
// a Wine thread's among them (graphics-child-gpu-local-dxvk-01: the owner's SIGSEGV just below its stack pointer).
void reset(Client &c)
{
    c.~Client();
    new (&c) Client();
}
bool (*client_absent)(int)=nullptr;
bool absent(int token) { return client_absent && client_absent(token); }
std::string output_directory;
std::string output_path(const char *path)
{ const char *slash=strrchr(path,'/');return output_directory+"/"+(slash ? slash+1 : path); }
[[noreturn]] void retain(FILE *out,const char *reason)
{
    fprintf(out,"gpu-broker unsafe_to_close=1 reason=%s\n",reason);fsync(fileno(out));
    for(;;)sceKernelUsleep(100000);
}
// When the broker cannot show the GPU has finished with something: the probes stop dead (retain), so the run shows
// it. The Wine broker instead marks the GPU wedged, frees nothing from then on, and answers EIO, so a game sees its
// device lost and the title can still go back to the launcher, whose process replacement reclaims everything.
bool retain_on_failure=true,wedged=false;
// Always false: the caller must not go on as if it had succeeded.
bool give_up(FILE *out,const char *reason)
{
    if(retain_on_failure)retain(out,reason);
    if(!wedged) { wedged=true;fprintf(out,"gpu-broker wedged=1 reason=%s\n",reason); }
    return false;
}
// PROTON_GPU_TEST_FAIL_SUBMIT=n (Wine broker): the submission after n succeeds is refused before it reaches the GPU,
// as a failed one would be, to show the title surviving a broker failure. 0: off.
unsigned test_fail_submit=0,test_submissions=0;
bool owned(const Client &c,uint64_t address,uint64_t bytes)
{
    for(const auto &b:c.buffers)if(b.token && pw_gpu_contains(b.address,b.bytes,address,bytes))return true;
    return false;
}
void release(Buffer &b)
{
    if(b.local)radv_ps5_memory_free(&b.memory);
    else if(b.ranged) {
        // Back to a reservation laid over the mapping (it replaces it), and the range back to the free list.
        void *at=(void *)b.address;auto &range=host_ranges[pw_gpu_host_range(b.address,b.bytes)];
        if(!sceKernelReserveVirtualRange(&at,b.bytes,PW_GPU_RESERVE_FIXED,16384) && at==(void *)b.address)range.give(b.address,b.bytes);
        else sceKernelMunmap((void *)b.address,b.bytes);
    }
    else if(b.address)sceKernelMunmap((void *)b.address,b.bytes);
    if(b.fd>=0)close(b.fd);
    b=Buffer{};
}
// The device-local heap a client is told it has: the offer, or what direct memory above the floor allows now.
uint64_t local_offer()
{
    const uint64_t available=radv_ps5_memory_available_bytes();
    const uint64_t room=available>kLocalFloor ? (available-kLocalFloor)&~16383ull : 0;
    return room<kLocalOffer ? room : kLocalOffer;
}
int allocate_local(Client &c,PwGpuMessage &m,Buffer &b,uint64_t bytes,uint64_t alignment)
{
    c.allocation_stage="local_budget";
    if(bytes>kLocalOffer-c.local)return ENOMEM;
    c.allocation_stage="local_floor";
    if(radv_ps5_memory_available_bytes()<kLocalFloor+bytes)return ENOMEM;
    c.allocation_stage="local_direct";
    radv_ps5_memory memory{};
    if(!radv_ps5_memory_alloc(bytes,alignment,m.argument[1]!=0,&memory))return ENOMEM;
    if(m.argument[2]&PW_GPU_ALLOC_ZERO) { memset(memory.cpu,0,memory.bytes);radv_ps5_cpu_flush(memory.cpu,memory.bytes); }
    b=Buffer{(uint64_t)memory.cpu,memory.bytes,++c.next_token,-1,true,false,memory};
    c.local+=memory.bytes;if(c.local>c.local_peak)c.local_peak=c.local;
    m.address=b.address;m.bytes=b.bytes;m.argument[0]=b.token;
    c.allocation_stage="complete";
    return 0;
}
int allocate(Client &c,PwGpuMessage &m,uint64_t &next_window,uint64_t &next_high,int &fd)
{
    c.allocation_stage="validation";
    const bool local=m.argument[2]&PW_GPU_ALLOC_LOCAL;
    if(!m.bytes || m.bytes>(local ? kLocalOffer : PW_GPU_LIMIT) || m.argument[0]>PW_GPU_ALIGNMENT_MAX ||
       (m.argument[0] && (m.argument[0]&(m.argument[0]-1))) || m.argument[1]>1 ||
       (m.argument[2]&~(PW_GPU_ALLOC_LOCAL|PW_GPU_ALLOC_ZERO)))return EINVAL;
    const uint64_t bytes=(m.bytes+16383)&~16383ull,alignment=m.argument[0]>16384 ? m.argument[0] : 16384;
    Buffer *b=nullptr;for(auto &slot:c.buffers)if(!slot.token){b=&slot;break;}
    c.allocation_stage="slot";if(!b)return ENOSPC;
    if(local)return allocate_local(c,m,*b,bytes,alignment);
    c.allocation_stage="client_budget";
    if(bytes>PW_GPU_LIMIT-c.allocated)return ENOMEM;
    HostRange &range=host_ranges[m.argument[1] ? 0 : 1];
    if(range.reserved) {
        c.allocation_stage="range";
        const uint64_t address=range.take(bytes,alignment);
        if(!address)return ENOMEM;
        c.allocation_stage="shm";
        const int shared=shm_open(SHM_ANON,O_RDWR,0600);
        void *mapped=MAP_FAILED;
        if(shared>=0 && !ftruncate(shared,bytes)) {
            c.allocation_stage="map";
            mapped=mmap((void *)address,bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,shared,0);
        }
        bool granted=mapped==(void *)address;
        if(granted) { c.allocation_stage="gpu_permissions";granted=radv_ps5_memory_grant_gpu(mapped,bytes); }
        if(!granted) {
            void *at=(void *)address;
            if(!sceKernelReserveVirtualRange(&at,bytes,PW_GPU_RESERVE_FIXED,16384) && at==(void *)address)range.give(address,bytes);
            if(shared>=0)close(shared);
            return ENOMEM;
        }
        *b=Buffer{address,bytes,++c.next_token,shared,false,true};c.allocated+=bytes;
        m.address=address;m.bytes=bytes;m.argument[0]=b->token;fd=shared;
        c.allocation_stage="complete";
        return 0;
    }
    // The cursor's next address may already hold one of the owner's own window mappings (which the kernel places,
    // device-local buffers included): step past what is there, a bounded number of times.
    uint64_t &cursor=m.argument[1] ? next_window : next_high;
    void *reserved=nullptr;uint64_t address=0;
    for(unsigned attempt=0;;++attempt) {
        address=(cursor+alignment-1)&~(alignment-1);
        c.allocation_stage="window";
        if(m.argument[1] && !pw_gpu_contains(RADV_PS5_WINDOW_BASE,RADV_PS5_WINDOW_BYTES,address,bytes))return ENOMEM;
        reserved=(void *)address;
        c.allocation_stage="reserve";
        if(!sceKernelReserveVirtualRange(&reserved,bytes,0,alignment) && reserved==(void *)address)break;
        if(reserved && reserved!=(void *)address)sceKernelMunmap(reserved,bytes);
        if(attempt==63)return EADDRINUSE;
        cursor=address+(bytes>(UINT64_C(2)<<20) ? bytes : UINT64_C(2)<<20);
    }
    c.allocation_stage="shm";
    const int shared=shm_open(SHM_ANON,O_RDWR,0600);
    void *mapped=MAP_FAILED;
    if(shared>=0) {
        c.allocation_stage="truncate";
        if(!ftruncate(shared,bytes)) {
            c.allocation_stage="map";
            mapped=mmap(reserved,bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,shared,0);
        }
    }
    bool granted=false;
    if(mapped==reserved) { c.allocation_stage="gpu_permissions";granted=radv_ps5_memory_grant_gpu(mapped,bytes); }
    if(!granted) {
        sceKernelMunmap(reserved,bytes);if(shared>=0)close(shared);return ENOMEM;
    }
    *b=Buffer{address,bytes,++c.next_token,shared};c.allocated+=bytes;cursor=address+bytes;
    m.address=address;m.bytes=bytes;m.argument[0]=b->token;fd=shared;
    c.allocation_stage="complete";
    return 0;
}
// Asynchronous submission. The broker copies a client's stream into owner-private staging, appends its own
// end-of-work packet writing a sequence number to an owner-private fence page, submits, and replies at once: the
// client waits on its own marker, which the GPU's writes reach coherently (graphics-child-gpu-coherence-*). Every
// client's work goes to the one GPU ring in submission order, so one fence orders it all. Staging, and the backing of
// any buffer a client frees while work is in flight, are released only once the fence has passed the last
// submission that could use them.
struct Inflight { uint32_t seq;radv_ps5_memory staging; };
struct Deferred { Buffer buffer;uint32_t seq; };
struct Fences {
    radv_ps5_memory page{};
    uint32_t issued=0;
    static constexpr unsigned kRing=512,kDeferred=1024;
    Inflight ring[kRing]{};unsigned head=0,count=0;
    Deferred deferred[kDeferred]{};unsigned deferred_count=0;
    // The GPU state every client shares, as last set: changing it waits for all work in flight.
    uint64_t tess=0;uint32_t tess_bytes=0,granularity=0,buffering=0;bool offchip=false;
    uint64_t drains=0,state_drains=0;
    // The suspend point that starts a submission promptly (ps5platform/agc.h), made by the broker's own thread once its
    // replies are out, rather than by the platform's kick thread inside the round trip.
    bool kick=false;uint64_t kicks=0,kick_ns=0;
} fences;
int broker_submit(void *words,uint32_t count)
{
    radv_ps5_cpu_flush(words,size_t(count)*4);
    ps5_agc_submit_description description{words,count,0,{}};
    const int status=sceAgcDriverSubmitDcb(&description);
    if(!status)fences.kick=true;
    return status;
}
void kick()
{
    if(!fences.kick)return;
    const uint64_t start=radv_ps5_now_ns();
    sceAgcSuspendPoint();
    fences.kick_ns+=radv_ps5_now_ns()-start;++fences.kicks;fences.kick=false;
}
bool done(uint32_t seq,uint32_t completed) { return int32_t(seq-completed)<=0; }
uint32_t completed()
{
    auto *value=(volatile uint32_t *)fences.page.cpu;
    radv_ps5_cpu_flush((const void *)value,4);return *value;
}
// Releases whatever the fence has passed.
void reap()
{
    if(!fences.page.cpu)return;
    const uint32_t now=completed();
    while(fences.count && done(fences.ring[fences.head].seq,now)) {
        radv_ps5_memory_free(&fences.ring[fences.head].staging);
        fences.ring[fences.head]=Inflight{};fences.head=(fences.head+1)%Fences::kRing;--fences.count;
    }
    for(unsigned i=0;i<fences.deferred_count;)
        if(done(fences.deferred[i].seq,now)) { release(fences.deferred[i].buffer);fences.deferred[i]=fences.deferred[--fences.deferred_count]; }
        else ++i;
}
// Waits, up to the timeout, until nothing is in flight. False: the GPU never got there, and nothing it might still
// use has been released.
bool drain(uint64_t timeout_ns)
{
    reap();
    if(!fences.count && !fences.deferred_count)return true;
    ++fences.drains;
    const uint64_t end=radv_ps5_now_ns()+timeout_ns;
    while((fences.count || fences.deferred_count) && radv_ps5_now_ns()<end) { sceKernelUsleep(50);reap(); }
    return !fences.count && !fences.deferred_count;
}
// After a drain: the fence page goes back, and the state is as at start. In place, as Fences is about 100 KiB.
void fences_reset()
{
    if(fences.page.cpu)radv_ps5_memory_free(&fences.page);
    fences.~Fences();new (&fences) Fences();
}
bool fences_ready(FILE *out)
{
    if(fences.page.cpu)return true;
    if(!radv_ps5_memory_alloc(16384,16384,false,&fences.page))return false;
    *(volatile uint32_t *)fences.page.cpu=fences.issued;radv_ps5_cpu_flush(fences.page.cpu,64);
    fprintf(out,"gpu-broker fences address=%#llx\n",(unsigned long long)(uint64_t)fences.page.cpu);
    return true;
}
// A buffer the client let go of: released now if no work is in flight, otherwise once the fence passes the last
// submission so far.
void free_buffer(Client &c,Buffer &b,FILE *out)
{
    (b.local ? c.local : c.allocated)-=b.bytes;
    if(wedged) { b=Buffer{};return; }   // left mapped: nothing is freed once the GPU is in doubt
    reap();
    if(!fences.count) { release(b);return; }
    if(fences.deferred_count==Fences::kDeferred && !drain(5000000000ull)) { give_up(out,"deferred_free_stalled");b=Buffer{};return; }
    if(!fences.count) { release(b);return; }
    fences.deferred[fences.deferred_count++]=Deferred{b,fences.issued};b=Buffer{};
}
int submit(Client &c,const PwGpuMessage &m,FILE *out,unsigned client)
{
    if(!m.bytes || m.bytes%4 || m.bytes>16*1024*1024 || m.address%4 || !owned(c,m.address,m.bytes) ||
       !m.argument[0] || m.argument[0]%4 || !owned(c,m.argument[0],4))return EINVAL;
    if(wedged)return EIO;
    if(!fences_ready(out))return ENOMEM;
    const uint64_t t0=radv_ps5_now_ns();
    reap();
    if(fences.count==Fences::kRing && !drain(5000000000ull))return give_up(out,"vulkan_ring_stalled"),EIO;
    // The GPU state all clients share changes only with nothing in flight that might depend on the old value.
    const bool tess_changes=c.tess && (c.tess!=fences.tess || c.tess_bytes!=fences.tess_bytes);
    const bool offchip_changes=c.offchip && (!fences.offchip || c.granularity!=fences.granularity || c.buffering!=fences.buffering);
    if(tess_changes || offchip_changes) {
        ++fences.state_drains;
        if(!drain(5000000000ull))return give_up(out,"vulkan_state_drain_stalled"),EIO;
    }
    const uint64_t t1=radv_ps5_now_ns();c.time_wait+=t1-t0;
    radv_ps5_memory staging{};
    if(!radv_ps5_memory_alloc(m.bytes+32,16384,false,&staging))return ENOMEM;
    memcpy(staging.cpu,(void *)m.address,m.bytes);
    uint32_t seq=++fences.issued;if(!seq)seq=++fences.issued;
    ps5_gpu_completion_packet((uint32_t *)((uint8_t *)staging.cpu+m.bytes),(uint64_t)fences.page.cpu,seq);
    const uint64_t t2=radv_ps5_now_ns();c.time_stage+=t2-t1;
    int status=0;
    if(tess_changes && !(status=radv_ps5_set_tess_factor_ring(c.tess,c.tess_bytes))) { fences.tess=c.tess;fences.tess_bytes=c.tess_bytes; }
    if(!status && offchip_changes && !(status=radv_ps5_set_hs_offchip_param(c.granularity,c.buffering))) {
        fences.offchip=true;fences.granularity=c.granularity;fences.buffering=c.buffering;
    }
    if(!status) {
        const bool refused=test_fail_submit && ++test_submissions>test_fail_submit;
        status=refused ? EIO : broker_submit(staging.cpu,(uint32_t)(m.bytes/4)+8);
        // An error cannot prove that nothing reached the hardware: the staging stays, and nothing else is freed.
        if(status) { give_up(out,refused ? "test_submit_failure" : "vulkan_submit_error");return EIO; }
    }
    if(status) { radv_ps5_memory_free(&staging);return status; }
    fences.ring[(fences.head+fences.count++)%Fences::kRing]=Inflight{seq,staging};
    c.time_submit+=radv_ps5_now_ns()-t2;++c.submissions;++c.waits;
    const bool final=true;
    // Tracing only: a line per submission, flushed to disk, cost 90-190 ms each (graphics-child-gpu-sync-timing-03).
    if(trace)fprintf(out,"gpu-vulkan client=%u submission=%u words=%llu final=%d status=0\n",client,c.submissions,(unsigned long long)(m.bytes/4),final);
    return 0;
}
}

namespace {
struct Broker {
    pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER,display_lock=PTHREAD_MUTEX_INITIALIZER;
    Client clients[32];
    Ps5GpuDisplay display;
    uint64_t window=0x240000000ull,high=0x5100000000ull;
    int next_cookie=0;
    bool stopping=false,capture=false,started=false,local_memory=false;
    // PROTON_GPU_CAPTURE_EVERY=N: also every Nth frame, at most 12 of them, for a long-running client.
    unsigned capture_every=0;
    LogPipe log_pipe;FILE *log_file=nullptr;
    // The lowest free flexible and direct memory seen after any allocation: what the clients' memory cost the title.
    size_t flexible_min=SIZE_MAX;uint64_t direct_min=UINT64_MAX;
    // The most shared (host-visible) and device-local client memory live at once, across every client.
    uint64_t host_peak=0,local_peak=0;
    FILE *log=nullptr;
    // The screen (docs/WINDOWS_PROCESSES.md, "The desktop"), under display_lock: which source it shows. A source is a
    // Vulkan client (its cookie) or a helper's frame page (minus its pid), with the native process it belongs to.
    int holder=0,holder_pid=0;uint64_t holder_ns=0;unsigned switches=0,hidden=0;
    struct FrameSource { int pid=0;PwFramePage *page=nullptr;uint32_t shown=0; } frames[32];
    uint8_t *frame_copy=nullptr;int cursor_x=INT32_MIN,cursor_y=INT32_MIN;
    pthread_t compositor{};bool compositing=false,compositor_stop=false;
    // Under display_lock: a picture the application asked for (ps5_gpu_broker_capture_next), of the next frame shown.
    std::string capture_next;
} broker_state;
// The desktop state, supplied by the application: the native process of the foreground window, and the cursor.
int (*desktop_foreground)()=nullptr;
void (*desktop_cursor)(int *,int *)=nullptr;
// A source keeps the screen while it presents at least this often; then the next source to present takes it. The
// foreground window's process takes it whenever it presents.
constexpr uint64_t kHoldNs=300000000ull;
bool may_show(Broker &b,int source,int pid,uint64_t now)
{
    if(b.holder==source) { b.holder_ns=now;return true; }
    const int foreground=desktop_foreground ? desktop_foreground() : 0;
    const bool idle=!b.holder || now-b.holder_ns>kHoldNs;
    const bool focus=foreground && pid==foreground && b.holder_pid!=foreground;
    if(!idle && !focus) { ++b.hidden;return false; }
    if(b.switches<64)fprintf(b.log,"gpu-desktop screen source=%d pid=%d from=%d foreground=%d reason=%s\n",source,pid,
        b.holder,foreground,focus ? "foreground" : "idle");
    b.holder=source;b.holder_pid=pid;b.holder_ns=now;++b.switches;return true;
}
// Where the pointer goes on the 1920x1080 screen for a picture of width x height whose top-left corner is at
// (x, y) on the desktop: the screen shows the picture scaled to fill it.
void screen_cursor(int x,int y,unsigned width,unsigned height,int &screen_x,int &screen_y)
{
    screen_x=screen_y=-1;
    if(!desktop_cursor)return;
    int cx,cy;desktop_cursor(&cx,&cy);cx-=x;cy-=y;
    if(cx<0 || cy<0 || unsigned(cx)>=width || unsigned(cy)>=height)return;
    screen_x=int(int64_t(cx)*1920/width);screen_y=int(int64_t(cy)*1080/height);
}
void note_peaks(Broker &b)
{
    uint64_t host=0,local=0;
    for(const auto &c:b.clients)if(c.socket>=0) { host+=c.allocated;local+=c.local; }
    if(host>b.host_peak)b.host_peak=host;
    if(local>b.local_peak)b.local_peak=local;
}
// One thread per client, started with its slot, waiting on that client's socket alone (await_message); the broker's
// work is serialized by its lock, taken only while a message is handled. The single worker this replaces held the
// lock across a 20 ms poll of every socket, so opening or closing a slot, and every other client, waited on it.
void *client_main(void *argument)
{
    auto &b=broker_state;auto &c=*static_cast<Client *>(argument);
    const int socket=c.socket;uint64_t spin_until=0,idle_since=radv_ps5_now_ns();
    for(;;) {
        const int found=await_message(socket,spin_until);
        pthread_mutex_lock(&b.lock);
        if(b.stopping || c.ended || found<0) { c.ended=true;pthread_mutex_unlock(&b.lock);return nullptr; }
        if(!found) {
            // Idle: what the fence has passed is released now and then, not on every short wait.
            if(radv_ps5_now_ns()-idle_since>20000000) { reap();idle_since=radv_ps5_now_ns(); }
            pthread_mutex_unlock(&b.lock);continue;
        }
        do {
            PwGpuMessage m{};int received_fds[2];unsigned count=0;
            const ssize_t bytes=ps5_gpu_packet_recv(c.socket,&m,sizeof(m),received_fds,&count);
            for(unsigned n=0;n<count;++n)close(received_fds[n]);
            if(bytes!=sizeof(m) || count || m.magic!=PW_GPU_MAGIC || m.status || m.sequence!=c.sequence+1) {
                c.ended=true;break;
            }
            c.sequence=m.sequence;m.status=EINVAL;int fd=-1;
            if(m.operation==PW_GPU_INIT && c.pid<0 && m.argument[0]>0 && m.argument[0]<=INT32_MAX && m.argument[0]!=uint64_t(getpid())) {
                c.pid=(int)m.argument[0];m.status=0;
                const uint64_t ranges_reserved_reported=m.argument[2];m.argument[2]=m.argument[3]=0;
                if(b.local_memory) { m.argument[2]=local_offer();m.argument[3]=PW_GPU_LIMIT; }
                fprintf(b.log,"gpu-wine cookie=%d pid=%d local_offer=%llu host_limit=%llu flexible=%zu ranges_reserved=%llu\n",c.cookie,c.pid,
                    (unsigned long long)m.argument[2],(unsigned long long)m.argument[3],flexible_now(),(unsigned long long)ranges_reserved_reported);
            } else if(c.pid>0) switch(m.operation) {
            case PW_GPU_ALLOC:
                m.status=wedged ? EIO : allocate(c,m,b.window,b.high,fd);
                {
                    size_t flexible=0;
                    if(!sceKernelAvailableFlexibleMemorySize(&flexible) && flexible<b.flexible_min)b.flexible_min=flexible;
                    const uint64_t direct=radv_ps5_memory_available_bytes();if(direct<b.direct_min)b.direct_min=direct;
                }
                if(m.status) {
                    size_t available=0;const int status=sceKernelAvailableFlexibleMemorySize(&available);
                    fprintf(b.log,"gpu-wine allocation_failed=1 cookie=%d pid=%d stage=%s bytes=%llu flexible_status=%#x flexible_available=%llu\n",
                        c.cookie,c.pid,c.allocation_stage,(unsigned long long)m.bytes,unsigned(status),(unsigned long long)available);
                }break;
            case PW_GPU_FREE:
                for(auto &a:c.buffers)if(a.token && a.token==m.argument[0] && a.address==m.address && a.bytes==m.bytes) {
                    if(c.tess && pw_gpu_contains(a.address,a.bytes,c.tess,c.tess_bytes)) { c.tess=0;c.tess_bytes=0; }
                    free_buffer(c,a,b.log);m.status=0;break;
                }
                break;
            case PW_GPU_SUBMIT: m.status=submit(c,m,b.log,c.cookie);break;
            case PW_GPU_TESS:
                if(m.bytes<=UINT32_MAX && owned(c,m.address,m.bytes)) { c.tess=m.address;c.tess_bytes=(uint32_t)m.bytes;m.status=0; }break;
            case PW_GPU_OFFCHIP:
                if(m.argument[0]<=3 && m.argument[1]<=511) {
                    c.granularity=(uint32_t)m.argument[0];c.buffering=(uint32_t)m.argument[1];c.offchip=true;m.status=0;
                }break;
            case PW_GPU_SWAPCHAIN:
                if(wedged) { m.status=EIO;break; }
                if(m.argument[0]>0 && m.argument[0]<=1920 && m.argument[1]>0 && m.argument[1]<=1080 &&
                   m.argument[2]>=m.argument[0]*4 && m.argument[2]<=1920*4 && m.argument[3]<=1 &&
                   m.bytes==m.argument[1]*m.argument[2] && !(m.address&3) && owned(c,m.address,m.bytes)) {
                    // No cache maintenance: the client waited for its frame's fence, and GPU writes reach CPU reads
                    // coherently (graphics-child-gpu-coherence-*). The copy runs outside the broker's lock, under the
                    // display's own, so other clients' submissions go on meanwhile: this client's buffers cannot go
                    // away, as its next request waits for this reply and closing it joins this thread first.
                    char path[160];snprintf(path,sizeof(path),"/app0/gpu-child-%d-%u.ppm",c.cookie,c.presentations);
                    const bool periodic=b.capture_every && c.presentations && !(c.presentations%b.capture_every) &&
                                        c.presentations/b.capture_every<=12;
                    const std::string capture=b.capture && (c.presentations<4 || periodic) ? output_path(path) : std::string();
                    pthread_mutex_unlock(&b.lock);
                    pthread_mutex_lock(&b.display_lock);
                    // A client that does not hold the screen is told its frame was shown, a frame's time later,
                    // so it paces as it would on a display.
                    const bool shown=may_show(b,c.cookie,c.pid,radv_ps5_now_ns());
                    if(shown) {
                        int cursor_x,cursor_y;screen_cursor(0,0,(unsigned)m.argument[0],(unsigned)m.argument[1],cursor_x,cursor_y);
                        std::string asked;asked.swap(b.capture_next);
                        const std::string &picture=asked.empty() ? capture : asked;
                        m.status=b.display.present((void *)m.address,(unsigned)m.argument[0],(unsigned)m.argument[1],
                            (unsigned)m.argument[2],b.log,picture.empty() ? nullptr : picture.c_str(),m.argument[3]!=0,
                            cursor_x,cursor_y);
                    } else m.status=0;
                    pthread_mutex_unlock(&b.display_lock);
                    if(!shown)sceKernelUsleep(16000);
                    pthread_mutex_lock(&b.lock);
                    if(!m.status)++c.presentations;
                    if(trace || m.status || c.presentations<=8 || !(c.presentations%600))
                        fprintf(b.log,"gpu-wine cookie=%d pid=%d frame=%u extent=%llux%llu flip=%llu status=%#x\n",c.cookie,c.pid,c.presentations,
                            (unsigned long long)m.argument[0],(unsigned long long)m.argument[1],(unsigned long long)b.display.flips,unsigned(m.status));
                }break;
            default:break;
            }
            if(trace || m.status)
                fprintf(b.log,"gpu-wine cookie=%d pid=%d operation=%u status=%#x live_bytes=%llu local_bytes=%llu\n",c.cookie,c.pid,m.operation,unsigned(m.status),(unsigned long long)c.allocated,(unsigned long long)c.local);
            note_peaks(b);
            if(ps5_gpu_packet_send(c.socket,&m,sizeof(m),fd>=0 ? &fd : nullptr,fd>=0 ? 1 : 0))c.ended=true;
            kick();
        } while(false);
        const bool ended=c.ended;
        pthread_mutex_unlock(&b.lock);
        if(ended)return nullptr;
        spin_until=radv_ps5_now_ns()+kServeSpinNs;idle_since=radv_ps5_now_ns();
    }
}
}
namespace {
// Helpers' frames (src/pw_frame_page.h): every 4 ms, the newest picture of a frame page that may hold the screen is
// copied out of the page, checked whole, and shown, with the pointer over it; the holder's picture is shown again
// when only the pointer moved.
void *compositor_main(void *)
{
    auto &b=broker_state;
    for(;;) {
        sceKernelUsleep(4000);
        pthread_mutex_lock(&b.display_lock);
        if(b.compositor_stop) { pthread_mutex_unlock(&b.display_lock);return nullptr; }
        int cx=INT32_MIN,cy=INT32_MIN;if(desktop_cursor)desktop_cursor(&cx,&cy);
        const bool cursor_moved=cx!=b.cursor_x || cy!=b.cursor_y;
        for(auto &f:b.frames) {
            if(!f.page)continue;
            const uint32_t sequence=__atomic_load_n(&f.page->sequence,__ATOMIC_ACQUIRE);
            if((sequence&1) || !sequence)continue;
            const bool fresh=sequence!=f.shown,held=b.holder==-f.pid;
            if((!fresh && !(held && cursor_moved)) || !may_show(b,-f.pid,f.pid,radv_ps5_now_ns()))continue;
            const unsigned width=f.page->width,height=f.page->height,stride=f.page->stride;
            const int x=f.page->x,y=f.page->y;
            if(!width || width>PW_FRAME_WIDTH || !height || height>PW_FRAME_HEIGHT || stride!=width*4)continue;
            memcpy(b.frame_copy,pw_frame_pixels(f.page),size_t(stride)*height);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if(__atomic_load_n(&f.page->sequence,__ATOMIC_ACQUIRE)!=sequence)continue; // torn: the next tick
            int screen_x,screen_y;screen_cursor(x,y,width,height,screen_x,screen_y);
            // PROTON_GPU_CAPTURE: a picture of each helper's first frame as the screen showed it, pointer included.
            char path[96];snprintf(path,sizeof(path),"/app0/gpu-desktop-%d.ppm",f.pid);
            const std::string capture=b.capture && !f.shown ? output_path(path) : std::string();
            std::string asked;asked.swap(b.capture_next);
            const std::string &picture=asked.empty() ? capture : asked;
            const int status=b.display.present(b.frame_copy,width,height,stride,b.log,picture.empty() ? nullptr : picture.c_str(),
                true,screen_x,screen_y);
            if(!f.shown || status)fprintf(b.log,"gpu-desktop frame pid=%d %ux%u at %d,%d cursor=%d,%d status=%#x\n",f.pid,
                width,height,x,y,screen_x,screen_y,unsigned(status));
            f.shown=sequence;
            break;
        }
        b.cursor_x=cx;b.cursor_y=cy;
        pthread_mutex_unlock(&b.display_lock);
    }
}
}
unsigned long long ps5_gpu_broker_flips()
{
    auto &b=broker_state;
    pthread_mutex_lock(&b.display_lock);
    const unsigned long long flips=b.display.flips;
    pthread_mutex_unlock(&b.display_lock);
    return flips;
}
bool ps5_gpu_broker_capture_next(const char *path)
{
    auto &b=broker_state;
    if(!path || !*path)return false;
    pthread_mutex_lock(&b.display_lock);
    const bool asked=b.started && b.capture_next.empty();
    if(asked)b.capture_next=path;
    pthread_mutex_unlock(&b.display_lock);
    return asked;
}
void ps5_gpu_broker_set_desktop(int (*foreground)(),void (*cursor)(int *,int *))
{
    desktop_foreground=foreground;desktop_cursor=cursor;
}
void ps5_gpu_broker_frame_attach(int pid,int fd)
{
    auto &b=broker_state;
    void *mapped=mmap(nullptr,PW_FRAME_BYTES,PROT_READ,MAP_SHARED,fd,0);
    close(fd);
    auto *page=mapped==MAP_FAILED ? nullptr : static_cast<PwFramePage *>(mapped);
    bool attached=false;
    pthread_mutex_lock(&b.display_lock);
    if(page && page->magic==PW_FRAME_MAGIC && b.started)
        for(auto &f:b.frames)if(!f.page) { f.pid=pid;f.page=page;f.shown=0;attached=true;break; }
    if(b.log)fprintf(b.log,"gpu-desktop frames pid=%d attached=%d\n",pid,attached);
    pthread_mutex_unlock(&b.display_lock);
    if(page && !attached)munmap(page,PW_FRAME_BYTES);
}
void ps5_gpu_broker_frame_detach(int pid)
{
    auto &b=broker_state;
    pthread_mutex_lock(&b.display_lock);
    for(auto &f:b.frames)if(f.page && f.pid==pid) { munmap(f.page,PW_FRAME_BYTES);f=Broker::FrameSource{}; }
    if(b.holder==-pid) { b.holder=0;b.holder_pid=0; }
    pthread_mutex_unlock(&b.display_lock);
}
bool ps5_gpu_broker_start(const char *directory,bool capture,bool local,bool (*is_absent)(int))
{
    auto &b=broker_state;if(b.started || !directory || !*directory || !is_absent)return false;
    output_directory=directory;client_absent=is_absent;
    b.local_memory=local;
    retain_on_failure=false;wedged=false;test_submissions=0;
    {
        const char *fail=getenv("PROTON_GPU_TEST_FAIL_SUBMIT");
        test_fail_submit=fail ? (unsigned)strtoul(fail,nullptr,10) : 0;
    }
    b.log_file=fopen(output_path("/app0/gpu-broker-results.txt").c_str(),"w");
    if(!b.log_file) { fprintf(stderr,"gpu-wine startup stage=open errno=%d\n",errno);return false; }
    setvbuf(b.log_file,nullptr,_IONBF,0);
    b.log=b.log_pipe.open(b.log_file) ? b.log_pipe.front : b.log_file;
    if(!radv_ps5_platform_init() || !ps5_gpu_broker_warm(b.log)) { b.log_pipe.close();fclose(b.log_file);b.log=b.log_file=nullptr;return false; }
    b.capture=capture;b.stopping=false;
    {
        const char *every=getenv("PROTON_GPU_CAPTURE_EVERY");
        b.capture_every=every ? (unsigned)strtoul(every,nullptr,10) : 0;
    }
    host_ranges_reserve(b.log);
    fprintf(b.log,"gpu-wine broker test_fail_submit=%u capture_every=%u\n",test_fail_submit,b.capture_every);
    fprintf(b.log,"gpu-wine broker local_memory=%d local_offer=%llu local_floor=%llu host_limit=%llu\n",local,
        (unsigned long long)(local ? local_offer() : 0),(unsigned long long)kLocalFloor,(unsigned long long)PW_GPU_LIMIT);
    b.holder=b.holder_pid=0;b.switches=b.hidden=0;b.compositor_stop=false;
    if(!b.frame_copy)b.frame_copy=static_cast<uint8_t *>(malloc(size_t(PW_FRAME_WIDTH)*PW_FRAME_HEIGHT*4));
    b.compositing=b.frame_copy && !pthread_create(&b.compositor,nullptr,compositor_main,nullptr);
    fprintf(b.log,"gpu-desktop compositor=%d\n",b.compositing);
    b.started=true;return true;
}
int ps5_gpu_broker_open(int *cookie)
{
    auto &b=broker_state;if(!b.started || !cookie) { errno=EINVAL;return -1; }
    pthread_mutex_lock(&b.lock);
    Client *c=nullptr;for(auto &slot:b.clients)if(slot.socket<0) { c=&slot;break; }
    int pair[2],fd=-1;
    if(c && b.next_cookie<INT32_MAX && !ps5_gpu_packet_socketpair(pair,sizeof(PwGpuMessage))) {
        reset(*c);c->socket=pair[0];c->cookie=++b.next_cookie;*cookie=c->cookie;fd=pair[1];
        pthread_attr_t attributes;pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,512u<<10);
        c->serving=!pthread_create(&c->server,&attributes,client_main,c);
        pthread_attr_destroy(&attributes);
        if(!c->serving) { close(pair[0]);close(pair[1]);reset(*c);fd=-1;errno=EAGAIN; }
        // Before the child exists: with spawns serialized, the next INIT's figure less this is the child's footprint.
        else fprintf(b.log,"gpu-wine open cookie=%d flexible=%zu\n",c->cookie,flexible_now());
    } else errno=EAGAIN;
    pthread_mutex_unlock(&b.lock);return fd;
}
void ps5_gpu_broker_close(int cookie,int service)
{
    auto &b=broker_state;
    // EOF alone is insufficient: CPU mappings can still belong to a live helper.
    if(service!=0) {
        for(unsigned n=0;n<200 && !absent(service);++n)sceKernelUsleep(10000);
        if(!absent(service))give_up(b.log,"wine_gpu_service_still_live");
    }
    // The client's thread ends at the closed socket of the absent helper (or at the next poll, once marked ended).
    pthread_mutex_lock(&b.lock);
    Client *closing=nullptr;
    for(auto &c:b.clients)if(c.socket>=0 && c.cookie==cookie) { closing=&c;c.ended=true;break; }
    const bool serving=closing && closing->serving;const pthread_t server=serving ? closing->server : pthread_t{};
    pthread_mutex_unlock(&b.lock);
    if(serving)pthread_join(server,nullptr);
    pthread_mutex_lock(&b.lock);
    for(auto &c:b.clients)if(c.socket>=0 && c.cookie==cookie) {
        // The helper is gone, but its last submissions may not be: nothing it used is released before they are.
        if(!wedged && !drain(5000000000ull))give_up(b.log,"wine_gpu_pending_on_close");
        // Logged before the release below clears the accounts.
        fprintf(b.log,"gpu-wine cookie=%d pid=%d closed=1 absent=1 frames=%u submissions=%u backing_released=%llu "
            "local_released=%llu local_peak=%llu flexible=%zu\n",cookie,c.pid,c.presentations,c.submissions,(unsigned long long)c.allocated,
            (unsigned long long)c.local,(unsigned long long)c.local_peak,flexible_now());fsync(fileno(b.log));
        if(!wedged)for(auto &a:c.buffers)if(a.token)release(a);
        close(c.socket);reset(c);break;
    }
    pthread_mutex_unlock(&b.lock);
}
bool ps5_gpu_broker_stop()
{
    auto &b=broker_state;if(!b.started)return false;
    pthread_mutex_lock(&b.lock);
    for(auto &c:b.clients)if(c.socket>=0) { pthread_mutex_unlock(&b.lock);return false; }
    b.stopping=true;pthread_mutex_unlock(&b.lock);
    if(!wedged && !drain(5000000000ull))give_up(b.log,"wine_gpu_pending_on_stop");
    fprintf(b.log,"gpu-wine fences issued=%u drains=%llu state_drains=%llu kicks=%llu kick_us=%llu wedged=%d\n",fences.issued,
        (unsigned long long)fences.drains,(unsigned long long)fences.state_drains,(unsigned long long)fences.kicks,
        (unsigned long long)fences.kick_ns/1000,wedged);
    // Wedged, the fence page, staging and the reserved ranges (with whatever is mapped in them) stay as they are.
    if(!wedged) { fences_reset();host_ranges_release(b.log); }
    if(b.compositing) {
        pthread_mutex_lock(&b.display_lock);b.compositor_stop=true;pthread_mutex_unlock(&b.display_lock);
        pthread_join(b.compositor,nullptr);b.compositing=false;
    }
    pthread_mutex_lock(&b.display_lock);
    for(auto &f:b.frames)if(f.page) { munmap(f.page,PW_FRAME_BYTES);f=Broker::FrameSource{}; }
    fprintf(b.log,"gpu-desktop switches=%u hidden_presents=%u\n",b.switches,b.hidden);
    pthread_mutex_unlock(&b.display_lock);
    if(!b.display.close(b.log))give_up(b.log,"wine_gpu_display_teardown_failed");
    fprintf(b.log,"gpu-wine stopped=1 clients_closed=%d flips=%llu flexible_min=%llu direct_min=%llu host_peak=%llu local_peak=%llu\n",
        b.next_cookie,(unsigned long long)b.display.flips,(unsigned long long)(b.flexible_min==SIZE_MAX ? 0 : b.flexible_min),
        (unsigned long long)(b.direct_min==UINT64_MAX ? 0 : b.direct_min),(unsigned long long)b.host_peak,(unsigned long long)b.local_peak);
    b.log_pipe.close();fclose(b.log_file);b.log=b.log_file=nullptr;b.started=false;return true;
}

bool ps5_gpu_broker_warm(FILE *out)
{
    radv_ps5_memory memory{};
    if(!radv_ps5_memory_alloc(16384,16384,false,&memory))return false;
    memset(memory.cpu,0,memory.bytes);
    auto *marker=reinterpret_cast<volatile uint32_t *>(memory.cpu+1024);
    ps5_gpu_completion_packet(reinterpret_cast<uint32_t *>(memory.cpu),(uint64_t)marker,77);
    int result=radv_ps5_submit(reinterpret_cast<uint32_t *>(memory.cpu),8,marker,77);
    if(!result) {
        const uint64_t deadline=radv_ps5_now_ns()+5000000000ull;
        while(*marker!=77 && radv_ps5_now_ns()<deadline)sceKernelUsleep(1000);
        if(*marker!=77) {
            fprintf(out,"gpu-broker unsafe_to_close=1 warmup_timeout=1\n");fsync(fileno(out));
            for(;;)sceKernelUsleep(100000);
        }
    }
    fprintf(out,"gpu-broker warmup_submit=%d gpu_value=%u\n",result,*marker);fsync(fileno(out));
    radv_ps5_memory_free(&memory);
    return !result;
}
