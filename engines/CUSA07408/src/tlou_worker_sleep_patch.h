/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Applied only by the loader, while text is writable and before guest entry. */
static int sleep551_rx(const Segment* segments,uint64_t count,uint64_t at,uint64_t bytes) {
    for(uint64_t i=0;i<count;++i)if((segments[i].flags&1) && at>=segments[i].address && bytes<=segments[i].size && at-segments[i].address<=segments[i].size-bytes)return 1;
    return 0;
}
static int sleep551_patch(unsigned char* base,uint64_t size,const Segment* segments,uint64_t count,unsigned mode) {
    static const unsigned char function[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x89,0xf8,0x0f,0xb6,0xc8};
    static const unsigned char wait[]={0x80,0x7f,0x59,0x00,0x75,0x04,0xf3,0x90,0xeb,0x59,0x49,0x81,0xf8,0xe8,0x03,0x00,0x00};
    if(mode!=0)return 0;
    if(size<0xaaca36+sizeof(wait) || !sleep551_rx(segments,count,0xa90270,sizeof(function)) || !sleep551_rx(segments,count,0xaaca36,sizeof(wait)))return 0;
    if(memcmp(base+0xa90270,function,sizeof(function)) || memcmp(base+0xaaca36,wait,sizeof(wait)))return 0;

    return 1;
}
