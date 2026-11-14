/* Detroit: Become Human (CUSA08344) imports the God of War / Spider-Man runtime did not serve (2026-11-08).
 * Devices this machine does not have answer as a PS4 without them does (behaviour and codes after
 * shadPS4's libraries: hmd, move, camera, vr_tracker, companion, game_live_streaming, error_dialog):
 * no PS VR headset, no Move, no camera, not streaming, no companion app. libSceAudio3d is a paced
 * sink: ports, objects and queue levels behave, the frames go nowhere yet (no sound from Audio3d).
 * Every function says once in the log that it was called, so later failures can be traced. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#define SAID(name) do { static int said_; if (!__atomic_exchange_n(&said_,1,__ATOMIC_RELAXED)) { \
    printf("Detroit runtime: %s\n",name); fflush(stdout); } } while (0)
#define SAY(name,fmt,...) do { static int said_; if (!__atomic_exchange_n(&said_,1,__ATOMIC_RELAXED)) { \
    printf("Detroit runtime: %s " fmt "\n",name,__VA_ARGS__); fflush(stdout); } } while (0)

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void sleep_until_ns(uint64_t when) {
    uint64_t n=now_ns();
    if (when<=n) return;
    struct timespec d={(time_t)((when-n)/1000000000u),(long)((when-n)%1000000000u)};
    nanosleep(&d,NULL);
}

/* ---- libSceHmd: no headset (handle as shadPS4; per-handle queries fail as on firmware >= 4.50) ---- */
#define HMD_NOT_INITIALIZED ((int32_t)0x81110002)
#define HMD_HANDLE_INVALID ((int32_t)0x81110003)
#define HMD_PARAMETER_NULL ((int32_t)0x81110008)
#define HMD_PARAMETER_INVALID ((int32_t)0x81110009)
#define HMD_ALREADY_OPENED ((int32_t)0x81110006)
static int hmd_initialized; static int32_t hmd_handle, hmd_user;
static ABI int32_t hmd_initialize315(const void *param) {
    SAID("sceHmdInitialize315 (no PS VR headset)");
    if (!param) return HMD_PARAMETER_NULL;
    hmd_initialized=1; return 0;
}
static ABI int32_t hmd_open(int32_t user, int32_t type, int32_t index, const void *param) {
    SAY("sceHmdOpen","user=%d type=%d index=%d",user,type,index);
    if (!hmd_initialized) return HMD_NOT_INITIALIZED;
    if (type || index || param) return HMD_PARAMETER_INVALID;
    if (hmd_handle) return HMD_ALREADY_OPENED;
    hmd_user=user; hmd_handle=0xf000000; return hmd_handle;
}
typedef struct { uint32_t status; int32_t user; uint8_t reserve0[4]; uint32_t width, height; uint16_t r90, r120; uint8_t hmu_mount, reserve1[7]; } HmdDeviceInformation;
static ABI int32_t hmd_device_info(HmdDeviceInformation *info) {
    SAID("sceHmdGetDeviceInformation (not detected)");
    if (!info) return HMD_PARAMETER_NULL;
    if (!hmd_initialized) return HMD_NOT_INITIALIZED;
    memset(info,0,sizeof(*info)); info->status=2; /* NOT_DETECTED */ info->user=hmd_user; return 0;
}
static ABI int32_t hmd_by_handle(int32_t handle, void *out) { (void)handle; (void)out; SAID("sceHmd per-handle query: handle invalid (no headset)"); return HMD_HANDLE_INVALID; }
static ABI int32_t hmd_close(int32_t handle) {
    if (!hmd_initialized) return HMD_NOT_INITIALIZED;
    if (handle!=hmd_handle) return HMD_HANDLE_INVALID;
    hmd_handle=0; return 0;
}
static ABI int32_t hmd_terminate(void) { if (!hmd_initialized) return HMD_NOT_INITIALIZED; hmd_initialized=0; return 0; }
static ABI int32_t hmd_ok(void) { SAID("sceHmdReprojection* (no headset): accepted"); return 0; }
static ABI int32_t hmd_align(void) { return 0x100; }
static ABI int32_t hmd_garlic_size(void) { return 0x100000; }
static ABI int32_t hmd_onion_size(void) { return 0x810; }

