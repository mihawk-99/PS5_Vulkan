/* Copyright (C) 2026 Mihawk-99 */
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "display.h"
#include <ps5platform/kernel.h>
#include <ps5platform/videoout.h>
#include <cerrno>
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
bool capture_buffer(const uint8_t *buffer,const char *path)
{
    FILE *f=fopen(path,"wb");if(!f)return false;
    bool okay=fprintf(f,"P6\n%u %u\n255\n",width,height)>0;
    uint8_t row[width*3];
    for(unsigned y=0;y<height && okay;++y) {
        for(unsigned x=0;x<width;++x) {
            const uint32_t p=*(const uint32_t *)(buffer+tiled(x,y));
            row[x*3]=p>>16;row[x*3+1]=p>>8;row[x*3+2]=p;
        }
        okay=fwrite(row,1,sizeof(row),f)==sizeof(row);
    }
    okay &= !fflush(f);okay &= !fsync(fileno(f));okay &= !fclose(f);return okay;
}
}
int Ps5GpuDisplay::present(const void *pixels,unsigned w,unsigned h,unsigned pitch,FILE *log,const char *capture,bool bgra,
    int cursor_x,int cursor_y)
{
    if(!pixels || !w || w>width || !h || h>height || pitch<w*4 || pitch>width*4)return EINVAL;
    if(handle<0) {
        handle=sceVideoOutOpen(PS5_VIDEO_OUT_USER_SYSTEM,0,0,nullptr);
        if(handle<0)return handle;
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
    // Scaling, nearest neighbour, as tables; then each tile in memory order, flushed as it is finished.
    static uint16_t source_x[width],source_y[height];
    for(unsigned x=0;x<width;++x)source_x[x]=uint16_t(x*w/width);
    for(unsigned y=0;y<height;++y)source_y[y]=uint16_t(y*h/height);
    const auto &order=tile_order();
    for(unsigned by=0;by<block_rows;++by)for(unsigned bx=0;bx<blocks;++bx) {
        auto *tile=(uint32_t *)(buffer+(size_t(by*blocks+bx)<<16));
        for(unsigned k=0;k<16384;++k) {
            const unsigned at=order.pixel[k],x=bx*128+(at&127),y=by*128+(at>>7);
            if(y>=height)continue;
            uint32_t rgba;memcpy(&rgba,(const uint8_t *)pixels+size_t(source_y[y])*pitch+size_t(source_x[x])*4,4);
            tile[k]=bgra ? rgba : (rgba&0xff00ff00)|((rgba&0xff)<<16)|((rgba>>16)&0xff);
        }
        for(size_t at=0;at<65536;at+=64)__asm__ volatile("clflushopt (%0)"::"r"((uint8_t *)tile+at):"memory");
    }
    if(cursor_x>=0 && cursor_y>=0)draw_cursor(buffer,cursor_x,cursor_y);
    __asm__ volatile("sfence":::"memory");
    last_copy_ns=now_ns()-copy_start;
    const uint64_t serial=++flips;
    int status=sceVideoOutSubmitFlip(handle,back,PS5_VIDEO_OUT_FLIP_VSYNC,serial);
    // A capture (the probes') waits for this flip to be shown and reads that buffer back: proof of what was flipped.
    if(!status && capture) {
        if(!settled(handle))status=EBUSY;
        if(!status)status=sceVideoOutGetFlipStatus(handle,shown);
        if(!status && shown[PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT]!=serial)status=EIO;
        if(!status && !capture_buffer(buffer,capture))status=EIO;
    }
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
    return capture_buffer((const uint8_t *)mapped+((back+buffer_count-1)%buffer_count)*buffer_bytes,path);
}
bool Ps5GpuDisplay::read_last(uint32_t *pixels) const
{
    if(!registered || !flips || !mapped || !pixels)return false;
    const uint8_t *buffer=(const uint8_t *)mapped+((back+buffer_count-1)%buffer_count)*buffer_bytes;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
        pixels[size_t(y)*width+x]=*(const uint32_t *)(buffer+tiled(x,y));
    return true;
}
bool Ps5GpuDisplay::close(FILE *log)
{
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
