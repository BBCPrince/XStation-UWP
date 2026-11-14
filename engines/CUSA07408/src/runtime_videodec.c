/* SPDX-License-Identifier: GPL-2.0-or-later
 * H.264 software decoder. ABI/resource policy follows shadPS4 (see provenance).
 * FFmpeg owns its CPU decoder buffers; the guest receives linear NV12 frames. */
#include "runtime.h"
#include "runtime_videodec_types.h"
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#define E(name) ((int32_t)ORBIS_VIDEODEC_ERROR_##name)
_Static_assert(sizeof(OrbisVideodecConfigInfo)==40,"Video config ABI");
_Static_assert(sizeof(OrbisVideodecResourceInfo)==56,"Video resource ABI");
_Static_assert(sizeof(OrbisVideodecPictureInfo)==112,"Video picture ABI");
typedef struct { uintptr_t token; AVCodecContext *codec; struct SwsContext *convert; int draining; unsigned frames; } Decoder;
static Decoder decoders[16];
static uintptr_t next_token=1;
static pthread_mutex_t decoder_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t aligned(uint64_t value,uint64_t alignment) { return (value+alignment-1)&~(alignment-1); }
static Decoder *find_decoder(const OrbisVideodecCtrl *control) {
    if (!control || control->thisSize!=sizeof(*control)) return NULL;
    for (unsigned i=0;i<16;++i) if (decoders[i].token && decoders[i].token==(uintptr_t)control->handle) return &decoders[i];
    return NULL;
}
static int32_t config_valid(const OrbisVideodecConfigInfo *config) {
    if (!config) return E(ARGUMENT_POINTER);
    if (config->thisSize!=sizeof(*config)) return E(STRUCT_SIZE);
    if (config->codecType) return E(CODEC_TYPE);
    if (config->maxFrameWidth<0 || config->maxFrameHeight<0 || config->maxFrameWidth>8192 ||
        config->maxFrameHeight>8192 || config->maxDpbFrameCount<0 || config->maxDpbFrameCount>32) return E(CONFIG_INFO);
    return 0;
}
static ABI int32_t video_query(const OrbisVideodecConfigInfo *config,OrbisVideodecResourceInfo *resource) {
    int32_t error=config_valid(config); if (error) return error;
    if (!resource) return E(ARGUMENT_POINTER);
    if (resource->thisSize!=sizeof(*resource)) return E(STRUCT_SIZE);
    uint64_t width=config->maxFrameWidth,height=config->maxFrameHeight;
    if (!width || !height) { width=config->maxLevel>=150 ? 3840 : 1920; height=config->maxLevel>=150 ? 2160 : 1080; }
    uint64_t frame=aligned(aligned(width,256)*aligned(height,16)*3/2,256)+0x4000;
    uint64_t surfaces=(config->maxDpbFrameCount ? (uint64_t)config->maxDpbFrameCount : 8)+2;
    resource->cpuMemorySize=16*1024*1024;
    resource->cpuGpuMemorySize=frame*surfaces+8*1024*1024;
    resource->pCpuMemory=resource->pCpuGpuMemory=NULL;
    resource->maxFrameBufferSize=frame; resource->frameBufferAlignment=256;
    return 0;
}
static ABI int32_t video_create(const OrbisVideodecConfigInfo *config,const OrbisVideodecResourceInfo *resource,OrbisVideodecCtrl *control) {
    int32_t error=config_valid(config); if (error) return error;
    if (!resource || !control) return E(ARGUMENT_POINTER);
    if (resource->thisSize!=sizeof(*resource)) return E(STRUCT_SIZE);
    pthread_mutex_lock(&decoder_lock);
    Decoder *decoder=NULL;
    for (unsigned i=0;i<16;++i) if (!decoders[i].token) { decoder=&decoders[i]; break; }
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *context=decoder && codec ? avcodec_alloc_context3(codec) : NULL;
    if (!context) { pthread_mutex_unlock(&decoder_lock); return E(API_FAIL); }
    context->width=config->maxFrameWidth; context->height=config->maxFrameHeight;
    context->flags|=AV_CODEC_FLAG_COPY_OPAQUE; context->max_pixels=INT64_C(8192)*8192;
    if (avcodec_open2(context,codec,NULL)<0) {
        avcodec_free_context(&context); pthread_mutex_unlock(&decoder_lock); return E(API_FAIL);
    }
    *decoder=(Decoder){.token=next_token++,.codec=context};
    control->thisSize=sizeof(*control); control->handle=(void*)decoder->token; control->version=1;
    printf("Videodec: H.264 decoder %llu created (%dx%d)\n",(unsigned long long)decoder->token,context->width,context->height);
    pthread_mutex_unlock(&decoder_lock); return 0;
}
static int32_t receive_picture(Decoder *decoder,OrbisVideodecFrameBuffer *buffer,OrbisVideodecPictureInfo *picture) {
    AVFrame *frame=av_frame_alloc(); if (!frame) return E(API_FAIL);
    int error=avcodec_receive_frame(decoder->codec,frame);
    if (error==AVERROR(EAGAIN) || error==AVERROR_EOF) { av_frame_free(&frame); return 0; }
    if (error<0) { av_frame_free(&frame); return E(DECODE_AU); }
    if (frame->width<=0 || frame->height<=0 || frame->width>8192 || frame->height>8192 ||
        (frame->width&1) || (frame->height&1)) { av_frame_free(&frame); return E(MISMATCH_SPEC); }
    uint64_t pitch=aligned(frame->width,64),height=aligned(frame->height,16),bytes=pitch*height*3/2;
    if (!buffer->pFrameBuffer) { av_frame_free(&frame); return E(FRAME_BUFFER_POINTER); }
    if (buffer->frameBufferSize<bytes) { av_frame_free(&frame); return E(FRAME_BUFFER_SIZE); }
    if ((uintptr_t)buffer->pFrameBuffer&255) { av_frame_free(&frame); return E(FRAME_BUFFER_ALIGNMENT); }
    if (frame->format!=AV_PIX_FMT_NV12) {
        AVFrame *converted=av_frame_alloc();
        if (!converted) { av_frame_free(&frame); return E(API_FAIL); }
        converted->format=AV_PIX_FMT_NV12; converted->width=frame->width; converted->height=frame->height;
        if (av_frame_get_buffer(converted,32)<0 || av_frame_copy_props(converted,frame)<0) {
            av_frame_free(&converted); av_frame_free(&frame); return E(API_FAIL);
        }
        converted->opaque=frame->opaque;
        decoder->convert=sws_getCachedContext(decoder->convert,frame->width,frame->height,(enum AVPixelFormat)frame->format,
            frame->width,frame->height,AV_PIX_FMT_NV12,SWS_FAST_BILINEAR,NULL,NULL,NULL);
        if (!decoder->convert || sws_scale(decoder->convert,(const uint8_t *const*)frame->data,frame->linesize,0,
            frame->height,converted->data,converted->linesize)!=frame->height) {
            av_frame_free(&converted); av_frame_free(&frame); return E(API_FAIL);
        }
        av_frame_free(&frame); frame=converted;
    }
    uint8_t *luma=buffer->pFrameBuffer,*chroma=luma+pitch*height;
    memset(luma,16,(size_t)(pitch*height)); memset(chroma,128,(size_t)(pitch*height/2));
    for (uint64_t y=0;y<height;++y) {
        int source=y<(uint64_t)frame->height ? (int)y : frame->height-1;
        memcpy(luma+y*pitch,frame->data[0]+(int64_t)source*frame->linesize[0],(size_t)frame->width);
    }
    for (uint64_t y=0;y<height/2;++y) {
        int source=y<(uint64_t)frame->height/2 ? (int)y : frame->height/2-1;
        memcpy(chroma+y*pitch,frame->data[1]+(int64_t)source*frame->linesize[1],(size_t)frame->width);
    }
    picture->isValid=1; picture->codecType=0;
    picture->frameWidth=(uint32_t)aligned(frame->width,16); picture->framePitch=(uint32_t)pitch;
    picture->frameHeight=(uint32_t)height; picture->isErrorPic=0;
    picture->ptsData=(uint64_t)frame->pts; picture->attachedData=(uintptr_t)frame->opaque;
    memset(&picture->codec,0,sizeof(picture->codec));
    OrbisVideodecAvcInfo *avc=&picture->codec.avc;
    avc->colourPrimaries=(uint8_t)frame->color_primaries;
    avc->transferCharacteristics=(uint8_t)frame->color_trc;
    avc->matrixCoefficients=(uint8_t)frame->colorspace;
    avc->videoFullRangeFlag=frame->color_range==AVCOL_RANGE_JPEG;
    avc->frameCropRightOffset=(uint32_t)(pitch-frame->width); avc->frameCropBottomOffset=(uint32_t)(height-frame->height);
    AVRational rate=decoder->codec->framerate;
    if (rate.num>0 && rate.den>0) { avc->numUnitsInTick=(uint32_t)rate.den; avc->timeScale=(uint32_t)rate.num*2; avc->fixedFrameRateFlag=1; }
    if (frame->sample_aspect_ratio.num>0 && frame->sample_aspect_ratio.num<=65535 &&
        frame->sample_aspect_ratio.den>0 && frame->sample_aspect_ratio.den<=65535) {
        avc->aspectRatioIdc=255; avc->sarWidth=(uint16_t)frame->sample_aspect_ratio.num; avc->sarHeight=(uint16_t)frame->sample_aspect_ratio.den;
    }
    if (!decoder->frames++) printf("Videodec: first H.264 picture %dx%d, NV12 pitch %llu\n",frame->width,frame->height,(unsigned long long)pitch);
    av_frame_free(&frame); return 0;
}
static int32_t output_valid(OrbisVideodecCtrl *control,OrbisVideodecFrameBuffer *buffer,OrbisVideodecPictureInfo *picture) {
    if (!control || !buffer || !picture) return E(ARGUMENT_POINTER);
    if (control->thisSize!=sizeof(*control) || buffer->thisSize!=sizeof(*buffer) || picture->thisSize!=sizeof(*picture)) return E(STRUCT_SIZE);
    picture->isValid=0; return 0;
}
static ABI int32_t video_decode(OrbisVideodecCtrl *control,const OrbisVideodecInputData *input,
                               OrbisVideodecFrameBuffer *buffer,OrbisVideodecPictureInfo *picture) {
    int32_t error=output_valid(control,buffer,picture); if (error) return error;
    if (!input) return E(ARGUMENT_POINTER);
    if (input->thisSize!=sizeof(*input)) return E(STRUCT_SIZE);
    if (!input->pAuData) return E(AU_POINTER);
    if (!input->auSize || input->auSize>INT_MAX) return E(AU_SIZE);
    pthread_mutex_lock(&decoder_lock);
    Decoder *decoder=find_decoder(control);
    if (!decoder) { pthread_mutex_unlock(&decoder_lock); return E(HANDLE); }
    AVPacket *packet=av_packet_alloc();
    if (!packet || av_new_packet(packet,(int)input->auSize)<0) {
        av_packet_free(&packet); pthread_mutex_unlock(&decoder_lock); return E(API_FAIL);
    }
    /* FFmpeg requires zero padding beyond the access unit; do not overread a
     * caller's exact-size buffer or let the decoder retain guest-owned bytes. */
    memcpy(packet->data,input->pAuData,(size_t)input->auSize);
    packet->pts=(int64_t)input->ptsData; packet->dts=(int64_t)input->dtsData; packet->opaque=(void*)(uintptr_t)input->attachedData;
    if (decoder->draining) { avcodec_flush_buffers(decoder->codec); decoder->draining=0; }
    int sent=avcodec_send_packet(decoder->codec,packet);
    av_packet_free(&packet);
    int32_t result=sent<0 ? E(DECODE_AU) : receive_picture(decoder,buffer,picture);
    pthread_mutex_unlock(&decoder_lock); return result;
}
static ABI int32_t video_flush(OrbisVideodecCtrl *control,OrbisVideodecFrameBuffer *buffer,OrbisVideodecPictureInfo *picture) {
    int32_t error=output_valid(control,buffer,picture); if (error) return error;
    pthread_mutex_lock(&decoder_lock); Decoder *decoder=find_decoder(control);
    if (!decoder) { pthread_mutex_unlock(&decoder_lock); return E(HANDLE); }
    int sent=0;
    if (!decoder->draining) { sent=avcodec_send_packet(decoder->codec,NULL); if (!sent || sent==AVERROR_EOF) decoder->draining=1; }
    int32_t result=sent<0 && sent!=AVERROR_EOF ? E(DECODE_AU) : receive_picture(decoder,buffer,picture);
    pthread_mutex_unlock(&decoder_lock); return result;
}
static ABI int32_t video_reset(OrbisVideodecCtrl *control) {
    if (!control) return E(ARGUMENT_POINTER);
    pthread_mutex_lock(&decoder_lock); Decoder *decoder=find_decoder(control);
    int32_t result=decoder ? 0 : E(HANDLE);
    if (decoder) { avcodec_flush_buffers(decoder->codec); decoder->draining=0; }
    pthread_mutex_unlock(&decoder_lock); return result;
}
static ABI int32_t video_delete(OrbisVideodecCtrl *control) {
    if (!control) return E(ARGUMENT_POINTER);
    pthread_mutex_lock(&decoder_lock); Decoder *decoder=find_decoder(control);
    int32_t result=decoder ? 0 : E(HANDLE);
    if (decoder) { avcodec_free_context(&decoder->codec); sws_freeContext(decoder->convert); *decoder=(Decoder){0}; control->handle=NULL; }
    pthread_mutex_unlock(&decoder_lock); return result;
}
uintptr_t runtime_videodec_resolve(const char *name) {
    static const RuntimeExport exports[]={
        {"sceVideodecQueryResourceInfo",video_query},{"sceVideodecCreateDecoder",video_create},
        {"sceVideodecDecode",video_decode},{"sceVideodecFlush",video_flush},
        {"sceVideodecReset",video_reset},{"sceVideodecDeleteDecoder",video_delete},
    };
    return RUNTIME_LOOKUP(exports,name);
}