/* ---- libSceHmdSetupDialog: the user cancels at once ---- */
static ABI int32_t dialog_ok(void) { return 0; }
static ABI int32_t dialog_finished(void) { return 3; } /* CommonDialog FINISHED */
static ABI int32_t hmd_setup_result(int32_t *result) { SAID("sceHmdSetupDialogGetResult: user canceled"); if (result) { memset(result,0,36); *result=1; } return 0; }

/* ---- libSceErrorDialog: shown and finished at once (the error is in the log) ---- */
static int error_dialog_status; /* 0 none, 1 initialized, 3 finished */
static ABI int32_t error_dialog_init(void) { error_dialog_status=1; return 0; }
static ABI int32_t error_dialog_open(const int32_t *param) {
    printf("Detroit runtime: sceErrorDialogOpen error 0x%08x (not shown)\n",param ? (uint32_t)param[2] : 0u); fflush(stdout);
    error_dialog_status=3; return 0;
}
static ABI int32_t error_dialog_status_now(void) { return error_dialog_status; }
static ABI int32_t error_dialog_terminate(void) { error_dialog_status=0; return 0; }

/* ---- libSceMove: Init works, handles are given, no controller is ever connected ---- */
static int move_initialized;
static ABI int32_t move_init(void) { SAID("sceMoveInit (no Move controllers)"); if (move_initialized) return (int32_t)0x80EE0002; move_initialized=1; return 0; }
static ABI int32_t move_open(int32_t user, int32_t type, int32_t index) {
    (void)user; (void)type; (void)index;
    if (!move_initialized) return (int32_t)0x80EE0001;
    static int32_t handle=0x30b0000; return __atomic_add_fetch(&handle,0x100,__ATOMIC_RELAXED);
}
static ABI int32_t move_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t move_no_controller(void) { return 1; } /* ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED */

/* ---- libSceCamera: no camera attached ---- */
static ABI int32_t camera_attached(int32_t index) { SAID("sceCameraIsAttached: no camera"); return index ? (int32_t)0x802E0000 : 0; }
static ABI int32_t camera_open(void) { SAID("sceCameraOpen: not connected"); return (int32_t)0x802E0010; }
static ABI int32_t camera_not_open(void) { return (int32_t)0x802E0004; }

/* ---- libSceVrTracker: not initialized (only used with a headset) ---- */
static ABI int32_t vr_tracker_none(void) { SAID("sceVrTracker*: not available (no headset)"); return (int32_t)0x81190002; }

/* ---- libSceVideoRecording: accepted, nothing is recorded ---- */
static ABI int32_t video_recording_ok(void) { SAID("sceVideoRecording*: accepted, nothing recorded"); return 0; }
static ABI int32_t video_recording_mem(void) { return 0x10000; }

/* ---- libSceCompanionUtil: no companion app, never an event ---- */
static ABI int32_t companion_ok(void) { return 0; }
static ABI int32_t companion_event(void *event) { (void)event; return (int32_t)0x80AD0008; }

/* ---- libSceGameLiveStreaming: not on air ---- */
static ABI int32_t streaming_ok(void) { return 0; }
static ABI int32_t streaming_status2(uint8_t *status) { if (status) { memset(status,0,72); *(int32_t *)status=1; } return 0; }
static ABI int32_t streaming_count(void *param, uint32_t *count) { (void)param; if (count) *count=0; return 0; }

/* ---- libSceAudio3d: a paced sink. A port keeps "queue_depth" frames of "granularity" samples at
 * 48 kHz; PortAdvance queues a frame, frames drain in real time, a blocking PortPush waits until
 * at most one frame is queued (one frame playing), as an output does. ---- */
