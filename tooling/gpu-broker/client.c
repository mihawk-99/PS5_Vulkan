/* Copyright (C) 2026 Mihawk-99 */
/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Experimental offscreen RADV platform. All GPU operations belong to the title. */
#include "radv_ps5_platform.h"
#include "protocol.h"
#include "packet_io.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <ps5platform/kernel.h>
extern int sceKernelUsleep(unsigned);
static int broker=-1,broken;
static uint32_t sequence;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t allocated,local_allocated;
/* The broker's answer to INIT: device-local bytes (the owner's direct memory, never mapped here) and host-visible
 * bytes (shared memory). No local bytes: one unified host-visible pool, as before. */
static uint64_t local_limit,host_limit=PW_GPU_LIMIT;
/* This process reserved the broker's host ranges before anything else could map there (set by the application before binding): buffers are mapped over that reservation, and freed back to it. Without them no host-visible buffer is mapped. */
static int ranges_reserved;
/* A host-visible buffer at `at`: the broker's direct memory when it names an offset (dmem_plus_one, the offset and one), else its
 * shared-memory object (fd). */
static void *map_host_at(void *at,uint64_t bytes,int fd,uint64_t dmem_plus_one)
{
    if(dmem_plus_one) {
        void *got=at;
        const int status=sceKernelMapDirectMemory(&got,(size_t)bytes,0x33,PS5_KERNEL_MAP_FIXED,(int64_t)(dmem_plus_one-1),16384);
        if(status || got!=at) {
            fprintf(stderr,"gpu-client: direct memory %llu at %p for %llu bytes: status %#x, mapped at %p\n",(unsigned long long)(dmem_plus_one-1),at,(unsigned long long)bytes,(unsigned)status,got);
            return MAP_FAILED;
        }
        return got;
    }
    return mmap(at,bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0);
}
extern int sceKernelVirtualQuery(const void *,int,void *,size_t);
/* 1 when [at, +bytes) is bare reservation end to end (protection 0, no hole). A fixed mapping goes only there: never over
 * a mapping this process's application made (PS5_Proton's Wine keeps its memory in reservations of its own, and tearing
 * down wired GPU memory hung the console, "unwire timeout"), nor into a hole another mapping may have taken. One query
 * per entry (NEXT: the entry holding the address, or the one after it). */
static int bare(void *at,uint64_t bytes)
{
    static unsigned refusals;
    struct { void *start,*end; uint64_t offset; int32_t protection,type; uint8_t flags; char name[32]; uint8_t padding[7]; } entry={0};
    _Static_assert(sizeof(entry)==72,"libkernel query record");
    for(uintptr_t cursor=(uintptr_t)at;cursor<(uintptr_t)at+bytes;cursor=(uintptr_t)entry.end) {
        const int status=sceKernelVirtualQuery((void *)cursor,1,&entry,sizeof(entry));
        if(status || (uintptr_t)entry.start>cursor || (uintptr_t)entry.end<=cursor || entry.protection) {
            /* Otherwise a refused buffer reads as an ordinary out-of-memory in DXVK or Zink, and the owner logs nothing. */
            if(__atomic_fetch_add(&refusals,1,__ATOMIC_RELAXED)<8)
                fprintf(stderr,"gpu-client: %p +%llu not bare at %#llx: query %#x, entry %p-%p protection %#x\n",at,
                        (unsigned long long)bytes,(unsigned long long)cursor,(unsigned)status,entry.start,entry.end,
                        (unsigned)entry.protection);
            return 0;
        }
    }
    return 1;
}
#define PS5_GPU_LOCAL_MEMORY (-2)

int ps5_gpu_client_bind(int fd, bool reserved);
int ps5_gpu_client_window_swapchain(const void *,unsigned,unsigned,unsigned,unsigned,uint32_t,uint64_t);
int ps5_gpu_client_done(int result,uint64_t images,uint64_t hash);
int ps5_gpu_client_image(unsigned cycle,unsigned pixels,uint64_t hash);
int ps5_gpu_client_coherence(unsigned slot,unsigned words,uint64_t hash);
int ps5_gpu_client_timing(unsigned slot,unsigned count,uint64_t ns);
void pw_module_lookup(void *(*lookup)(void *,const char *));
/* PS5_Proton's helper loader ends the process 5 s after its GPU reason is stamped (ntdll's pw_ps5_child_doomed(1),
 * PW_CHILD_DOOM_GPU). The module loader that opens this module in a helper hands it its own symbol lookup: this
 * module's dlsym is libkernel's, and the firmware's lookup finds no application module's export (sceKernelDlsym on
 * 12.02). bind resolves the stamp with it, outside the rpc lock. None (a title's process, a host build): no stamp. */
