/* SPDX-License-Identifier: GPL-2.0-or-later
 * Ajm MP3 streaming adapter. Decoder: pinned CC0 minimp3 (vendor provenance).
 * Format/sideband ABI checked against pinned shadPS4 Ajm references.
 * Included after Instance, Output, output_write and gapless helpers. */
typedef struct {
    uint64_t frame_size;
    uint32_t channels,samples,bitrate,rate,delay,frames,total,ofl;
} Mp3Parse601;
typedef struct {
    mp3dec_t decoder;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    unsigned char converted[MINIMP3_MAX_SAMPLES_PER_FRAME*4];
    uint32_t raw_header,bitrate;
    unsigned decoded,ofl_checked,reported;
} Mp3State601;
_Static_assert(sizeof(Mp3Parse601)==40,"Ajm MP3 parse ABI");
static size_t mp3_frames601;
static uint32_t mp3_be601(const unsigned char* p) {return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint32_t mp3_bits601(const unsigned char* p,unsigned at,unsigned n) {
    uint32_t value=0;for(unsigned i=0;i<n;++i)value=(value<<1)|((p[(at+i)/8]>>(7-(at+i)%8))&1);return value;
}
static int mp3_header601(const unsigned char* p,uint64_t size,Mp3Parse601* info) {
    static const uint16_t high[]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const uint16_t low[]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const uint32_t rates[]={44100,48000,32000};
    if(!p || !info || size<4)return 0;
    uint32_t h=mp3_be601(p);unsigned version=(h>>19)&3,layer=(h>>17)&3,rate=(h>>10)&3,bit=(h>>12)&15;
    if((h&0xffe00000)!=0xffe00000 || version==1 || layer!=1 || rate==3 || bit==0 || bit==15)return 0;
    memset(info,0,sizeof(*info));info->channels=((h>>6)&3)==3?1:2;info->rate=rates[rate]>>(version==3?0:version==2?1:2);
    info->samples=version==3?1152:576;info->bitrate=(version==3?high[bit]:low[bit])*1000u;
    info->frame_size=(uint64_t)(version==3?144:72)*info->bitrate/info->rate+((h>>9)&1);
    return info->frame_size>=4;
}
static void mp3_ofl601(const unsigned char* p,uint64_t size,Mp3Parse601* info) {
    if(size<info->frame_size)return;
    uint32_t h=mp3_be601(p);unsigned v=(h>>19)&3,ch=info->channels;
    unsigned first=4+(((h>>16)&1)?0:2),side=v==3?(ch==1?17:32):(ch==1?9:17);
    if(first+side>info->frame_size)return;
    const unsigned char* si=p+first;
    if(mp3_bits601(si,0,v==3?9:8))return; /* beginning of stream only; no prior reservoir */
    unsigned at=v==3?(ch==1?18:20):(ch==1?9:10),payload=0;
    for(unsigned g=0;g<(v==3?2u:1u);++g)for(unsigned c=0;c<ch;++c){payload+=mp3_bits601(si,at,12);at+=v==3?59:63;}
    uint64_t pos=first+(at+payload+7)/8,end=info->frame_size;
    if(pos>end)return;
    if(pos+8<=end && (!memcmp(p+pos,"Xing",4)||!memcmp(p+pos,"Info",4))) {
        uint32_t flags=mp3_be601(p+pos+4);uint64_t field=pos+8;
        if(flags&1){if(field+4>end)return;info->frames=mp3_be601(p+field);field+=4;}
        if(flags&2)field+=4;if(flags&4)field+=100;if(flags&8)field+=4;
        if(field+24>end || memcmp(p+field,"LAME",4))return;
        uint32_t delay=((uint32_t)p[field+21]<<4)|(p[field+22]>>4),padding=((p[field+22]&15u)<<8)|p[field+23];
        uint64_t full=(uint64_t)info->frames*info->samples;
        if(info->frames && full>=delay+padding && full-delay-padding<=UINT32_MAX)info->total=(uint32_t)(full-delay-padding);
        info->delay=info->samples+delay+529;info->ofl=1;return;
    }
    if(pos+26<=end && !memcmp(p+pos,"VBRI",4)){info->delay=((uint32_t)p[pos+6]<<8)|p[pos+7];info->ofl=2;return;}
    for(;pos+10<=end;++pos)if(p[pos]==0xb4) {
        unsigned char crc=0xff;
        for(unsigned i=0;i<9;++i)for(unsigned bit=0;bit<8;++bit){unsigned same=((crc>>7)&1)==((p[pos+i]>>(7-bit))&1);crc=(unsigned char)((crc<<1)^(same?0x45:0));}
        if(crc!=p[pos+9])continue;
        info->delay=((uint32_t)p[pos+1]<<8)|p[pos+2];info->total=mp3_be601(p+pos+3);info->ofl=3;return;
    }
}
static ABI int32_t mp3_parse601(const unsigned char* p,uint32_t bytes,int ofl,Mp3Parse601* info) {
    if(!mp3_header601(p,bytes,info))return ERR_INVALID_PARAMETER;
    if(ofl)mp3_ofl601(p,bytes,info);return 0;
}
static void mp3_reset601(Instance* in) {
    Mp3State601* state=in->handle;if(!state)return;
    const unsigned reported=state->reported;memset(state,0,sizeof(*state));state->reported=reported;mp3dec_init(&state->decoder);
    memset(&in->info,0,sizeof(in->info));in->initialized=1;
}
static uint32_t mp3_frame601(Instance* in,const unsigned char** input,uint64_t* bytes,Output* out,uint32_t* written,uint32_t* decoded,int32_t* internal) {
    Mp3State601* state=in->handle;*written=0;*decoded=0;
    if(!state)return RESULT_NOT_INITIALIZED;
    /* Complete tags may be consumed; incomplete frames/tags remain caller-owned. */
    while(*bytes>=3 && !memcmp(*input,"ID3",3)) {
        if(*bytes<10)return RESULT_PARTIAL_INPUT;
        const unsigned char* p=*input;if((p[6]|p[7]|p[8]|p[9])&0x80)return RESULT_INVALID_PARAMETER;
        uint64_t n=10+((uint32_t)p[6]<<21)+((uint32_t)p[7]<<14)+((uint32_t)p[8]<<7)+p[9]+((p[5]&0x10)?10u:0u);
        if(n>*bytes)return RESULT_PARTIAL_INPUT;*input+=n;*bytes-=n;
    }
    if(*bytes<4)return RESULT_PARTIAL_INPUT;
    Mp3Parse601 info;
    if(!mp3_header601(*input,*bytes,&info)){*internal=-1;return RESULT_CODEC_ERROR;}
    if(*bytes<info.frame_size)return RESULT_PARTIAL_INPUT;
    uint32_t h=mp3_be601(*input);memcpy(&state->raw_header,*input,4);state->bitrate=info.bitrate;
    if(!state->ofl_checked) {
        state->ofl_checked=1;if(!(in->codec_flags&1)) {
            mp3_ofl601(*input,*bytes,&info);
            if(info.total || info.delay){in->gapless_init.total_samples=info.total;in->gapless_init.skip_samples=(uint16_t)info.delay;gapless_reset(in);}
        }
    }
    (void)h;
    in->info.channels=(int)info.channels;in->info.samplingRate=(int)info.rate;in->info.frameSamples=(int)info.samples;in->info.framesInSuperframe=1;in->info.superframeSize=(int)info.frame_size;
    if(!gapless_end(in) && output_room(out)<next_frame_bytes(in))return RESULT_NOT_ENOUGH_ROOM;
    mp3dec_frame_info_t actual={0};int samples=mp3dec_decode_frame(&state->decoder,*input,(int)info.frame_size,state->pcm,&actual);
    if(actual.frame_bytes!=(int)info.frame_size || actual.frame_offset || (samples && (actual.channels!=(int)info.channels || actual.hz!=(int)info.rate || samples!=(int)info.samples))){*internal=-2;return RESULT_CODEC_ERROR|RESULT_FATAL;}
    *input+=info.frame_size;*bytes-=info.frame_size;
    if(!samples)return 0; /* bit-reservoir priming on a newly sought stream */
    uint32_t skip=in->gapless.skip_samples<(uint32_t)samples?in->gapless.skip_samples:(uint32_t)samples;
    uint32_t count=(uint32_t)samples-skip;
    if(in->gapless_init.total_samples && count>in->gapless.total_samples)count=in->gapless.total_samples;
    in->gapless.skip_samples-=(uint16_t)skip;
    for(uint32_t s=0;s<count;++s)for(uint32_t c=0;c<info.channels;++c) {
        int16_t value=state->pcm[(s+skip)*info.channels+c];size_t index=(size_t)s*info.channels+c;
        if(in->format==FORMAT_S16)memcpy(state->converted+index*2,&value,2);
        else if(in->format==FORMAT_S32){int32_t v=(int32_t)value*65536;memcpy(state->converted+index*4,&v,4);}
        else {float v=(float)value/32768.0f;memcpy(state->converted+index*4,&v,4);}
    }
    output_write(out,state->converted,(uint64_t)count*info.channels*(unsigned)pcm_size(in->format));
    in->gapless.skipped_samples+=(uint16_t)((uint32_t)samples-count);if(in->gapless_init.total_samples)in->gapless.total_samples-=count;
    *written=count;*decoded=1;++state->decoded;++mp3_frames601;
    if(!state->reported && count){state->reported=1;printf("AJM601 MP3 decoded: channels=%u rate=%u samples=%u frame_bytes=%llu format=%d decoder_bytes=%zu\n",info.channels,info.rate,count,(unsigned long long)info.frame_size,in->format,sizeof(*state));fflush(stdout);}
    return 0;
}
static void mp3_info601(const Instance* in,void* output) {
    unsigned char value[16]={0};const Mp3State601* state=in->handle;
    if(state && state->raw_header){memcpy(value,&state->raw_header,4);unsigned char bytes[4];memcpy(bytes,&state->raw_header,4);uint32_t h=mp3_be601(bytes);value[4]=(h>>16)&1;value[5]=(h>>6)&3;value[6]=(h>>4)&3;value[7]=(h>>3)&1;value[8]=(h>>2)&1;value[9]=h&3;}
    memcpy(output,value,16);
}
static void release_codec601(Instance* in) {
    if(in->handle){if(in->codec==1)Atrac9ReleaseHandle(in->handle);else free(in->handle);in->handle=NULL;}
}
