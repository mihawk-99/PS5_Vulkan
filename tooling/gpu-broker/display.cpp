/* Copyright (C) 2026 Mihawk-99 */
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "display.h"
#include "pw_display_capture.h"
#include "pw_display_rate.h"
#include "pw_display_overlay.h"
#include <ps5platform/kernel.h>
#include <ps5platform/videoout.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
extern "C" int sceKernelUsleep(unsigned);
#include <time.h>
namespace {
constexpr unsigned width=1920,height=1080,blocks=15,block_rows=9,buffer_count=3;
constexpr size_t alignment=2u<<20,frame_bytes=size_t(blocks)*block_rows<<16;
constexpr size_t buffer_bytes=(frame_bytes+alignment-1)/alignment*alignment,memory_bytes=buffer_bytes*buffer_count;
size_t tiled(unsigned x,unsigned y)
{
    const unsigned offset=((y<<4)&0x70)^((y<<5)&0xf00)^((y<<9)&0x1000)^((y<<8)&0x4000)^
        ((x<<2)&0xc)^((x<<5)&0x380)^((x<<4)&0x400)^((x<<6)&0x800)^((x<<9)&0xa000);
    return (size_t((y>>7)*blocks+(x>>7))<<16)+offset;
}
// The pixel each 4-byte word of a 64 KiB tile holds: dy<<7|dx. A frame is written tile by tile in memory order, so
// the stores are sequential, rather than pixel by pixel across the tiled layout.
struct TileOrder {
    uint16_t pixel[16384];
    TileOrder() { for(unsigned dy=0;dy<128;++dy)for(unsigned dx=0;dx<128;++dx)pixel[tiled(dx,dy)>>2]=uint16_t(dy<<7|dx); }
};
const TileOrder &tile_order() { static const TileOrder order;return order; }
// The pointer: an arrow, 1 the outline, 2 the fill, drawn at its tip.
constexpr unsigned cursor_width=12,cursor_height=19;
const char cursor_shape[cursor_height][cursor_width+1]={
    "1           ","11          ","121         ","1221        ","12221       ","122221      ","1222221     ",
    "12222221    ","122222221   ","1222222221  ","12222222221 ","122222111111","1222121     ","122112221   ",
    "12 1 1221   ","1   12221   ","     1221   ","     1221   ","      11    "};
void draw_cursor(uint8_t *buffer,int at_x,int at_y)
{
    for(unsigned dy=0;dy<cursor_height;++dy)for(unsigned dx=0;dx<cursor_width;++dx) {
        const char c=cursor_shape[dy][dx];
        const int x=at_x+int(dx),y=at_y+int(dy);
        if(c==' ' || x<0 || y<0 || x>=int(width) || y>=int(height))continue;
        auto *pixel=(uint32_t *)(buffer+tiled(unsigned(x),unsigned(y)));
        *pixel=c=='1' ? 0xff000000u : 0xffffffffu;
        __asm__ volatile("clflushopt (%0)"::"r"(pixel):"memory");
    }
}
uint64_t now_ns() { timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000u+t.tv_nsec; }
bool settled(int handle)
{
    for(unsigned i=0;i<2000;++i) {
        const int pending=sceVideoOutIsFlipPending(handle);
        if(pending<=0)return pending==0;
        sceKernelUsleep(1000);
    }
    return false;
}
// The refresh period: the median of six intervals between vblank waits, the first wait finding the phase; 0 when a
// wait fails (PS5_Mesa's wsi_common_videoout.c, videoout_vblank_period_ns).
uint64_t vblank_period_ns(int handle)
{
    uint64_t intervals[6];
    if(sceVideoOutWaitVblank(handle))return 0;
    uint64_t previous=now_ns();
    for(auto &interval:intervals) {
        if(sceVideoOutWaitVblank(handle))return 0;
        const uint64_t now=now_ns();interval=now-previous;previous=now;
    }
    for(unsigned i=1;i<6;++i)for(unsigned j=i;j>0 && intervals[j-1]>intervals[j];--j) {
        const uint64_t swap=intervals[j];intervals[j]=intervals[j-1];intervals[j-1]=swap;
    }
    return intervals[3];
}
// The display's captures, written on their own thread (pw_display_capture.h); one display per title.
PwDisplayCaptures captures=PW_DISPLAY_CAPTURES_INIT;
static_assert(frame_bytes==PW_DISPLAY_TILED_BYTES,"the capture copies the whole tiled frame");
}
// Opens the display: at the first present, or earlier when the title asks the rate it kept (graphics patch 0029,
// ps5_gpu_broker_display_hz). 0, or the first failure; a display that failed after VideoOut opened stays unregistered.
int Ps5GpuDisplay::open(FILE *log)
{
    handle=sceVideoOutOpen(PS5_VIDEO_OUT_USER_SYSTEM,0,0,nullptr);
    if(handle<0)return handle;
    // PS5_Proton's PROTON_120HZ=1, for a title that declares high frame rate (param.json attribute3 0x40): the
    // output is asked for 119.88 Hz as RADV's VideoOut asks (wsi_common_videoout.c, videoout_open_locked), and
    // kept only if the vblanks then come within 12.5 ms; otherwise 59.94 Hz is restored. Without it the broker's
    // display stayed at 59.94 Hz whatever the game asked, and three buffers flipped at each vblank held every
    // present to 60 a second. First the rate this process finds, before it changes anything: 119.88 Hz that an
    // earlier process left (a crash, or any end that skipped close()) is put back unless this one asks for it,
    // so no later run inherits it unasked. About seven vblanks, once a display.
    const char *high=getenv("PROTON_120HZ");const bool asked=high && !strcmp(high,"1");
    const uint64_t found=vblank_period_ns(handle);
    int restored=-1,supported=0,configured=-1;uint64_t period=0;
    if(!asked && found && found<=12500000)
        restored=sceVideoOutConfigureOutput(handle,PS5_VIDEO_OUT_MODE_RESTORE,nullptr,nullptr,nullptr);
    if(asked) {
        supported=sceVideoOutIsOutputSupported(handle,PS5_VIDEO_OUT_MODE_HIGH_FRAME_RATE,nullptr,nullptr,nullptr);
        configured=supported>0 ? sceVideoOutConfigureOutput(handle,PS5_VIDEO_OUT_MODE_HIGH_FRAME_RATE,nullptr,nullptr,nullptr) : -1;
        period=supported>0 && !configured ? vblank_period_ns(handle) : 0;
        high_frame_rate=pw_display_kept_120(asked,supported,configured,period);   // pw_display_rate.h
        if(supported>0 && !configured && !high_frame_rate)
            sceVideoOutConfigureOutput(handle,PS5_VIDEO_OUT_MODE_RESTORE,nullptr,nullptr,nullptr);
    }
    fprintf(log,"gpu-display rate found_us=%llu restored=%d asked_120hz=%d supported=%d configured=%d period_us=%llu "
        "high_frame_rate=%d\n",(unsigned long long)found/1000,restored,asked,supported,configured,
        (unsigned long long)period/1000,high_frame_rate);
    int status=sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),memory_bytes,alignment,3,&physical);
    if(!status)status=sceKernelMapDirectMemory(&mapped,memory_bytes,0x33,0,physical,alignment);
    if(status)return status;
    memset(mapped,0,memory_bytes);
    uint8_t attribute[PS5_VIDEO_OUT_ATTRIBUTE_BYTES]{};
    sceVideoOutSetBufferAttribute2(attribute,PS5_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SDR,PS5_VIDEO_OUT_TILING_64KB_R_X,width,height,0,0,0);
    ps5_video_out_buffer buffers[buffer_count]{};
    for(unsigned i=0;i<buffer_count;++i)buffers[i]={(uint8_t *)mapped+i*buffer_bytes,nullptr,{}};
    status=sceVideoOutSetFlipRate(handle,0);
    if(!status)status=sceVideoOutRegisterBuffers2(handle,0,0,buffers,buffer_count,attribute,0,nullptr);
    if(status)return status;
    registered=true;
    // PS5_Proton's PROTON_GPU_FAST_COPY=1: an unscaled frame is copied a 64-byte line at a time with streaming
    // stores (pw_display_copy.h); a scaled one, and every frame without it, pixel by pixel below.
    const char *fast=getenv("PROTON_GPU_FAST_COPY");fast_copy=fast && !strcmp(fast,"1");
    fprintf(log,"gpu-display opened fast_copy=%d\n",fast_copy);
    return 0;
}
// The see-through panel's cost (the blend, the blur and the bubbles), said every 120 composes (PS5_Proton rule 7: measured
// before it ships): its mean and worst in microseconds
static void composed(FILE *log,uint64_t ns)
{
    static unsigned count;static uint64_t sum,worst;
    sum+=ns;if(ns>worst)worst=ns;
    if(++count<120)return;
    if(log)fprintf(log,"gpu-overlay compose n=%u avg_us=%.0f max_us=%.0f\n",count,double(sum)/count/1e3,double(worst)/1e3);
    count=0,sum=0,worst=0;
}
int Ps5GpuDisplay::present(const void *pixels,unsigned w,unsigned h,unsigned pitch,FILE *log,const char *capture,bool bgra,
    int cursor_x,int cursor_y)
{
    // A row may be longer than the part shown: the top-left of a swapchain larger than the screen (pw_gpu_display_shown).
    if(!pixels || !w || w>width || !h || h>height || pitch<w*4 || pitch>2176*4)return EINVAL;
    if(handle<0) {
        const int status=open(log);
        if(status)return status;
    }
    if(!registered)return EBUSY;
    // Three buffers: the one written next was last on screen two flips ago, so it is free once the flip after it has
    // been shown. Only then is there a wait, and only when presents outrun the display.
    uint64_t shown[PS5_VIDEO_OUT_FLIP_STATUS_WORDS]{};
    if(flips>=2) {
        int status=0;unsigned waited=0;
        for(;;) {
            status=sceVideoOutGetFlipStatus(handle,shown);
            if(status || shown[PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT]+1>=flips || waited>=2000)break;
            sceKernelUsleep(250);++waited;
        }
        if(status)return status;
        if(shown[PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT]+1<flips)return EBUSY;
    }
    const uint64_t copy_start=now_ns();
    auto *buffer=(uint8_t *)mapped+back*buffer_bytes;
    // The frame into a tiled buffer, the pointer over it: the display's buffer, and a capture's own copy below.
    const auto frame=[&](uint8_t *target) {
        if(fast_copy && w==width && h==height)pw_display_copy_lines(target,(const uint8_t *)pixels,pitch,bgra);
        else {
        // Scaling, nearest neighbour, as tables; then each tile in memory order, flushed as it is finished.
        static uint16_t source_x[width],source_y[height];
        for(unsigned x=0;x<width;++x)source_x[x]=uint16_t(x*w/width);
        for(unsigned y=0;y<height;++y)source_y[y]=uint16_t(y*h/height);
        const auto &order=tile_order();
        for(unsigned by=0;by<block_rows;++by)for(unsigned bx=0;bx<blocks;++bx) {
            auto *tile=(uint32_t *)(target+(size_t(by*blocks+bx)<<16));
            for(unsigned k=0;k<16384;++k) {
                const unsigned at=order.pixel[k],x=bx*128+(at&127),y=by*128+(at>>7);
                if(y>=height)continue;
                uint32_t rgba;memcpy(&rgba,(const uint8_t *)pixels+size_t(source_y[y])*pitch+size_t(source_x[x])*4,4);
                tile[k]=bgra ? rgba : (rgba&0xff00ff00)|((rgba&0xff)<<16)|((rgba>>16)&0xff);
            }
            for(size_t at=0;at<65536;at+=64)__asm__ volatile("clflushopt (%0)"::"r"((uint8_t *)tile+at):"memory");
        }
        }
    };
    // clean: the frame without the panel, in cached memory (the shadow, or a capture's own picture)
    const auto decorate=[&](uint8_t *target,const uint8_t *clean) {
        if(overlay && blur) {
            const uint64_t start=now_ns();
            const int fresh=!blur_ready || !(composes++&1);
            pw_display_overlay_compose(target,clean,overlay,overlay_x,overlay_y,overlay_w,overlay_h,start/1e9,blur,fresh,
                                       sweep_at(start));
            blur_ready=true;
            composed(log,now_ns()-start);
        }
        else if(overlay)pw_display_overlay_blit(target,overlay,overlay_x,overlay_y,overlay_w,overlay_h);
        if(cursor_x>=0 && cursor_y>=0)draw_cursor(target,cursor_x,cursor_y);
    };
    const auto fill=[&](uint8_t *target) { frame(target);decorate(target,target); };
    last_cursor_x=cursor_x,last_cursor_y=cursor_y;
    last_frame_ns=now_ns();
    if(overlay && shadow) {   // the frame kept for the panel's re-shows, then streamed out with the panel over it
        frame(shadow);shadow_valid=true;
        pw_display_stream_copy(buffer,shadow,frame_bytes);decorate(buffer,shadow);
    } else fill(buffer);
    __asm__ volatile("sfence":::"memory");
    last_copy_ns=now_ns()-copy_start;
    // A capture (a probe's or a pad script's): the same frame filled again into memory of its own and written by the
    // capture thread, so the present does not wait for the picture (pw_display_capture.h). Never read back from the
    // display's buffer: that reads at about 105 MB/s, 80 ms for a frame inside the present (2026-10-17).
    // Without memory for the copy or a thread, it is read back and written here once the flip is shown, as before.
    // With the fast copy the capture reads what was scanned out instead (below, once the flip is shown): its own copy
    // came out black on the console while the screen showed the frame (w3-ab-b-pro-2026-11-01, the owner watching),
    // and the same copy into the same kind of buffer is exact on a host. A capture then costs about 80 ms in that present.
    uint8_t *picture=nullptr;uint64_t picture_ns=0;
    if(capture && !fast_copy) {
        const uint64_t start=now_ns();
        picture=(uint8_t *)aligned_alloc(64,frame_bytes);   // the line copy's streaming stores want 16-byte lines
        if(picture)fill(picture);
        picture_ns=now_ns()-start;
    }
    const uint64_t serial=__atomic_add_fetch(&flips,1,__ATOMIC_RELAXED);   // read without display_lock (ps5_gpu_broker_flips)
    int status=sceVideoOutSubmitFlip(handle,back,PS5_VIDEO_OUT_FLIP_VSYNC,serial);
    last_present_ns=now_ns();
    if(!status && capture) {
        if(picture && !pw_display_capture_take(&captures,picture,capture,serial,picture_ns,log))picture=nullptr;
        else {
            if(!settled(handle))status=EBUSY;
            if(!status)status=sceVideoOutGetFlipStatus(handle,shown);
            if(!status && shown[PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT]!=serial)status=EIO;
            if(!status && pw_display_capture_write(buffer,capture))status=EIO;
        }
    }
    free(picture);   // not handed over: the flip failed, or it was written here
    if(status || serial<=8 || !(serial%600))
        fprintf(log,"gpu-display flip=%llu shown=%llu source=%ux%u copy_us=%llu status=%#x\n",
            (unsigned long long)serial,(unsigned long long)shown[PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT],w,h,
            (unsigned long long)last_copy_ns/1000,unsigned(status));
    back=(back+1)%buffer_count;return status;
}
bool Ps5GpuDisplay::capture_last(const char *path)
{
    // Waits for the last flip to be shown, then reads that buffer back.
    if(!registered || !flips || !settled(handle))return false;
    uint64_t shown[PS5_VIDEO_OUT_FLIP_STATUS_WORDS]{};
    if(sceVideoOutGetFlipStatus(handle,shown) || shown[PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT]!=flips)return false;
    return !pw_display_capture_write((const uint8_t *)mapped+((back+buffer_count-1)%buffer_count)*buffer_bytes,path);
}
bool Ps5GpuDisplay::read_last(uint32_t *pixels) const
{
    if(!registered || !flips || !mapped || !pixels)return false;
    const uint8_t *buffer=(const uint8_t *)mapped+((back+buffer_count-1)%buffer_count)*buffer_bytes;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
        pixels[size_t(y)*width+x]=*(const uint32_t *)(buffer+tiled(x,y));
    return true;
}
int Ps5GpuDisplay::reshow(FILE *log)
{
    if(!registered || !flips || !mapped)return EBUSY;
    uint64_t shown[PS5_VIDEO_OUT_FLIP_STATUS_WORDS]{};
    if(flips>=2) {   // as present: the buffer written next is free once the flip after it has been shown
        int status=0;unsigned waited=0;
        for(;;) {
            status=sceVideoOutGetFlipStatus(handle,shown);
            if(status || shown[PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT]+1>=flips || waited>=2000)break;
            sceKernelUsleep(250);++waited;
        }
        if(status)return status;
        if(shown[PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT]+1<flips)return EBUSY;
    }
    // The last frame from the shadow (cached; streamed out in a few ms). Without one, or before it holds a frame, the
    // last frame is read back from the display (slow: 137 ms on a standard PS5), into the shadow when there is one.
    const auto *last=(const __m128i *)((const uint8_t *)mapped+((back+buffer_count-1)%buffer_count)*buffer_bytes);
    auto *buffer=(uint8_t *)mapped+back*buffer_bytes;
    if(shadow && !shadow_valid) {
        for(size_t at=0;at<frame_bytes/16;++at)_mm_store_si128((__m128i *)(void *)shadow+at,_mm_load_si128(last+at));
        shadow_valid=true;
    }
    if(shadow)pw_display_stream_copy(buffer,shadow,frame_bytes);
    else for(size_t at=0;at<frame_bytes/16;++at)_mm_stream_si128((__m128i *)(void *)buffer+at,_mm_load_si128(last+at));
    if(overlay && shadow && blur) {
        const uint64_t start=now_ns();
        const int fresh=!blur_ready || !(composes++&1);
        pw_display_overlay_compose(buffer,shadow,overlay,overlay_x,overlay_y,overlay_w,overlay_h,start/1e9,blur,fresh,
                                   sweep_at(start));
        blur_ready=true;
        composed(log,now_ns()-start);
    }
    else if(overlay)pw_display_overlay_blit(buffer,overlay,overlay_x,overlay_y,overlay_w,overlay_h);
    if(last_cursor_x>=0 && last_cursor_y>=0)draw_cursor(buffer,last_cursor_x,last_cursor_y);
    _mm_sfence();
    const uint64_t serial=__atomic_add_fetch(&flips,1,__ATOMIC_RELAXED);
    const int status=sceVideoOutSubmitFlip(handle,back,PS5_VIDEO_OUT_FLIP_VSYNC,serial);
    last_present_ns=now_ns();
    if(status && log)fprintf(log,"gpu-display reshow flip=%llu status=%#x\n",(unsigned long long)serial,unsigned(status));
    back=(back+1)%buffer_count;
    return status;
}
double Ps5GpuDisplay::sweep_at(uint64_t now)
{
    if(sweep_ns && (now-sweep_ns>1800000000ull || panel_press_ns>sweep_ns))sweep_ns=0;   // over, or a key stopped it
    if(!sweep_ns && next_sweep_ns && now>=next_sweep_ns && (!panel_press_ns || now-panel_press_ns>=3000000000ull))
        sweep_ns=now,next_sweep_ns=now+30000000000ull;
    return sweep_ns ? (now-sweep_ns)/1e9 : -1.0;
}
void Ps5GpuDisplay::keep_shadow(bool on)
{
    if(on && !shadow) {
        shadow=(uint8_t *)aligned_alloc(64,frame_bytes),shadow_valid=false,blur_ready=false;
        blur=(uint32_t *)malloc((size_t(width/8+2)*(height/8+2)*2+size_t(width/4)*(height/4))*sizeof(uint32_t));
    } else if(!on) free(shadow),free(blur),shadow=nullptr,blur=nullptr,shadow_valid=false;
}
bool Ps5GpuDisplay::close(FILE *log)
{
    pw_display_capture_drain(&captures);   // every picture asked for is on disk before the title goes
    free(overlay);overlay=nullptr;overlay_w=overlay_h=0;
    free(shadow);free(blur);shadow=nullptr;blur=nullptr;shadow_valid=false;
    int blank=0,unregistered=0,closed=0;
    if(handle>=0) {
        if(registered) {
            blank=sceVideoOutSubmitFlip(handle,-1,PS5_VIDEO_OUT_FLIP_VSYNC,0);
            if(!settled(handle))return false;
            unregistered=sceVideoOutUnregisterBuffers(handle,0);
            for(unsigned i=0;i<120 && unregistered==PS5_VIDEO_OUT_ERROR_BUSY;++i) {
                sceKernelUsleep(10000);unregistered=sceVideoOutUnregisterBuffers(handle,0);
            }
        }
        if(high_frame_rate)sceVideoOutConfigureOutput(handle,PS5_VIDEO_OUT_MODE_RESTORE,nullptr,nullptr,nullptr);
        high_frame_rate=false;
        closed=sceVideoOutClose(handle);
        if(registered && unregistered && closed)return false;
        handle=-1;registered=false;
    }
    int unmapped=mapped ? sceKernelMunmap(mapped,memory_bytes) : 0;
    if(unmapped)return false;
    mapped=nullptr;
    int released=physical>=0 ? sceKernelReleaseDirectMemory(physical,memory_bytes) : 0;
    if(!released)physical=-1;
    fprintf(log,"gpu-display closed=1 flips=%llu blank=%#x unregister=%#x close=%#x release=%#x\n",
        (unsigned long long)flips,unsigned(blank),unsigned(unregistered),unsigned(closed),unsigned(released));fsync(fileno(log));
    return !released && !closed && !unregistered && !blank;
}