static void *(*module_lookup)(void *,const char *);
static void (*doomed)(unsigned);
void pw_module_lookup(void *(*lookup)(void *,const char *)) { module_lookup=lookup; }
/* The client gives the broker up for good: from here every call fails at once and the program sees its device lost.
 * Only a loss does this: no answer in 15 s (the broker stuck under its lock, as when a 2026-10-15 run froze the
 * console), the connection gone, a reply that is not the answer, or a free the broker did not take back. A call the
 * broker answered with a refusal is none: in gate-hang-a-2026-10-15-01 it answered a helper's first two window presents
 * with status 70 (ESTALE), the program presented on (39 frames in 2.1 s) and ended by itself, and a game that rebuilds
 * its window, swapchain or device goes on the same way. The first transition alone says why and stamps
 * the GPU reason (one compare-exchange, safe under the rpc lock). */
static void broker_lost(const char *why,unsigned operation,unsigned sequence)
{
    if(__atomic_exchange_n(&broken,1,__ATOMIC_RELAXED))return;
    fprintf(stderr,"gpu-client: the broker is given up on: %s (operation %u sequence %u)\n",why,operation,sequence);
    if(doomed)doomed(1 /* PW_CHILD_DOOM_GPU */);
}
/* For SUBMIT round trips: the time spent taking the lock, sending, and waiting for the reply to be readable. */
static uint64_t rpc_lock_ns,rpc_send_ns,rpc_wait_ns;
void ps5_gpu_client_rpc_split(uint64_t split[3])
{
    split[0]=__atomic_load_n(&rpc_lock_ns,__ATOMIC_RELAXED);split[1]=__atomic_load_n(&rpc_send_ns,__ATOMIC_RELAXED);
    split[2]=__atomic_load_n(&rpc_wait_ns,__ATOMIC_RELAXED);
}
/* One call on a connection to the owner: the main one (rpc), or the present link (present_rpc), each one call at a time
 * under its own lock and with its own sequence. */