#define A3D_INVALID_PORT ((int32_t)0x80EA0002)
#define A3D_INVALID_PARAMETER ((int32_t)0x80EA0004)
#define A3D_NOT_READY ((int32_t)0x80EA0007)
#define A3D_OUT_OF_RESOURCES ((int32_t)0x80EA0006)
typedef struct { uint64_t size_this; uint32_t granularity, rate, max_objects, queue_depth, buffer_mode, pad, num_beds; } A3dOpen;
typedef struct { int open; uint32_t granularity, depth, max_objects, objects, next_object, queued; uint64_t period_ns, drained_at; pthread_mutex_t lock; } A3dPort;
static int a3d_initialized;
static A3dPort a3d_ports[4]={{.lock=PTHREAD_MUTEX_INITIALIZER},{.lock=PTHREAD_MUTEX_INITIALIZER},{.lock=PTHREAD_MUTEX_INITIALIZER},{.lock=PTHREAD_MUTEX_INITIALIZER}};
static A3dPort *a3d_port(uint32_t id) { return id<4 && a3d_ports[id].open ? &a3d_ports[id] : NULL; }
static void a3d_drain(A3dPort *p) { /* lock held */
    uint64_t n=now_ns();
    if (!p->queued) { p->drained_at=n; return; }
    uint64_t done=(n-p->drained_at)/p->period_ns;
    if (done>=p->queued) { p->queued=0; p->drained_at=n; }
    else { p->queued-=(uint32_t)done; p->drained_at+=done*p->period_ns; }
}
static ABI int32_t a3d_initialize(int64_t reserved) {
    SAID("sceAudio3dInitialize (paced sink, no Audio3d sound yet)");
    if (reserved) return A3D_INVALID_PARAMETER;
    if (a3d_initialized) return A3D_NOT_READY;
    a3d_initialized=1; return 0;
}
static ABI int32_t a3d_terminate(void) { if (!a3d_initialized) return A3D_NOT_READY; for (int i=0;i<4;++i) a3d_ports[i].open=0; a3d_initialized=0; return 0; }
static ABI int32_t a3d_port_open(int32_t user, const A3dOpen *param, uint32_t *port) {
    if (port) *port=0xFFFFFFFFu;
    if (user!=0xFF || !param || !port) return A3D_INVALID_PARAMETER;
    if (!a3d_initialized) return A3D_NOT_READY;
    for (uint32_t i=0;i<4;++i) if (!a3d_ports[i].open) {
        A3dPort *p=&a3d_ports[i];
        pthread_mutex_lock(&p->lock);
        p->granularity=param->granularity ? param->granularity : 256;
        p->depth=param->queue_depth ? param->queue_depth : 2;
        p->max_objects=param->max_objects ? param->max_objects : 512;
        p->objects=0; p->next_object=0; p->queued=0;
        p->period_ns=(uint64_t)p->granularity*1000000000u/48000u; p->drained_at=now_ns(); p->open=1;
        pthread_mutex_unlock(&p->lock);
        printf("Detroit runtime: sceAudio3dPortOpen port %u granularity %u queue %u objects %u mode %u beds %u\n",i,p->granularity,
               p->depth,p->max_objects,param->buffer_mode,param->num_beds); fflush(stdout);
        *port=i; return 0;
    }
    return A3D_OUT_OF_RESOURCES;
}
static ABI int32_t a3d_port_close(uint32_t id) { A3dPort *p=a3d_port(id); if (!p) return A3D_INVALID_PORT; p->open=0; return 0; }
static ABI int32_t a3d_object_reserve(uint32_t id, uint32_t *object) {
    if (!object) return A3D_INVALID_PARAMETER;
    *object=0xFFFFFFFFu;
    A3dPort *p=a3d_port(id); if (!p) return A3D_INVALID_PORT;
    pthread_mutex_lock(&p->lock);
    int32_t r=0;
    if (p->objects>=p->max_objects) r=A3D_OUT_OF_RESOURCES;
    else { ++p->objects; *object=++p->next_object; }
    pthread_mutex_unlock(&p->lock);
    return r;
}
static ABI int32_t a3d_object_unreserve(uint32_t id, uint32_t object) {
    (void)object; A3dPort *p=a3d_port(id); if (!p) return A3D_INVALID_PORT;
    pthread_mutex_lock(&p->lock); if (p->objects) --p->objects; pthread_mutex_unlock(&p->lock); return 0;
}
static ABI int32_t a3d_port_ok(uint32_t id) { return a3d_port(id) ? 0 : A3D_INVALID_PORT; }
static ABI int32_t a3d_bed_write(uint32_t id, uint32_t channels, uint32_t format, const void *buffer, uint32_t samples) {
    (void)channels; (void)format; (void)samples;
    if (!a3d_port(id)) return A3D_INVALID_PORT;
    return buffer ? 0 : A3D_INVALID_PARAMETER;
}
static ABI int32_t a3d_port_advance(uint32_t id) {
    A3dPort *p=a3d_port(id); if (!p) return A3D_INVALID_PORT;
    pthread_mutex_lock(&p->lock); a3d_drain(p);
    int32_t r=0;
    if (p->queued>=p->depth) r=A3D_NOT_READY; else ++p->queued;
    pthread_mutex_unlock(&p->lock);
    return r;
}
static ABI int32_t a3d_port_push(uint32_t id, uint32_t blocking) {
    A3dPort *p=a3d_port(id); if (!p) return A3D_INVALID_PORT;
    for (;;) {
        pthread_mutex_lock(&p->lock); a3d_drain(p);
        uint32_t q=p->queued; uint64_t next=p->drained_at+p->period_ns;
        pthread_mutex_unlock(&p->lock);
        if (!blocking || q<=1) return 0;
        sleep_until_ns(next);
    }
}
static ABI int32_t a3d_port_flush(uint32_t id) {
    A3dPort *p=a3d_port(id); if (!p) return A3D_INVALID_PORT;
    for (;;) {
        pthread_mutex_lock(&p->lock); a3d_drain(p);
        uint32_t q=p->queued; uint64_t next=p->drained_at+p->period_ns;
        pthread_mutex_unlock(&p->lock);
        if (!q) return 0;
        sleep_until_ns(next);
    }
}
static ABI int32_t a3d_queue_level(uint32_t id, uint32_t *level, uint32_t *available) {
    A3dPort *p=a3d_port(id); if (!p) return A3D_INVALID_PORT;
    if (!level && !available) return A3D_INVALID_PARAMETER;
    pthread_mutex_lock(&p->lock); a3d_drain(p);
    if (level) *level=p->queued;
    if (available) *available=p->queued<p->depth ? p->depth-p->queued : 0;
    pthread_mutex_unlock(&p->lock);
    return 0;
}

