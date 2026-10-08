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
/* This process reserved the broker's host ranges before anything else could map there (set by the application before binding): buffers are mapped over that reservation, and freed back to it. */
static int ranges_reserved;
#define PS5_GPU_LOCAL_MEMORY (-2)

int ps5_gpu_client_bind(int fd, bool reserved);
int ps5_gpu_client_done(int result,uint64_t images,uint64_t hash);
int ps5_gpu_client_image(unsigned cycle,unsigned pixels,uint64_t hash);
int ps5_gpu_client_coherence(unsigned slot,unsigned words,uint64_t hash);
int ps5_gpu_client_timing(unsigned slot,unsigned count,uint64_t ns);
/* For SUBMIT round trips: the time spent taking the lock, sending, and waiting for the reply to be readable. */
static uint64_t rpc_lock_ns,rpc_send_ns,rpc_wait_ns;
void ps5_gpu_client_rpc_split(uint64_t split[3])
{
    split[0]=__atomic_load_n(&rpc_lock_ns,__ATOMIC_RELAXED);split[1]=__atomic_load_n(&rpc_send_ns,__ATOMIC_RELAXED);
    split[2]=__atomic_load_n(&rpc_wait_ns,__ATOMIC_RELAXED);
}
static int rpc(PwGpuMessage *message,int *descriptor)
{
    int fds[2]={-1,-1};unsigned count=0;
    const bool timed=message->operation==PW_GPU_SUBMIT;
    const uint64_t t0=timed ? radv_ps5_now_ns() : 0;
    pthread_mutex_lock(&lock);
    const uint64_t t1=timed ? radv_ps5_now_ns() : 0;
    message->magic=PW_GPU_MAGIC;message->sequence=++sequence;
    const uint32_t expected=message->sequence,operation=message->operation;
    struct pollfd p={broker,POLLIN,0};
    const int sent=!__atomic_load_n(&broken,__ATOMIC_RELAXED) && broker>=0 && !ps5_gpu_packet_send(broker,message,sizeof(*message),NULL,0);
    const uint64_t t2=timed ? radv_ps5_now_ns() : 0;
    /* The owner answers in tens of microseconds, so the reply is looked for by peeking: continuously for 2 ms, then
     * after short polls, which bound the wait for one that takes longer. Adopted while poll was suspected of waking
     * late (graphics-child-gpu-async-07 to -13); those stalls were the owner's disk writes under its lock (async-18). */
    int readable=0;
    if(sent) {
        const uint64_t start=radv_ps5_now_ns(),spin_until=start+2000000,give_up=start+15000000000ull;
        char peek=0;
        for(;;) {
            const ssize_t found=recv(broker,&peek,1,MSG_PEEK|MSG_DONTWAIT);
            if(found==1) { readable=1;break; }
            if(found==0 || (found<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR))break;
            const uint64_t now=radv_ps5_now_ns();
            if(now>=give_up)break;
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
        ps5_gpu_packet_recv(broker,message,sizeof(*message),fds,&count)==sizeof(*message) &&
        message->magic==PW_GPU_MAGIC && message->sequence==expected && message->operation==operation &&
        count==(unsigned)(descriptor && !message->status);
    if(okay && descriptor) *descriptor=fds[0];
    else for(unsigned i=0;i<count;++i)close(fds[i]);
    if(!okay)__atomic_store_n(&broken,1,__ATOMIC_RELAXED);
    pthread_mutex_unlock(&lock);
    return okay ? message->status : EIO;
}
int ps5_gpu_client_bind(int fd, bool reserved)
{
    if(broker>=0 || fd<0)return -1;
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
/* Device-local memory: the owner allocates and maps it for the GPU; this process gets only its address. */
bool ps5_gpu_client_alloc_local(uint64_t bytes,uint64_t alignment,bool window32,bool zero,struct radv_ps5_memory *out)
{
    if(!local_limit || !bytes || bytes>local_limit || alignment>PW_GPU_ALIGNMENT_MAX || (alignment && (alignment&(alignment-1))))
        return false;
    PwGpuMessage message={.operation=PW_GPU_ALLOC,.bytes=bytes,.argument={alignment,window32,PW_GPU_ALLOC_LOCAL|(zero ? PW_GPU_ALLOC_ZERO : 0)}};
    if(rpc(&message,NULL))return false;
    const bool okay=message.bytes>=bytes && message.bytes<=local_limit && !(message.bytes&16383) && !(message.address&16383) &&
        (!alignment || !(message.address&(alignment-1))) && message.argument[0]>0 && message.argument[0]<=UINT32_MAX &&
        message.address>=RADV_PS5_WINDOW_BASE && message.address<=RADV_PS5_GPU_ADDRESS_LIMIT-message.bytes &&
        (!window32 || pw_gpu_contains(RADV_PS5_WINDOW_BASE,RADV_PS5_WINDOW_BYTES,message.address,message.bytes));
    if(!okay) {
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
int ps5_gpu_client_swapchain(const void *pixels,unsigned width,unsigned height,unsigned pitch,unsigned bgra)
{
    if(!width || width>1920 || !height || height>1080 || pitch<width*4 || pitch>1920*4 || bgra>1)return EINVAL;
    radv_ps5_cpu_flush(pixels,(size_t)pitch*height);
    PwGpuMessage message={.operation=PW_GPU_SWAPCHAIN,.address=(uint64_t)pixels,.bytes=(uint64_t)pitch*height,
                         .argument={width,height,pitch,bgra}};
    return rpc(&message,NULL);
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
uint64_t radv_ps5_vrange_space_bytes(void) { return 0; }
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
/* The shared memory behind each host-visible allocation, kept for a placed mapping (VK_EXT_map_memory_placed): a
 * 32-bit Windows program under WoW64 maps its memory again below 4 GiB, which takes the descriptor after the first
 * mapping (PS5_Proton's docs/IA32_DESIGN.md, M4 step 3). One per allocation, closed when the allocation is freed,
 * and only when the application asked (ps5_gpu_client_keep_sources): a payload process has a few hundred
 * descriptors (636 measured), which a 64-bit program with many allocations would otherwise spend. */
struct shared_source { uintptr_t address; int fd; };
static int keep_sources;
void ps5_gpu_client_keep_sources(void) { __atomic_store_n(&keep_sources,1,__ATOMIC_RELEASE); }
static struct shared_source *sources;
static size_t source_count,source_capacity;
static pthread_mutex_t sources_lock=PTHREAD_MUTEX_INITIALIZER;
static bool source_add(uintptr_t address,int fd)
{
    pthread_mutex_lock(&sources_lock);
    if(source_count==source_capacity) {
        const size_t capacity=source_capacity ? source_capacity*2 : 64;
        struct shared_source *grown=realloc(sources,capacity*sizeof(*grown));
        if(!grown) { pthread_mutex_unlock(&sources_lock);return false; }
        sources=grown;source_capacity=capacity;
    }
    sources[source_count++]=(struct shared_source){address,fd};
    pthread_mutex_unlock(&sources_lock);
    return true;
}
/* The descriptor of the allocation at address, or -1; take removes it from the table. */
static int source_find(uintptr_t address,bool take)
{
    int fd=-1;
    pthread_mutex_lock(&sources_lock);
    for(size_t i=0;i<source_count;++i)
        if(sources[i].address==address) {
            fd=sources[i].fd;
            if(take)sources[i]=sources[--source_count];
            break;
        }
    pthread_mutex_unlock(&sources_lock);
    return fd;
}
bool radv_ps5_memory_alloc(uint64_t bytes,uint64_t alignment,bool window32,struct radv_ps5_memory *out)
{
    if(!bytes || bytes>host_limit || alignment>PW_GPU_ALIGNMENT_MAX || (alignment && (alignment&(alignment-1))))return false;
    PwGpuMessage message={.operation=PW_GPU_ALLOC,.bytes=bytes,.argument={alignment,window32}};
    int fd=-1;
    if(rpc(&message,&fd))return false;
    void *at=(void *)(uintptr_t)message.address;
    bool okay=message.bytes>=bytes && message.bytes<=host_limit && !(message.bytes&16383) &&
        !(message.address&16383) && (!alignment || !(message.address&(alignment-1))) &&
        message.argument[0]>0 && message.argument[0]<=UINT32_MAX && message.address>=RADV_PS5_WINDOW_BASE && message.address<=RADV_PS5_GPU_ADDRESS_LIMIT-message.bytes &&
        (!window32 || pw_gpu_contains(RADV_PS5_WINDOW_BASE,RADV_PS5_WINDOW_BYTES,message.address,message.bytes));
    const int ranged=okay && ranges_reserved && pw_gpu_host_range(message.address,message.bytes)>=0;
    if(ranged) {
        /* Over this process's own reservation of the range: a fixed mapping replaces it. */
        void *mapped=mmap(at,message.bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0);
        okay=mapped==at;
        if(!okay) { void *again=at;sceKernelReserveVirtualRange(&again,message.bytes,PW_GPU_RESERVE_FIXED,16384); }
    } else if(okay) {
        okay=sceKernelReserveVirtualRange(&at,message.bytes,0,16384)==0;
        if(okay && at!=(void *)(uintptr_t)message.address) { sceKernelMunmap(at,message.bytes);okay=false; }
        if(okay) {
            void *mapped=mmap(at,message.bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0);
            okay=mapped==at;
            if(!okay)sceKernelMunmap(at,message.bytes);
        }
    }
    /* Kept for a placed mapping of this memory (radv_ps5_memory_map_at); closed when it is freed. */
    if(!okay || !__atomic_load_n(&keep_sources,__ATOMIC_ACQUIRE) || !source_add((uintptr_t)at,fd))close(fd);
    if(!okay) {
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
    /* Broker submissions complete before acknowledgment; no GPU references remain. */
    if(rpc(&message,NULL)) { __atomic_store_n(&broken,1,__ATOMIC_RELAXED);return; }
    if(memory->physical==PS5_GPU_LOCAL_MEMORY)__atomic_sub_fetch(&local_allocated,memory->bytes,__ATOMIC_RELAXED);
    else {
        /* In a reserved range the mapping goes back to a reservation (laid over it, it replaces it). */
        void *at=memory->cpu;
        if(!(ranges_reserved && pw_gpu_host_range((uint64_t)(uintptr_t)at,memory->bytes)>=0 &&
             !sceKernelReserveVirtualRange(&at,memory->bytes,PW_GPU_RESERVE_FIXED,16384) && at==memory->cpu))
            sceKernelMunmap(memory->cpu,memory->bytes);
        const int fd=source_find((uintptr_t)memory->cpu,true);
        if(fd>=0)close(fd);
        __atomic_sub_fetch(&allocated,memory->bytes,__ATOMIC_RELAXED);
    }
    memset(memory,0,sizeof(*memory));
}
static uint64_t submit_ns,submit_count,reply_transit_ns;
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
/* A placed mapping (VK_EXT_map_memory_placed): the same shared memory mapped again for the CPU at the address the
 * application chose, over its reservation there; the GPU keeps the first mapping's address. Device-local memory has
 * no CPU mapping in this process, and so no source. */
bool radv_ps5_memory_map_at(const struct radv_ps5_memory *m,void *a)
{
    const int fd=m->physical==PS5_GPU_LOCAL_MEMORY ? -1 : source_find((uintptr_t)m->cpu,false);
    if(fd<0 || !a || ((uintptr_t)a&16383))return false;
    return mmap(a,m->bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==a;
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
bool radv_ps5_vrange_reserve(uint64_t b,bool w,bool r,uint64_t v,struct radv_ps5_memory *m)
{ (void)b;(void)w;(void)r;(void)v;(void)m;return false; }
void radv_ps5_vrange_release(struct radv_ps5_memory *m) { (void)m; }
bool radv_ps5_vrange_bind(void *a,uint64_t b,const struct radv_ps5_memory *m,uint64_t o)
{ (void)a;(void)b;(void)m;(void)o;return false; }
bool radv_ps5_vrange_unbind(void *a,uint64_t b) { (void)a;(void)b;return false; }