static int rpc_on(int socket,pthread_mutex_t *link_lock,uint32_t *link_sequence,PwGpuMessage *message,int *descriptor)
{
    int fds[2]={-1,-1};unsigned count=0;
    const bool timed=message->operation==PW_GPU_SUBMIT;
    const uint64_t t0=timed ? radv_ps5_now_ns() : 0;
    pthread_mutex_lock(link_lock);
    const uint64_t t1=timed ? radv_ps5_now_ns() : 0;
    message->magic=PW_GPU_MAGIC;message->sequence=++*link_sequence;
    const uint32_t expected=message->sequence,operation=message->operation;
    struct pollfd p={socket,POLLIN,0};
    const int sent=!__atomic_load_n(&broken,__ATOMIC_RELAXED) && socket>=0 && !ps5_gpu_packet_send(socket,message,sizeof(*message),NULL,0);
    const uint64_t t2=timed ? radv_ps5_now_ns() : 0;
    /* The owner answers in tens of microseconds, so the reply is looked for by peeking: continuously for 2 ms, then
     * after short polls, which bound the wait for one that takes longer. Adopted while poll was suspected of waking
     * late (graphics-child-gpu-async-07 to -13); those stalls were the owner's disk writes under its lock (async-18). */
    int readable=0;const char *lost=sent ? "a reply that is not the answer" : "the send failed";
    if(sent) {
        const uint64_t start=radv_ps5_now_ns(),spin_until=start+2000000,give_up=start+15000000000ull;
        char peek=0;
        for(;;) {
            const ssize_t found=recv(socket,&peek,1,MSG_PEEK|MSG_DONTWAIT);
            if(found==1) { readable=1;break; }
            if(found==0 || (found<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) {
                lost=found ? "the connection failed" : "the connection closed";break;
            }
            const uint64_t now=radv_ps5_now_ns();
            if(now>=give_up) { lost="no answer in 15 s";break; }
            /* Yield, not pause: the owner's thread serving this client may share its CPU. */
            if(now<spin_until)sched_yield();
            else poll(&p,1,1);
        }
    }
    if(timed) {
        const uint64_t t3=radv_ps5_now_ns();
        __atomic_add_fetch(&rpc_lock_ns,t1-t0,__ATOMIC_RELAXED);__atomic_add_fetch(&rpc_send_ns,t2-t1,__ATOMIC_RELAXED);
        __atomic_add_fetch(&rpc_wait_ns,t3-t2,__ATOMIC_RELAXED);
    }
    int okay=readable &&
        ps5_gpu_packet_recv(socket,message,sizeof(*message),fds,&count)==sizeof(*message) &&
        message->magic==PW_GPU_MAGIC && message->sequence==expected && message->operation==operation &&
        (count==(unsigned)(descriptor && !message->status) ||
         (operation==PW_GPU_ALLOC && descriptor && !message->status && !count && message->argument[2]));
    if(okay && descriptor) *descriptor=count ? fds[0] : -1;
    else for(unsigned i=0;i<count;++i)close(fds[i]);
    if(!okay)broker_lost(lost,operation,expected);
    pthread_mutex_unlock(link_lock);
    return okay ? message->status : EIO;
}
static int rpc(PwGpuMessage *message,int *descriptor) { return rpc_on(broker,&lock,&sequence,message,descriptor); }
/* The present link (protocol.h, PW_GPU_PRESENT_LINK), with WINE_PS5_PRESENT_LINK=1 in the environment: asked for at the
 * first present, then every present and its timing go over it, so the submissions on the main connection never wait
 * behind a present the display is copying. Refused or not asked for: the main connection, as before. */
static int presenter=-1,present_link;   /* present_link: 0 not asked yet, 1 open, -1 not used */
static uint32_t present_sequence;
static pthread_mutex_t present_lock=PTHREAD_MUTEX_INITIALIZER;
static int present_rpc(PwGpuMessage *message)
{
    if(!__atomic_load_n(&present_link,__ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&present_lock);
        if(!present_link) {
            const char *wanted=getenv("WINE_PS5_PRESENT_LINK");
            PwGpuMessage link={.operation=PW_GPU_PRESENT_LINK};int fd=-1;
            const int status=wanted && !strcmp(wanted,"1") ? rpc(&link,&fd) : ENOTSUP;
            presenter=!status ? fd : -1;
            if(wanted)fprintf(stderr,"gpu-client present link=%d status=%d\n",presenter>=0,status);
            __atomic_store_n(&present_link,presenter>=0 ? 1 : -1,__ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&present_lock);
    }
    return __atomic_load_n(&present_link,__ATOMIC_ACQUIRE)>0 ? rpc_on(presenter,&present_lock,&present_sequence,message,NULL)
                                                             : rpc(message,NULL);
}
int ps5_gpu_client_bind(int fd, bool reserved)
{
    if(broker>=0 || fd<0)return -1;
    /* NULL: every module loaded, as Wine's dlsym(RTLD_DEFAULT). */
    if(module_lookup)doomed=(void (*)(unsigned))module_lookup(NULL,"pw_ps5_child_doomed");
    broker=fd;
    ranges_reserved=reserved;
    /* argument[2]: whether this process reserved the host ranges, for the owner's log. */
    PwGpuMessage request={.operation=PW_GPU_INIT,.argument={(uint64_t)getpid(),0,(uint64_t)ranges_reserved}};
    const int status=rpc(&request,NULL);
    if(!status) {
        if(request.argument[2]<=PW_GPU_LOCAL_MAX && !(request.argument[2]&16383))local_limit=request.argument[2];
        if(request.argument[3] && request.argument[3]<=PW_GPU_HOST_LIMIT_MAX && !(request.argument[3]&16383))host_limit=request.argument[3];
    }
    return status;
}
uint64_t ps5_gpu_client_local_limit(void) { return local_limit; }
uint64_t ps5_gpu_client_local_allocated(void) { return __atomic_load_n(&local_allocated,__ATOMIC_RELAXED); }
/* The owner's device-local room at its last ALLOC or FREE reply, plus one (patch 0042; 0: not said yet). */
static uint64_t local_room_plus_one;
static void room_from(const PwGpuMessage *message)
{
    if(message->argument[3])__atomic_store_n(&local_room_plus_one,message->argument[3],__ATOMIC_RELAXED);
}
/* What this client's device-local budget still has: its heap less what it holds, and never more than the owner's room,
 * which the game's own heaps and every other program draw from too. */
uint64_t ps5_gpu_client_local_available(void)
{
    const uint64_t used=__atomic_load_n(&local_allocated,__ATOMIC_RELAXED);
    uint64_t left=used<local_limit ? local_limit-used : 0;
    const uint64_t room=__atomic_load_n(&local_room_plus_one,__ATOMIC_RELAXED);
    if(room && room-1<left)left=room-1;
    return left;
}
/* Device-local memory: the owner allocates and maps it for the GPU; this process gets only its address. */
/* Every allocation this client refuses, said once with its reason (PS5_Proton patch 0040, diagnostic): a game's
 * E_OUTOFMEMORY otherwise names no layer. The first 32, then one in 256. */
static unsigned long refusals;
static void refused(const char *kind,const char *why,uint64_t bytes,uint64_t alignment,uint64_t limit)
{
    const unsigned long n=__atomic_add_fetch(&refusals,1,__ATOMIC_RELAXED);
    if(n<=32 || !(n%256))
        fprintf(stderr,"gpu-client refused %s: %s bytes=%llu alignment=%llu limit=%llu (%lu refused)\n",kind,why,
                (unsigned long long)bytes,(unsigned long long)alignment,(unsigned long long)limit,n);
}
bool ps5_gpu_client_alloc_local(uint64_t bytes,uint64_t alignment,bool window32,bool zero,struct radv_ps5_memory *out)
{
    if(!local_limit || !bytes || bytes>local_limit || alignment>PW_GPU_ALIGNMENT_MAX || (alignment && (alignment&(alignment-1)))) {
        refused("local",!local_limit ? "no local memory" : bytes>local_limit ? "over the limit" :
                alignment>PW_GPU_ALIGNMENT_MAX ? "alignment over 2 MiB" : "bad size or alignment",bytes,alignment,local_limit);
        return false;
    }
    PwGpuMessage message={.operation=PW_GPU_ALLOC,.bytes=bytes,.argument={alignment,window32,PW_GPU_ALLOC_LOCAL|(zero ? PW_GPU_ALLOC_ZERO : 0)}};
    const int status=rpc(&message,NULL);
    room_from(&message);
    if(status) { refused("local","the broker said no",bytes,alignment,local_limit);return false; }
    const bool okay=message.bytes>=bytes && message.bytes<=local_limit && !(message.bytes&16383) && !(message.address&16383) &&
        (!alignment || !(message.address&(alignment-1))) && message.argument[0]>0 && message.argument[0]<=UINT32_MAX &&
        message.address>=RADV_PS5_WINDOW_BASE && message.address<=RADV_PS5_GPU_ADDRESS_LIMIT-message.bytes &&
        (!window32 || pw_gpu_contains(RADV_PS5_WINDOW_BASE,RADV_PS5_WINDOW_BYTES,message.address,message.bytes));
    if(!okay) {
        refused("local","the broker's answer did not fit",bytes,alignment,local_limit);
        message.operation=PW_GPU_FREE;rpc(&message,NULL);return false;
    }
    *out=(struct radv_ps5_memory){.cpu=(void *)(uintptr_t)message.address,.bytes=message.bytes,
                                  .physical=PS5_GPU_LOCAL_MEMORY,.granule=(uint32_t)message.argument[0]};
    __atomic_add_fetch(&local_allocated,message.bytes,__ATOMIC_RELAXED);
    return true;
}
int ps5_gpu_client_image(unsigned cycle,unsigned pixels,uint64_t hash)
{
    PwGpuMessage message={.operation=PW_GPU_IMAGE,.argument={cycle,pixels,hash,
#ifdef PS5_GPU_DRAW
        1
#else
        0
#endif
    }};return rpc(&message,NULL);
}
/* A no-op round trip to the owner. */
int ps5_gpu_client_ping(void)
{
    PwGpuMessage message={.operation=PW_GPU_IMAGE,.argument={0,0,0,4}};return rpc(&message,NULL);
}
/* A timing: slot = cycle*8 + 0 serial, 1 two in flight, 2 submit calls, 3 fence waits, 4 1 ms sleeps, 5 no-op round
 * trips; the count and the elapsed nanoseconds. */
int ps5_gpu_client_timing(unsigned slot,unsigned count,uint64_t ns)
{
    PwGpuMessage message={.operation=PW_GPU_IMAGE,.argument={slot,count,ns,3}};return rpc(&message,NULL);
}
/* A coherence check's result (proton_gpu_vulkan_probe.c): slot = cycle*2+kind, words matched of 4,096. */
int ps5_gpu_client_coherence(unsigned slot,unsigned words,uint64_t hash)
{
    PwGpuMessage message={.operation=PW_GPU_IMAGE,.argument={slot,words,hash,2}};return rpc(&message,NULL);
}
int ps5_gpu_client_present(const void *pixels,unsigned width,unsigned height,unsigned pitch,unsigned cycle)
{
    radv_ps5_cpu_flush(pixels,(size_t)pitch*height);
    PwGpuMessage message={.operation=PW_GPU_PRESENT,.address=(uint64_t)pixels,.bytes=(uint64_t)pitch*height,
                         .argument={width,height,pitch,cycle}};
    return rpc(&message,NULL);
}
/* The client's share of its frames' time (PS5_Proton's src/pw_gpu_timing.h), sent to the owner about once a second
 * (PW_GPU_TIMING): the WSI's waits for its frames' fences (ps5_gpu_client_present_waited), the frames' cache flushes
 * and the presents' round trips, and the submissions' round trips (submit_ns, submit_count). */
static uint64_t submit_ns,submit_count;   /* every submission's round trip, and how many (radv_ps5_submit) */
static uint64_t timing_presents,timing_fence_ns,timing_flush_ns,timing_present_ns,timing_sent_ns,timing_submit_ns,timing_submits;
void ps5_gpu_client_present_waited(uint64_t ns) { __atomic_add_fetch(&timing_fence_ns,ns,__ATOMIC_RELAXED); }
static int present_timed(PwGpuMessage *message,const void *pixels,size_t bytes)
{
    const uint64_t start=radv_ps5_now_ns();
    radv_ps5_cpu_flush(pixels,bytes);
    const uint64_t flushed=radv_ps5_now_ns();
    const int status=present_rpc(message);
    const uint64_t end=radv_ps5_now_ns();
    __atomic_add_fetch(&timing_presents,1,__ATOMIC_RELAXED);__atomic_add_fetch(&timing_flush_ns,flushed-start,__ATOMIC_RELAXED);
    __atomic_add_fetch(&timing_present_ns,end-flushed,__ATOMIC_RELAXED);
    uint64_t sent=__atomic_load_n(&timing_sent_ns,__ATOMIC_RELAXED);
    if(end-sent>=1000000000ull && __atomic_compare_exchange_n(&timing_sent_ns,&sent,end,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {
        const uint64_t submit=__atomic_load_n(&submit_ns,__ATOMIC_RELAXED),submits=__atomic_load_n(&submit_count,__ATOMIC_RELAXED);
        const uint64_t submit_before=__atomic_exchange_n(&timing_submit_ns,submit,__ATOMIC_RELAXED);
        const uint64_t submits_before=__atomic_exchange_n(&timing_submits,submits,__ATOMIC_RELAXED);
        PwGpuMessage timing={.operation=PW_GPU_TIMING,.address=submit-submit_before,.bytes=submits-submits_before,
                            .argument={__atomic_exchange_n(&timing_presents,0,__ATOMIC_RELAXED),
                                       __atomic_exchange_n(&timing_fence_ns,0,__ATOMIC_RELAXED),
                                       __atomic_exchange_n(&timing_flush_ns,0,__ATOMIC_RELAXED),
                                       __atomic_exchange_n(&timing_present_ns,0,__ATOMIC_RELAXED)}};
        /* Its status is not the present's: an owner without the operation refuses it, and nothing is lost but the line. */
        (void)present_rpc(&timing);
    }
    return status;
}
int ps5_gpu_client_swapchain(const void *pixels,unsigned width,unsigned height,unsigned pitch,unsigned bgra)
{
    PwGpuMessage message={.operation=PW_GPU_SWAPCHAIN,.address=(uint64_t)pixels,.bytes=(uint64_t)pitch*height,
                         .argument={width,height,pitch,bgra}};
    if(!pw_gpu_display_image(&message))return EINVAL;
    return present_timed(&message,pixels,(size_t)pitch*height);
}
int ps5_gpu_client_window_swapchain(const void *pixels,unsigned width,unsigned height,unsigned stride,unsigned bgra,
                                  uint32_t hwnd,uint64_t generation)
{
    PwGpuMessage message={.operation=PW_GPU_WINDOW_SWAPCHAIN,.address=(uint64_t)pixels,.bytes=(uint64_t)stride*height,
                         .argument={(uint64_t)width|((uint64_t)height<<32),
                                    (uint64_t)stride|((uint64_t)hwnd<<32),generation,bgra}};
    if(!pw_gpu_window_image(&message))return EINVAL;
    return present_timed(&message,pixels,(size_t)stride*height);
}
int ps5_gpu_client_done(int result,uint64_t images,uint64_t hash)
{
    PwGpuMessage request={.operation=PW_GPU_DONE,.argument={(uint64_t)result,images,hash,__atomic_load_n(&allocated,__ATOMIC_RELAXED)}};
    return rpc(&request,NULL);
}
bool radv_ps5_platform_init(void) { return broker>=0 && !__atomic_load_n(&broken,__ATOMIC_RELAXED); }
bool radv_ps5_platform_runs_gpu(void) { return true; }
bool radv_ps5_window_replayable(void) { return false; }
uint64_t radv_ps5_memory_pool_bytes(void) { return host_limit; }
uint64_t radv_ps5_memory_available_bytes(void)
{
    const uint64_t used=__atomic_load_n(&allocated,__ATOMIC_RELAXED);
    return used<host_limit ? host_limit-used : 0;
}
/* Sparse ranges come from the owner's device-memory region (patch 0041), 256 GiB shared with the owner's own. */
uint64_t radv_ps5_vrange_space_bytes(void) { return UINT64_C(256)<<30; }
uint64_t radv_ps5_now_ns(void)
{
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
void radv_ps5_sleep_us(unsigned us) { sceKernelUsleep(us); }
void radv_ps5_cpu_flush(const void *address,size_t bytes)
{
    if(!bytes)return;
    __asm__ volatile("mfence");
    for(uintptr_t p=(uintptr_t)address&~63ull;p<(uintptr_t)address+bytes;p+=64)
        __asm__ volatile("clflushopt %0" : "+m"(*(volatile char *)p));
    __asm__ volatile("mfence");
}
/* The memory behind each host-visible allocation, kept for a placed mapping (VK_EXT_map_memory_placed): a 32-bit
 * Windows program under WoW64 maps its memory again below 4 GiB (PS5_Proton's docs/IA32_DESIGN.md, M4 step 3). The
 * direct-memory offset, or the descriptor of the owner's shared memory (fd, -1 otherwise). One per allocation, dropped
 * (the descriptor closed) when it is freed, and only when the application asked (ps5_gpu_client_keep_sources): a
 * payload process has a few hundred descriptors (636 measured). Over a reservation shared memory reads as the
 * reservation, so PS5_Proton's Wine keeps the range it lends for a placed mapping as foreign by record (Wine patch
 * 1130); graphics patch 0014 had refused to place it, and a 32-bit program's maps failed once direct memory ran out
 * (host-dmem-fallback64, v2-backend-gate-2026-10-15). */
struct shared_source { uintptr_t address; int fd; uint64_t dmem_plus_one; };
static int keep_sources;
void ps5_gpu_client_keep_sources(void) { __atomic_store_n(&keep_sources,1,__ATOMIC_RELEASE); }
/* ps5_gpu_client_in_owner: set once, before binding; never by a client in a process of its own. */
static int in_owner;
void ps5_gpu_client_in_owner(void) { __atomic_store_n(&in_owner,1,__ATOMIC_RELEASE); }
static struct shared_source *sources;
static size_t source_count,source_capacity;
static pthread_mutex_t sources_lock=PTHREAD_MUTEX_INITIALIZER;
static bool source_add(uintptr_t address,int fd,uint64_t dmem_plus_one)
{
    pthread_mutex_lock(&sources_lock);
    if(source_count==source_capacity) {
        const size_t capacity=source_capacity ? source_capacity*2 : 64;
        struct shared_source *grown=realloc(sources,capacity*sizeof(*grown));
        if(!grown) { pthread_mutex_unlock(&sources_lock);return false; }
        sources=grown;source_capacity=capacity;
    }
    sources[source_count++]=(struct shared_source){address,fd,dmem_plus_one};
    pthread_mutex_unlock(&sources_lock);
    return true;
}
/* The source of the allocation at address: 1 with it, or 0 when it has none; take removes it (the caller closes fd). */
static bool source_find(uintptr_t address,bool take,struct shared_source *found)
{
    bool okay=false;
    pthread_mutex_lock(&sources_lock);
    for(size_t i=0;i<source_count;++i)
        if(sources[i].address==address) {
            *found=sources[i];okay=true;
            if(take)sources[i]=sources[--source_count];
            break;
        }
    pthread_mutex_unlock(&sources_lock);
    return okay;
}
bool radv_ps5_memory_alloc(uint64_t bytes,uint64_t alignment,bool window32,struct radv_ps5_memory *out)
{
    if(!bytes || bytes>host_limit || alignment>PW_GPU_ALIGNMENT_MAX || (alignment && (alignment&(alignment-1)))) {
        refused("host",bytes>host_limit ? "over the limit" : alignment>PW_GPU_ALIGNMENT_MAX ? "alignment over 2 MiB" :
                "bad size or alignment",bytes,alignment,host_limit);
        return false;
    }
    PwGpuMessage message={.operation=PW_GPU_ALLOC,.bytes=bytes,.argument={alignment,window32}};
    int fd=-1;
    const int status=rpc(&message,&fd);
    room_from(&message);
    if(status) { refused("host","the broker said no",bytes,alignment,host_limit);return false; }
    void *at=(void *)(uintptr_t)message.address;
    bool okay=message.bytes>=bytes && message.bytes<=host_limit && !(message.bytes&16383) &&
        !(message.address&16383) && (!alignment || !(message.address&(alignment-1))) &&
        message.argument[0]>0 && message.argument[0]<=UINT32_MAX && message.address>=RADV_PS5_WINDOW_BASE && message.address<=RADV_PS5_GPU_ADDRESS_LIMIT-message.bytes &&
        (!window32 || pw_gpu_contains(RADV_PS5_WINDOW_BASE,RADV_PS5_WINDOW_BYTES,message.address,message.bytes));
    /* Only over this process's own reservation of a host range, still bare: an address the owner chose anywhere else
     * was taken where the kernel found a hole, which may lie inside the application's own reservations. */
    /* In the owner's process the owner has mapped the buffer at `at` already (its allocate): that mapping is this
     * client's too. */
    const int owned=okay && __atomic_load_n(&in_owner,__ATOMIC_ACQUIRE);
    const int ranged=okay && !owned && ranges_reserved && pw_gpu_host_range(message.address,message.bytes)>=0 &&
        bare(at,message.bytes);
    if(ranged) {
        /* Over this process's own reservation of the range: a fixed mapping replaces it. */
        void *mapped=map_host_at(at,message.bytes,fd,message.argument[2]);
        okay=mapped==at;
        if(!okay) { void *again=at;sceKernelReserveVirtualRange(&again,message.bytes,PW_GPU_RESERVE_FIXED,16384); }
    } else if(!owned)okay=false;
    /* Kept for a placed mapping of this memory (radv_ps5_memory_map_at): its direct-memory offset, or its shared
     * memory's descriptor; dropped when it is freed. */
    const bool kept=okay && __atomic_load_n(&keep_sources,__ATOMIC_ACQUIRE) &&
        source_add((uintptr_t)at,message.argument[2] ? -1 : fd,message.argument[2]);
    if(fd>=0 && (!kept || message.argument[2]))close(fd);
    if(!okay) {
        refused("host",owned ? "the broker's answer did not fit" : "no bare reservation for the mapping",bytes,alignment,host_limit);
        message.operation=PW_GPU_FREE;rpc(&message,NULL);return false;
    }
    *out=(struct radv_ps5_memory){.cpu=at,.bytes=message.bytes,.physical=-1,.granule=(uint32_t)message.argument[0]};
    __atomic_add_fetch(&allocated,message.bytes,__ATOMIC_RELAXED);
    return true;
}
void radv_ps5_memory_free(struct radv_ps5_memory *memory)
{
    if(!memory->cpu)return;
    PwGpuMessage message={.operation=PW_GPU_FREE,.address=(uint64_t)memory->cpu,.bytes=memory->bytes,
                         .argument={memory->granule}};
    /* The owner gives the range back when it answers, and the next ALLOC of any thread here may get it at once: nothing
     * here may map it, or still lay a reservation over it, by then (a lay after FREE tore down that thread's new
     * buffer). In a reserved range the mapping goes back to a reservation (laid over it, it replaces it). */
    /* In the owner's process the mapping is the owner's: its free unmaps it. */
    if(memory->physical!=PS5_GPU_LOCAL_MEMORY && !__atomic_load_n(&in_owner,__ATOMIC_ACQUIRE)) {
        void *at=memory->cpu;
        if(!(ranges_reserved && pw_gpu_host_range((uint64_t)(uintptr_t)at,memory->bytes)>=0 &&
             !sceKernelReserveVirtualRange(&at,memory->bytes,PW_GPU_RESERVE_FIXED,16384) && at==memory->cpu))
            sceKernelMunmap(memory->cpu,memory->bytes);
        struct shared_source source;
        if(source_find((uintptr_t)memory->cpu,true,&source) && source.fd>=0)close(source.fd);
    }
    /* Broker submissions complete before acknowledgment; no GPU references remain. */
    if(rpc(&message,NULL)) { broker_lost("a free it did not take back",PW_GPU_FREE,message.sequence);return; }
    room_from(&message);
    if(memory->physical==PS5_GPU_LOCAL_MEMORY)__atomic_sub_fetch(&local_allocated,memory->bytes,__ATOMIC_RELAXED);
    else __atomic_sub_fetch(&allocated,memory->bytes,__ATOMIC_RELAXED);
    memset(memory,0,sizeof(*memory));
}
static uint64_t reply_transit_ns;
/* The round trips radv_ps5_submit has made and the time they took: what submission costs this process. */
void ps5_gpu_client_submit_stats(uint64_t *ns,uint64_t *count)
{
    *ns=__atomic_load_n(&submit_ns,__ATOMIC_RELAXED);*count=__atomic_load_n(&submit_count,__ATOMIC_RELAXED);
}
/* The part of those round trips between the owner sending its reply and this process having it, in TSC ticks. */
uint64_t ps5_gpu_client_reply_transit(void) { return __atomic_load_n(&reply_transit_ns,__ATOMIC_RELAXED); }
int radv_ps5_submit(uint32_t *words,uint32_t count,volatile uint32_t *marker,uint32_t value)
{
    radv_ps5_cpu_flush(words,(size_t)count*4);
    /* argument[2] carries the send time, and the reply's argument[3] the owner's: both processes read one monotonic
     * clock, so each leg of the round trip is measured. */
    /* The processes' monotonic clocks disagree (async-05: a transit longer than its whole round trip), so each leg is
     * measured on the CPUs' shared time-stamp counter: the send's in argument[2], the owner's reply's in argument[3]. */
    const uint64_t start=radv_ps5_now_ns();
    PwGpuMessage message={.operation=PW_GPU_SUBMIT,.address=(uint64_t)words,.bytes=(uint64_t)count*4,
                         .argument={(uint64_t)marker,value,__builtin_ia32_rdtsc()}};
    const int status=rpc(&message,NULL);
    const uint64_t end=radv_ps5_now_ns(),tsc=__builtin_ia32_rdtsc();
    __atomic_add_fetch(&submit_ns,end-start,__ATOMIC_RELAXED);__atomic_add_fetch(&submit_count,1,__ATOMIC_RELAXED);
    if(!status && message.argument[3] && message.argument[3]<=tsc)
        __atomic_add_fetch(&reply_transit_ns,tsc-message.argument[3],__ATOMIC_RELAXED);
    return status;
}
void radv_ps5_submit_times(uint64_t times[3]) { memset(times,0,3*sizeof(*times)); }
int radv_ps5_set_tess_factor_ring(uint64_t va,uint32_t size)
{
    PwGpuMessage message={.operation=PW_GPU_TESS,.address=va,.bytes=size};return rpc(&message,NULL);
}
int radv_ps5_set_hs_offchip_param(uint32_t granularity,uint32_t buffering)
{
    PwGpuMessage message={.operation=PW_GPU_OFFCHIP,.argument={granularity,buffering}};return rpc(&message,NULL);
}
/* These features are disabled in the experimental winsys overlay. */
bool radv_ps5_memory_alloc_replayable(uint64_t b,uint64_t a,bool w,uint64_t v,struct radv_ps5_memory *m)
{ (void)b;(void)a;(void)w;(void)v;(void)m;return false; }
bool radv_ps5_memory_grant_gpu(void *a,uint64_t b) { (void)a;(void)b;return false; }
/* A placed mapping (VK_EXT_map_memory_placed): the same memory mapped again for the CPU at the address the application
 * chose, over its reservation there; the GPU keeps the first mapping's address. Device-local memory has no CPU mapping
 * in this process, and so no source. */
bool radv_ps5_memory_map_at(const struct radv_ps5_memory *m,void *a)
{
    struct shared_source source;
    /* The application hands over a bare reservation there (PS5_Proton's win32u, patch 1130); anything else is refused. */
    if(m->physical==PS5_GPU_LOCAL_MEMORY || !source_find((uintptr_t)m->cpu,false,&source) || !a ||
       ((uintptr_t)a&16383) || !bare(a,m->bytes))return false;
    return map_host_at(a,m->bytes,source.fd,source.dmem_plus_one)==a;
}
void radv_ps5_memory_unmap_at(void *a,uint64_t b,bool r)
{
    if(r) {
        /* A reservation laid over the mapping replaces it, as a fixed mapping does. */
        void *at=a;
        if(!sceKernelReserveVirtualRange(&at,b,PW_GPU_RESERVE_FIXED,16384) && at==a)return;
    }
    sceKernelMunmap(a,b);
}
/* Sparse ranges (patch 0041): the owner reserves, binds and releases them in its address space, which the GPU uses;
 * this process keeps their addresses only, as it does for device-local buffers. Only device-local memory binds. */
bool radv_ps5_vrange_reserve(uint64_t b,bool w,bool r,uint64_t v,struct radv_ps5_memory *m)
{
    *m=(struct radv_ps5_memory){.physical=-1};
    PwGpuMessage message={.operation=PW_GPU_VRANGE_RESERVE,.bytes=b,.argument={w,r,v}};
    if(rpc(&message,NULL) || message.bytes<b || (message.address&16383) || message.address<RADV_PS5_WINDOW_BASE ||
       message.address>RADV_PS5_GPU_ADDRESS_LIMIT-message.bytes) {
        refused("sparse range","the broker said no",b,0,0);
        return false;
    }
    *m=(struct radv_ps5_memory){.cpu=(void *)(uintptr_t)message.address,.bytes=message.bytes,.physical=-1};
    return true;
}
void radv_ps5_vrange_release(struct radv_ps5_memory *m)
{
    if(!m->cpu)return;
    PwGpuMessage message={.operation=PW_GPU_VRANGE_RELEASE,.address=(uint64_t)(uintptr_t)m->cpu};
    rpc(&message,NULL);
    *m=(struct radv_ps5_memory){.physical=-1};
}
bool radv_ps5_vrange_bind(void *a,uint64_t b,const struct radv_ps5_memory *m,uint64_t o)
{
    if(m->physical!=PS5_GPU_LOCAL_MEMORY) { refused("sparse bind","not device-local memory",b,0,0);return false; }
    PwGpuMessage message={.operation=PW_GPU_VRANGE_BIND,.address=(uint64_t)(uintptr_t)a,.bytes=b,.argument={m->granule,o}};
    if(rpc(&message,NULL)) { refused("sparse bind","the broker said no",b,0,0);return false; }
    return true;
}
bool radv_ps5_vrange_unbind(void *a,uint64_t b)
{
    PwGpuMessage message={.operation=PW_GPU_VRANGE_UNBIND,.address=(uint64_t)(uintptr_t)a,.bytes=b};
    return !rpc(&message,NULL);
}