/* ---- libSceAudioOut mastering, system/user services, save data, IME, sockets ---- */
static ABI int32_t mastering_ok(void) { SAID("sceAudioOutMastering*: accepted"); return 0; }
static ABI int32_t user_initialize2(int32_t priority, uint64_t affinity) { SAY("sceUserServiceInitialize2","priority=%d affinity=0x%llx",priority,(unsigned long long)affinity); return 0; }
static ABI int32_t disable_music_player(void) { return 0; }
static ABI int32_t param_get_string(int32_t id, char *buffer, uint64_t size) {
    SAY("sceSystemServiceParamGetString","id=%d",id);
    if (!buffer || !size) return (int32_t)0x80A10003;
    snprintf(buffer,(size_t)size,"%s","Player"); return 0;
}
static ABI int32_t save_backup(const void *param) { (void)param; SAID("sceSaveDataBackup: accepted (no backup kept)"); return 0; }
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SaveMountInfo;
static ABI int32_t save_mount_info(const void *mount_point, SaveMountInfo *info) {
    SAID("sceSaveDataGetMountInfo: 1 GiB, mostly free");
    if (!mount_point || !info) return (int32_t)0x809F0000; /* PARAMETER */
    memset(info,0,sizeof(*info)); info->blocks=32768; info->free_blocks=32000; return 0;
}
static ABI int32_t ime_panel_size_extended(const void *param, const void *extended, uint32_t *width, uint32_t *height) {
    (void)extended;
    if (!param || !width || !height) return (int32_t)0x80BC0003;
    *width=0x780; *height=0x438; return 0;
}
static ABI int32_t socket_not_connected(void) { *runtime_errno()=57; /* ENOTCONN */ return -1; }

/* ---- libSceNpToolkit2: offline. Requests construct and destruct, Core init/term succeed, a
 * profiles response is empty. ---- */
static ABI void tk_nothing(void *self) { (void)self; }
static ABI int32_t tk_ok(const void *request) { (void)request; SAID("NpToolkit2 Core init/term: offline, accepted"); return 0; }
static ABI const void *tk_profiles(const void *self) { (void)self; static uint64_t empty[512]; return empty; }

static const RuntimeExport exports[]={
    {"sceHmdInitialize315",hmd_initialize315}, {"sceHmdOpen",hmd_open}, {"sceHmdClose",hmd_close},
    {"sceHmdTerminate",hmd_terminate}, {"sceHmdGetDeviceInformation",hmd_device_info},
    {"sceHmdGetDeviceInformationByHandle",hmd_by_handle}, {"sceHmdGetFieldOfView",hmd_by_handle},
    {"sceHmdReprojectionInitialize",hmd_ok}, {"sceHmdReprojectionFinalize",hmd_ok},
    {"sceHmdReprojectionQueryGarlicBuffAlign",hmd_align}, {"sceHmdReprojectionQueryGarlicBuffSize",hmd_garlic_size},
    {"sceHmdReprojectionQueryOnionBuffAlign",hmd_align}, {"sceHmdReprojectionQueryOnionBuffSize",hmd_onion_size},
    {"sceHmdReprojectionSetDisplayBuffers",hmd_ok}, {"sceHmdReprojectionUnsetDisplayBuffers",hmd_ok},
    {"sceHmdReprojectionSetUserEventStart",hmd_ok}, {"sceHmdReprojectionStart",hmd_ok},
    {"sceHmdReprojectionStart2dVr",hmd_ok}, {"sceHmdReprojectionStop",hmd_ok},
    {"sceHmdSetupDialogInitialize",dialog_ok}, {"sceHmdSetupDialogOpen",dialog_ok},
    {"sceHmdSetupDialogUpdateStatus",dialog_finished}, {"sceHmdSetupDialogGetResult",hmd_setup_result},
    {"sceHmdSetupDialogTerminate",dialog_ok},
    {"sceErrorDialogInitialize",error_dialog_init}, {"sceErrorDialogOpen",error_dialog_open},
    {"sceErrorDialogUpdateStatus",error_dialog_status_now}, {"sceErrorDialogTerminate",error_dialog_terminate},
    {"sceMoveInit",move_init}, {"sceMoveOpen",move_open}, {"sceMoveClose",move_close},
    {"sceMoveReadStateLatest",move_no_controller}, {"sceMoveSetLightSphere",move_no_controller},
    {"sceMoveResetLightSphere",move_no_controller},
    {"sceCameraIsAttached",camera_attached}, {"sceCameraOpen",camera_open}, {"sceCameraClose",camera_not_open},
    {"sceCameraGetDeviceInfo",camera_not_open}, {"sceCameraGetFrameData",camera_not_open},
    {"sceCameraSetConfig",camera_not_open}, {"sceCameraSetVideoSync",camera_not_open},
    {"sceCameraStart",camera_not_open}, {"sceCameraStop",camera_not_open},
    {"sceVrTrackerInit",vr_tracker_none}, {"sceVrTrackerTerm",vr_tracker_none}, {"sceVrTrackerQueryMemory",vr_tracker_none},
    {"sceVrTrackerRegisterDevice",vr_tracker_none}, {"sceVrTrackerUnregisterDevice",vr_tracker_none},
    {"sceVrTrackerGpuSubmit",vr_tracker_none}, {"sceVrTrackerGpuWaitAndCpuProcess",vr_tracker_none},
    {"sceVrTrackerGetResult",vr_tracker_none}, {"sceVrTrackerGetTime",vr_tracker_none},
    {"sceVrTrackerRecalibrate",vr_tracker_none}, {"sceVrTrackerUpdateMotionSensorData",vr_tracker_none},
    {"sceVideoRecordingOpen2",video_recording_ok}, {"sceVideoRecordingClose",video_recording_ok},
    {"sceVideoRecordingStart",video_recording_ok}, {"sceVideoRecordingStop",video_recording_ok},
    {"sceVideoRecordingGetStatus",video_recording_ok}, {"sceVideoRecordingQueryMemSize2",video_recording_mem},
    {"sceCompanionUtilInitialize",companion_ok}, {"sceCompanionUtilOptParamInitialize",companion_ok},
    {"sceCompanionUtilTerminate",companion_ok}, {"sceCompanionUtilGetEvent",companion_event},
    {"sceGameLiveStreamingInitialize",streaming_ok}, {"sceGameLiveStreamingTerminate",streaming_ok},
    {"sceGameLiveStreamingGetCurrentStatus2",streaming_status2}, {"sceGameLiveStreamingEnableLiveStreaming",streaming_ok},
    {"sceGameLiveStreamingPermitLiveStreaming",streaming_ok}, {"sceGameLiveStreamingPermitServerSideRecording",streaming_ok},
    {"sceGameLiveStreamingClearPresetSocialFeedbackCommands",streaming_ok},
    {"sceGameLiveStreamingSetPresetSocialFeedbackCommands",streaming_ok},
    {"sceGameLiveStreamingClearSocialFeedbackMessages",streaming_ok},
    {"sceGameLiveStreamingGetSocialFeedbackMessages",streaming_count},
    {"sceGameLiveStreamingGetSocialFeedbackMessagesCount",streaming_count},
    {"sceAudio3dInitialize",a3d_initialize}, {"sceAudio3dTerminate",a3d_terminate},
    {"sceAudio3dPortOpen",a3d_port_open}, {"sceAudio3dPortClose",a3d_port_close},
    {"sceAudio3dObjectReserve",a3d_object_reserve}, {"sceAudio3dObjectUnreserve",a3d_object_unreserve},
    {"sceAudio3dObjectSetAttributes",a3d_port_ok}, {"sceAudio3dPortSetAttribute",a3d_port_ok},
    {"sceAudio3dBedWrite",a3d_bed_write}, {"sceAudio3dPortAdvance",a3d_port_advance},
    {"sceAudio3dPortPush",a3d_port_push}, {"sceAudio3dPortFlush",a3d_port_flush},
    {"sceAudio3dPortGetQueueLevel",a3d_queue_level},
    {"sceAudioOutMasteringInit",mastering_ok}, {"sceAudioOutMasteringSetParam",mastering_ok},
    {"sceUserServiceInitialize2",user_initialize2}, {"sceSystemServiceDisableMusicPlayer",disable_music_player},
    {"sceSystemServiceParamGetString",param_get_string},
    {"sceSaveDataBackup",save_backup}, {"sceSaveDataGetMountInfo",save_mount_info},
    {"sceImeDialogGetPanelSizeExtended",ime_panel_size_extended},
    {"getpeername",socket_not_connected}, {"getsockopt",socket_not_connected},
    {"_ZN3sce7Toolkit2NP2V211UserProfile7Request13GetNpProfilesC1Ev",tk_nothing},
    {"_ZN3sce7Toolkit2NP2V211UserProfile7Request13GetNpProfilesD1Ev",tk_nothing},
    {"_ZN3sce7Toolkit2NP2V24Core4initERKNS3_7Request10InitParamsE",tk_ok},
    {"_ZN3sce7Toolkit2NP2V24Core4termERKNS3_7Request10TermParamsE",tk_ok},
    {"_ZN3sce7Toolkit2NP2V24Core7Request10InitParamsC1Ev",tk_nothing},
    {"_ZN3sce7Toolkit2NP2V24Core7Request10InitParamsD1Ev",tk_nothing},
    {"_ZN3sce7Toolkit2NP2V24Core7Request10TermParamsC1Ev",tk_nothing},
    {"_ZN3sce7Toolkit2NP2V24Core7Request10TermParamsD1Ev",tk_nothing},
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_11UserProfile10NpProfilesEE3getEv",tk_profiles},
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_11UserProfile10NpProfilesEEC1Ev",tk_nothing},
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_11UserProfile10NpProfilesEED1Ev",tk_nothing},
};
uintptr_t runtime_detroit_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
