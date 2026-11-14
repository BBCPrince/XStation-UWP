/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - save-slot selection uses a visible picker; progress is controlled by the game;
 *   - the whole package is installed (PlayGo reports every chunk local).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include "runtime_text_input.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#define USER_ID 1
#define ORBIS_OK 0
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define TROPHY_INVALID ((int32_t)0x80551604)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }

static struct { int initialized; void *pool; uint64_t size; } remoteplay;
static ABI int32_t remoteplay_initialize(void *pool,uint64_t size) {
    if (!pool || !size) return (int32_t)UINT32_C(0x80020016);
    pthread_mutex_lock(&lock);
    remoteplay.initialized=1;
    remoteplay.pool=pool;
    remoteplay.size=size;
    pthread_mutex_unlock(&lock);
    note("RemotePlay initialized in offline mode; no connected remote client");
    return 0;
}
static ABI int32_t remoteplay_status(int32_t user,int32_t *status) {
    if (!status) return (int32_t)UINT32_C(0x8002000e);
    if (user<0) return (int32_t)UINT32_C(0x80020016);
    pthread_mutex_lock(&lock);
    int initialized=remoteplay.initialized;
    if (initialized) *status=0;
    pthread_mutex_unlock(&lock);
    return initialized ? 0 : (int32_t)UINT32_C(0x80020006);
}
static ABI int32_t remoteplay_terminate(void) {
    pthread_mutex_lock(&lock);
    memset(&remoteplay,0,sizeof(remoteplay));
    pthread_mutex_unlock(&lock);
    return 0;
}


/* ---- UserService ---- */
static int login_event_pending=1;
static ABI int32_t user_initialize(const void *params) { (void)params; note("UserService initialized (user 1 logged in)"); return 0; }
static ABI int32_t user_terminate(void) { return 0; }
static ABI int32_t user_initial(int32_t *id) { if (!id) return USER_INVALID_ARGUMENT; *id=USER_ID; return 0; }
static ABI int32_t user_list(int32_t *ids) {
    if (!ids) return USER_INVALID_ARGUMENT;
    ids[0]=USER_ID; ids[1]=ids[2]=ids[3]=-1; return 0;
}
static ABI int32_t user_name(int32_t id,char *name,uint64_t size) {
    if (id!=USER_ID || !name) return USER_INVALID_ARGUMENT;
    const char *value=getenv("BB_USER_NAME") ? getenv("BB_USER_NAME") : "Hunter";
    if (strlen(value)+1>size) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name,value); return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int pending=login_event_pending; login_event_pending=0;
    pthread_mutex_unlock(&lock);
    if (!pending) return USER_NO_EVENT;
    event[0]=0; event[1]=USER_ID; /* LOGIN */
    return 0;
}

/* ---- SystemService ---- */
static int language(void) { const char *v=getenv("BB_LANGUAGE"); return v ? atoi(v) : 1; }
static ABI int32_t system_param(int32_t id,int32_t *value) {
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=language(); break;           /* language (1 = English US, 8 = Russian) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
#ifdef _WIN32
    case 4: *value=(int32_t)(compat_utc_offset(time(NULL))/60); break;
#else
    case 4: { time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60); break; }
#endif
    case 5: *value=0; break;                    /* summer time */
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); exit(21);
    }
    return 0;
}
static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    return 0;
}
static ABI int32_t system_event(void *event) { (void)event; return SYSTEM_NO_EVENT; }
static ABI int32_t hide_splash(void) { note("SystemService: splash screen hidden"); return 0; }
/* Quit to Desktop (scripts/quit_menu.py): the patched menus call the browser with these
 * addresses. "bb:q" (title menu) closes the game; "bb:p" (System menu, in the world) marks the
 * Exit Game that follows, and "bb:d" (its confirmation dialog) answers 1 to show the Quit to
 * Desktop text and arms the quit. The game then saves and leaves the world, and closes as soon
 * as it starts the intro logos (a menu/logo_ file opens) with no save data mounted, or when the
 * title screen is built ("bb:t", with the intro skipped). An unanswered dialog disarms it after
 * two minutes. */
static int quit_pending, quit_waiting;
static time_t quit_armed;
static pthread_mutex_t quit_lock=PTHREAD_MUTEX_INITIALIZER;
static void quit_game(void) {
    note("Quit to Desktop");
    SDL_Event quit={.type=SDL_EVENT_QUIT};
    SDL_PushEvent(&quit);
}
static int quit_ready(void) { return quit_armed && time(NULL)-quit_armed<120; }
void runtime_quit_on_open(const char *guest) {
    if (!quit_armed || !guest || !strstr(guest,"/menu/logo_")) return;
    pthread_mutex_lock(&quit_lock);
    if (quit_ready()) {
        quit_armed=0;
        /* A save still writing finishes first (runtime_quit_on_save_done). */
        if (runtime_savedata_mounted()) quit_waiting=1; else quit_game();
    }
    pthread_mutex_unlock(&quit_lock);
}
void runtime_quit_on_save_done(void) {
    pthread_mutex_lock(&quit_lock);
    if (quit_waiting && !runtime_savedata_mounted()) { quit_waiting=0; quit_game(); }
    pthread_mutex_unlock(&quit_lock);
}
/* `arg`: the browser's second argument, a number for the port's own messages. */
static ABI int32_t launch_browser(const char *url, intptr_t arg) {
    /* Rebirth at the Doll (runtime_rebirth.c, scripts/rebirth.py). */
    if (url && !strcmp(url,"bb:b")) return runtime_rebirth();
    if (url && !strcmp(url,"bb:f")) return runtime_rebirth_free_level((int)arg);
    if (url && !strcmp(url,"bb:q")) { quit_game(); return 0; }
    if (url && !strcmp(url,"bb:p")) { quit_pending=1; return 0; }
    if (url && !strcmp(url,"bb:d")) {
        pthread_mutex_lock(&quit_lock);
        const int armed=quit_pending;
        quit_pending=0;
        quit_armed=armed ? time(NULL) : 0;
        pthread_mutex_unlock(&quit_lock);
        return armed;
    }
    if (url && !strcmp(url,"bb:t")) {
        pthread_mutex_lock(&quit_lock);
        if (quit_ready() || quit_waiting) quit_game();
        quit_armed=0;
        quit_waiting=0;
        pthread_mutex_unlock(&quit_lock);
        return 0;
    }
    note("SystemService: web browser request ignored (offline)");
    return 0;
}

/* ---- NetCtl / Net / Http / Ssl: no network ---- */
static int32_t net_errno;
static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_unreachable(void) { net_errno=51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_epoll_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
static ABI int32_t net_resolver_destroy(int id) { (void)id; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { net_errno=47; return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { net_errno=47; return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}
static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state=0; return 0; }
static ABI int32_t netctl_info(int code,void *info) { (void)code; (void)info; return NET_CTL_NOT_CONNECTED; }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int cid) { (void)cid; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}
static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
/* Objects are created so setup code proceeds; any transfer fails as unplugged. */
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x80431077; /* HTTP INVALID_VALUE */
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
#ifdef _WIN32
    if (timeout>0) compat_sleep_ns((uint64_t)timeout*1000);
#else
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
#endif
    return 0; /* no events: nothing is in flight */
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user; *state=1; /* SIGNED_OUT */
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* ---- Voice chat: ports exist, carry no audio ---- */
static ABI int32_t voice_port(void *param,uint32_t *port) { (void)param; if (!port) return (int32_t)0x8029000b; *port=(uint32_t)new_id(); return 0; }
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t port,const void *data,uint32_t *size) { (void)port; (void)data; (void)size; return 0; }
static ABI int32_t voice_info(uint32_t port,uint32_t *info) {
    (void)port;
    if (!info) return (int32_t)0x8029000b;
    memset(info,0,40); /* type, state=unconnected, no bytes available */
    return 0;
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
typedef struct { const char *name; int initialized, status; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0},{"MsgDialog",0,0},{"SaveDataDialog",0,0},
                         {"NpProfileDialog",0,0},{"NpCommerceDialog",0,0},{"ImeDialog",0,0}};
static int common_initialized;
static ABI int32_t common_init(void) { common_initialized=1; return 0; }
static int32_t dialog_init(int i) {
    if (dialogs[i].initialized) return (int32_t)0x80B80004;
    dialogs[i].initialized=1; dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",dialogs[i].name);
    dialogs[i].status=3; return 0;
}
static int32_t dialog_status(int i) { return dialogs[i].status; }
static int32_t dialog_term(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(msg,1) DIALOG(profile,3) DIALOG(commerce,4)
/* List and system confirmations wait for explicit input through the existing
 * UWP/desktop dialog bridge. Save progress remains controlled by the game. */
typedef struct { uint64_t size; uint8_t reserved[36]; uint32_t magic; } SaveDialogBase;
typedef struct { uint32_t type,pad; const char *message; uint32_t system; uint8_t reserved[28]; } SaveProgress;
typedef struct { uint32_t type,pad; uint64_t value; uint8_t reserved[32]; } SaveSystemMessage;
_Static_assert(sizeof(SaveSystemMessage)==48,"Save system message ABI");
typedef struct {
    SaveDialogBase base;
    int32_t size; uint32_t mode,type,pad;
    void *animation,*items,*user_message,*system_message,*error;
    const SaveProgress *progress;
    void *user,*option; uint8_t reserved[24];
} SaveDialogParam;
typedef struct {
    uint32_t mode,result,button,pad; void *directory,*parameter,*user; uint8_t reserved[32];
} SaveDialogResult;
typedef struct {
    int32_t user,pad; const char *title; const char (*directories)[32];
    uint32_t count,pad2; const void *new_item; uint32_t focus,pad3;
    const char *focus_directory; uint32_t style; uint8_t reserved[32];
} SaveDialogItems;
_Static_assert(sizeof(SaveDialogParam)==152,"Save dialog parameter ABI");
_Static_assert(sizeof(SaveDialogResult)==72,"Save dialog result ABI");
_Static_assert(sizeof(SaveDialogItems)==96,"Save dialog items ABI");
static pthread_mutex_t save_dialog_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t save_progress_value;
static void *save_dialog_user;
static uint32_t save_dialog_mode=5,save_dialog_result,save_dialog_button,save_system_kind;
static int32_t save_dialog_user_id;
static char save_dialog_title[10],save_selection[32],save_choices[33][32],save_prompt[2048];
static unsigned save_choice_count;
/* BB_SAVE_PICK=<save directory or list number>: scripted runs pick that save in the list
 * without the host dialog (a XAML dialog the pad injection cannot reach). */
static unsigned save_auto_pick;
static int save_system_begin(const SaveDialogParam *param) {
    const SaveSystemMessage *message=param->system_message;
    const SaveDialogItems *items=param->items;
    if (!message) return (int32_t)0x80B8000D;
    if (param->type<1 || param->type>3) return (int32_t)0x80B8000A;
    /* CONFIRM, OVERWRITE and FINISHED only. Unknown system messages retain an
     * explicit error instead of silently allowing an operation. */
    if (message->type!=2 && message->type!=3 && message->type!=7) {
        fprintf(stderr,"SaveDataDialog: unsupported system message %u\n",message->type);
        return (int32_t)0x80B8000F;
    }
    if (message->type==3 && param->type!=1) return (int32_t)0x80B8000A;
    char selection[32]={0},title[10]={0};
    int32_t user=0;
    if (items) {
        if (items->user<0 || items->count>1 || (items->count && !items->directories))
            return (int32_t)0x80B8000A;
        if (items->count) {
            if (!memchr(items->directories[0],0,32) || !items->directories[0][0])
                return (int32_t)0x80B8000A;
            memcpy(selection,items->directories[0],sizeof(selection));
        }
        if (items->title) {
            if (!memchr(items->title,0,sizeof(title))) return (int32_t)0x80B8000A;
            memcpy(title,items->title,sizeof(title));
        }
        user=items->user;
    }
    if (message->type==3 && !selection[0]) return (int32_t)0x80B8000A;
    const char *initial="YES";
    if (message->type==3) {
        snprintf(save_prompt,sizeof(save_prompt),
                 "Overwrite saved game?\nSave slot: %s\nConfirm replaces this save. Cancel keeps the existing save.\n"
                 "YES is preselected; you may enter NO to keep it.",selection);
    } else if (message->type==2) {
        const char *action=param->type==1 ? "Save" : param->type==2 ? "Load" : "Delete";
        snprintf(save_prompt,sizeof(save_prompt),
                 "%s saved game?\nSave slot: %s\nConfirm continues. Cancel leaves saves unchanged.\n"
                 "YES is preselected; you may enter NO to decline.",action,selection[0] ? selection : "new save");
    } else {
        const char *action=param->type==1 ? "Saving finished" : param->type==2 ? "Loading finished" : "Deletion finished";
        snprintf(save_prompt,sizeof(save_prompt),"%s.\nSave slot: %s\nSelect Confirm to return to the game.",
                 action,selection[0] ? selection : "current save");
        initial="";
    }
    if (!runtime_text_input_begin(initial,save_prompt)) return (int32_t)0x80B8000F;
    memcpy(save_selection,selection,sizeof(save_selection));
    memcpy(save_dialog_title,title,sizeof(save_dialog_title));
    save_dialog_user_id=user;
    save_system_kind=message->type;
    printf("SaveDataDialog: system message %u awaiting input for %s\n",message->type,
           selection[0] ? selection : "new save");
    return 0;
}
static int save_list_begin(const SaveDialogParam *param) {
    const SaveDialogItems *items=param->items;
    if (!items || items->user<0 || items->count>32 || (items->count && !items->directories))
        return (int32_t)0x80B8000A;
    if (param->type!=1 && param->type!=2) return (int32_t)0x80B8000F;
    save_choice_count=0;
    memset(save_choices,0,sizeof(save_choices));
    size_t used=(size_t)snprintf(save_prompt,sizeof(save_prompt),"%s save slot\nType a number, then confirm. Cancel leaves saves unchanged.\n",
        param->type==1 ? "Choose" : "Load");
    if (items->new_item && param->type==1) {
        ++save_choice_count;
        used+=(size_t)snprintf(save_prompt+used,sizeof(save_prompt)-used,"1: New save\n");
    }
    for (uint32_t i=0;i<items->count;++i) {
        if (!memchr(items->directories[i],0,32) || !items->directories[i][0]) return (int32_t)0x80B8000A;
        memcpy(save_choices[save_choice_count],items->directories[i],32);
        ++save_choice_count;
        int written=snprintf(save_prompt+used,sizeof(save_prompt)-used,"%u: %s\n",save_choice_count,items->directories[i]);
        if (written<0 || (size_t)written>=sizeof(save_prompt)-used) return (int32_t)0x80B8000A;
        used+=(size_t)written;
    }
    if (!save_choice_count) return (int32_t)0x80B8000F;
    save_dialog_user_id=items->user;
    memset(save_dialog_title,0,sizeof(save_dialog_title));
    if (items->title) {
        if (!memchr(items->title,0,sizeof(save_dialog_title))) return (int32_t)0x80B8000A;
        memcpy(save_dialog_title,items->title,sizeof(save_dialog_title));
    }
    const char *pick=getenv("BB_SAVE_PICK");
    save_auto_pick=0;
    if (pick && *pick && param->type==2) { /* load lists only: never picks where to save */
        for (unsigned i=0;i<save_choice_count;++i)
            if (!strcmp(save_choices[i],pick)) save_auto_pick=i+1;
        if (!save_auto_pick) {
            char *end=NULL; unsigned long n=strtoul(pick,&end,10);
            if (end && !*end && n>=1 && n<=save_choice_count) save_auto_pick=(unsigned)n;
        }
        printf("SaveDataDialog: %s list of %u, BB_SAVE_PICK=%s -> %u\n",param->type==1 ? "save" : "load",
               save_choice_count,pick,save_auto_pick);
        if (save_auto_pick) return 0;
    }
    if (!runtime_text_input_begin("1",save_prompt)) return (int32_t)0x80B8000F;
    return 0;
}
static void save_poll_selection(void) {
    if (dialogs[2].status!=2 || (save_dialog_mode!=1 && save_dialog_mode!=3)) return;
    if (save_dialog_mode==1 && save_auto_pick) {
        memcpy(save_selection,save_choices[save_auto_pick-1],sizeof(save_selection));
        save_auto_pick=0;
        save_dialog_result=0; dialogs[2].status=3;
        printf("SaveDataDialog: confirmed %s (BB_SAVE_PICK)\n",save_selection[0] ? save_selection : "new save");
        return;
    }
    char text[64]; int state=runtime_text_input_poll(text,sizeof(text));
    if (!state) return;
    if (state==2) { save_dialog_result=1; save_dialog_button=0; dialogs[2].status=3; return; }
    if (save_dialog_mode==3) {
        if (save_system_kind==7 || !SDL_strcasecmp(text,"YES") || !strcmp(text,"1")) {
            save_dialog_result=0; save_dialog_button=1; dialogs[2].status=3;
        } else if (!SDL_strcasecmp(text,"NO") || !strcmp(text,"2")) {
            save_dialog_result=0; save_dialog_button=2; dialogs[2].status=3;
        } else {
            if (!runtime_text_input_begin("",save_prompt)) {
                save_dialog_result=1; save_dialog_button=0; dialogs[2].status=3;
            }
            return;
        }
        printf("SaveDataDialog: system message %u answered %s\n",save_system_kind,save_dialog_button==1 ? "YES" : "NO");
        return;
    }
    char *end=NULL; unsigned long number=strtoul(text,&end,10);
    if (!text[0] || !end || *end || number<1 || number>save_choice_count) {
        if (!runtime_text_input_begin("1",save_prompt)) { save_dialog_result=1; dialogs[2].status=3; }
        return;
    }
    memcpy(save_selection,save_choices[number-1],sizeof(save_selection));
    save_dialog_result=0; dialogs[2].status=3;
    printf("SaveDataDialog: confirmed %s\n",save_selection[0] ? save_selection : "new save");
}
static ABI int32_t save_init(void) {
    if (!common_initialized) return (int32_t)0x80B80001;
    pthread_mutex_lock(&save_dialog_lock);
    int32_t result=dialog_init(2);
    if (!result) { save_progress_value=0; save_dialog_user=NULL; save_dialog_button=save_system_kind=0; }
    pthread_mutex_unlock(&save_dialog_lock);
    return result;
}
static ABI int32_t save_open(const SaveDialogParam *param) {
    pthread_mutex_lock(&save_dialog_lock);
    int32_t result=0;
    if (dialogs[2].status!=1 && dialogs[2].status!=3) result=(int32_t)0x80B80006;
    else if (!param) result=(int32_t)0x80B8000D;
    else if (param->size!=sizeof(*param) || param->base.size!=sizeof(param->base)) result=(int32_t)0x80B8000A;
    else if (param->mode==1) {
        result=save_list_begin(param);
        if (!result) {
            save_dialog_mode=1; save_dialog_result=save_dialog_button=0; memset(save_selection,0,sizeof(save_selection));
            save_dialog_user=param->user; dialogs[2].status=2;
        }
    } else if (param->mode==3) {
        result=save_system_begin(param);
        if (!result) {
            save_dialog_mode=3; save_dialog_result=save_dialog_button=0;
            save_dialog_user=param->user; dialogs[2].status=2;
        }
    } else if (param->mode!=5) {
        fprintf(stderr,"SaveDataDialog: mode %u requires an unimplemented picker/confirmation UI\n",param->mode);
        result=(int32_t)0x80B8000F;
    } else if (!param->progress) result=(int32_t)0x80B8000D;
    else if (param->progress->type) result=(int32_t)0x80B8000A;
    else {
        dialogs[2].status=2; save_dialog_mode=5; save_dialog_result=save_dialog_button=0;
        save_progress_value=0; save_dialog_user=param->user;
        puts("SaveDataDialog: progress started (runtime log; no system dialog drawn)");
    }
    pthread_mutex_unlock(&save_dialog_lock);
    return result;
}
static ABI int32_t save_status(void) {
    pthread_mutex_lock(&save_dialog_lock); save_poll_selection(); int status=dialogs[2].status;
    pthread_mutex_unlock(&save_dialog_lock); return status;
}
static ABI int32_t save_progress(uint32_t target,uint32_t rate) {
    pthread_mutex_lock(&save_dialog_lock);
    int32_t result=0;
    if (dialogs[2].status!=2) result=(int32_t)0x80B8000B;
    else if (save_dialog_mode!=5) result=(int32_t)0x80B8000F;
    else if (target) result=(int32_t)0x80B8000A;
    else { save_progress_value=rate; printf("SaveDataDialog: progress %u\n",save_progress_value); }
    pthread_mutex_unlock(&save_dialog_lock); return result;
}
static ABI int32_t save_close(void) {
    pthread_mutex_lock(&save_dialog_lock);
    int32_t result=0;
    if (dialogs[2].status!=2) result=(int32_t)0x80B8000B;
    else if (save_dialog_mode!=5) result=(int32_t)0x80B8000F;
    else { dialogs[2].status=3; puts("SaveDataDialog: progress closed by game"); }
    pthread_mutex_unlock(&save_dialog_lock); return result;
}
static ABI int32_t save_result(SaveDialogResult *out) {
    pthread_mutex_lock(&save_dialog_lock);
    save_poll_selection();
    int32_t result=0;
    if (dialogs[2].status!=3) result=(int32_t)0x80B80005;
    else if (!out) result=(int32_t)0x80B8000D;
    else {
        out->mode=save_dialog_mode; out->result=save_dialog_result; out->button=save_dialog_button; out->user=save_dialog_user;
        if ((save_dialog_mode==1 || save_dialog_mode==3) && !save_dialog_result) {
            if (out->directory) memcpy(out->directory,save_selection,sizeof(save_selection));
            if (out->parameter && save_selection[0]) {
                unsigned char info[1328];
                if (!runtime_savedata_load_info(save_dialog_user_id,save_dialog_title,save_selection,info,sizeof(info)))
                    memcpy(out->parameter,info,sizeof(info));
            }
        }
    }
    pthread_mutex_unlock(&save_dialog_lock); return result;
}
static ABI int32_t save_ready(void) { return 1; }
static ABI int32_t save_term(void) {
    pthread_mutex_lock(&save_dialog_lock);
    if (dialogs[2].status==2 && (save_dialog_mode==1 || save_dialog_mode==3)) {
        pthread_mutex_unlock(&save_dialog_lock); return (int32_t)0x80B80008;
    }
    int32_t result=dialog_term(2);
    if (!result) { save_progress_value=0; save_dialog_user=NULL; }
    pthread_mutex_unlock(&save_dialog_lock); return result;
}
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int running, finished, end_status; uint16_t *buffer; uint32_t max_length; } ime;
static size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->buffer || !param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (ime.running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&ime,0,sizeof(ime));
    ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=getenv("BB_IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    /* An empty field starts with the player name (launcher) or "Hunter": a controller alone
     * can accept it (the port has no on-screen keyboard). */
    const char *name=getenv("BB_USER_NAME");
    const char *start=initial[0] ? initial : name && *name ? name : "Hunter";
    if (!runtime_text_input_begin(start,prompt[0] ? prompt : "Text")) ime_complete(0,start);
    else printf("Runtime: ImeDialog opened: shown on screen; keyboard Enter or Cross accepts, Esc or Circle cancels\n");
    return 0;
}
static ABI int32_t ime_status(void) {
    if (ime.running) {
        char text[512];
        int state=runtime_text_input_poll(text,sizeof(text));
        if (state) ime_complete(state==1 ? 0 : 1,text);
    }
    return ime.running ? 1 : ime.finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (result) *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&ime,0,sizeof(ime)); return 0; }

/* ---- Trophies: accepted locally, recorded in the log ---- */
static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) {
    (void)user; (void)label; (void)options;
    if (!ctx) return TROPHY_INVALID;
    *ctx=new_id(); return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) { if (!handle) return TROPHY_INVALID; *handle=new_id(); return 0; }
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) { (void)ctx; (void)handle; (void)options; return 0; }
static uint32_t trophy_be32(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t handle,uint32_t *flags,uint32_t *count) {
    if (ctx<1 || handle<1 || !flags || !count) return TROPHY_INVALID;
    int fd=(int)runtime_file_open("/app0/sce_sys/trophy/trophy00.trp",0,0);
    if (fd<0) return TROPHY_INVALID;
    unsigned char header[96],entry[64],catalog[128]={0};
    int valid=runtime_file_read(fd,header,sizeof(header))==sizeof(header) &&
              trophy_be32(header)==UINT32_C(0xdca24d00) && trophy_be32(header+4)==3 &&
              trophy_be32(header+20)==64;
    uint32_t entries=valid ? trophy_be32(header+16) : 0,total=0;
    if (!entries || entries>4096) valid=0;
    for (uint32_t i=0;valid && i<entries;++i) {
        if (runtime_file_read(fd,entry,sizeof(entry))!=sizeof(entry)) { valid=0; break; }
        if (memcmp(entry,"TROP",4) || memcmp(entry+7,".PNG",4) || entry[11]) continue;
        if (entry[4]<'0' || entry[4]>'9' || entry[5]<'0' || entry[5]>'9' || entry[6]<'0' || entry[6]>'9') continue;
        uint32_t id=(entry[4]-'0')*100+(entry[5]-'0')*10+entry[6]-'0';
        if (id<128 && !catalog[id]) { catalog[id]=1; ++total; }
    }
    runtime_file_close(fd);
    if (!valid) return TROPHY_INVALID;
    char path[600];
    snprintf(path,sizeof(path),"%s/trophies.log",runtime_file_user_dir());
    pthread_mutex_lock(&lock);
    memset(flags,0,4*sizeof(*flags));
    FILE *log=fopen(path,"r");
    if (log) {
        int id; long long timestamp;
        while (fscanf(log,"%d %lld",&id,&timestamp)==2)
            if (id>=0 && id<128 && catalog[id]) flags[id/32]|=UINT32_C(1)<<(id%32);
        fclose(log);
    }
    *count=total;
    pthread_mutex_unlock(&lock);
    printf("Runtime: trophy unlock state loaded (%u catalog entries)\n",total);
    return 0;
}
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) {
    (void)ctx; (void)handle;
    /* <user>/trophies.log ("id unix-time" per line) is read by the launcher's Trophies page,
     * also when the game was started without it (shortcut, Steam). */
    if (id>=0 && id<128) {
        char path[600];
        snprintf(path,sizeof(path),"%s/trophies.log",runtime_file_user_dir());
        pthread_mutex_lock(&lock);
        FILE *log=fopen(path,"a");
        if (log) { fprintf(log,"%d %lld\n",id,(long long)time(NULL)); fclose(log); }
        pthread_mutex_unlock(&lock);
    }
    printf("Runtime: trophy %d unlocked\n",id);
    if (platinum) *platinum=-1;
    return 0;
}
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    (void)ctx; (void)handle;
    if (details) { uint64_t size; memcpy(&size,details,8); memset((char *)details+8,0,size>8 && size<4096 ? size-8 : 0); }
    if (data) { uint64_t size; memcpy(&size,data,8); memset((char *)data+8,0,size>8 && size<4096 ? size-8 : 0); }
    return 0;
}
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) {
    (void)id; return trophy_game_info(ctx,handle,details,data);
}

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks=-1;
static ABI int32_t playgo_init(const void *params) {
    (void)params;
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[12];
    playgo_chunks=1;
    if (fd>=0) {
        if (runtime_file_read(fd,header,sizeof(header))==sizeof(header)) playgo_chunks=header[10]|header[11]<<8;
        runtime_file_close(fd);
    }
    printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n",playgo_chunks);
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle,const void *param) {
    (void)param;
    if (!handle) return PLAYGO_BAD_POINTER;
    playgo_handle=1; *handle=1; return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle,uint16_t *ids,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=(uint32_t)playgo_chunks; return 0; }
    uint32_t n=count<(uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n; return 0;
}
static ABI int32_t playgo_locus(int32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
        loci[i]=3; /* LocalFast */
    }
    return 0;
}
static ABI int32_t playgo_speed(int32_t handle,int32_t speed) { (void)speed; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE; }

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size; (void)result; return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: mouse, microphone, voice chat ---- */
/* A mouse port opens but never reports a connected device. */
static ABI int32_t mouse_open(int32_t user,int32_t type,int32_t index,const void *param) { (void)user; (void)type; (void)index; (void)param; return new_id(); }
static ABI int32_t mouse_read(int32_t handle,unsigned char *data,int32_t count) {
    (void)handle;
    if (!data || count<1) return (int32_t)0x80DF0001;
    memset(data,0,40); return 1; /* timestamp 0, connected=false */
}
static ABI int32_t mouse_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }


static const RuntimeExport exports[]={
    {"sceRemoteplayInitialize",remoteplay_initialize},
    {"sceRemoteplayGetConnectionStatus",remoteplay_status},
    {"sceRemoteplayTerminate",remoteplay_terminate},
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceTerminate",user_terminate},
    {"sceUserServiceGetInitialUser",user_initial}, {"sceUserServiceGetLoginUserIdList",user_list},
    {"sceUserServiceGetUserName",user_name}, {"sceUserServiceGetEvent",user_event},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceGetStatus",system_status},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceLaunchWebBrowser",launch_browser},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_unreachable}, {"sceNetConnect",net_unreachable}, {"sceNetBind",net_unreachable},
    {"sceNetListen",net_unreachable}, {"sceNetAccept",net_unreachable}, {"sceNetSend",net_unreachable},
    {"sceNetSendto",net_unreachable}, {"sceNetRecv",net_unreachable}, {"sceNetRecvfrom",net_unreachable},
    {"sceNetSetsockopt",net_unreachable}, {"sceNetGetsockopt",net_unreachable},
    {"sceNetGetsockname",net_unreachable}, {"sceNetShutdown",net_unreachable},
    {"sceNetSocketClose",net_unreachable}, {"sceNetSocketAbort",net_unreachable},
    {"sceNetEpollControl",net_unreachable}, {"sceNetEpollWait",net_unreachable}, {"sceNetEpollAbort",net_unreachable},
    {"sceNetResolverStartNtoa",net_unreachable}, {"sceNetResolverStartAton",net_unreachable},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",netctl_check},
    {"sceNetCtlUnregisterCallback",netctl_unregister}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceSslInit",lib_init_id}, {"sceSslTerm",ok_void},
    {"sceHttpInit",lib_init_id}, {"sceHttpTerm",ok_void},
    {"sceHttpCreateTemplate",http_object}, {"sceHttpDeleteTemplate",ok_void},
    {"sceHttpCreateConnectionWithURL",http_object}, {"sceHttpCreateRequestWithURL",http_object},
    {"sceHttpSendRequest",http_fail}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",ok_void}, {"sceHttpSetConnectTimeOut",ok_void},
    {"sceHttpsEnableOption",ok_void}, {"sceHttpsDisableOption",ok_void},
    {"sceHttpAddRequestHeader",ok_void}, {"sceHttpSetRequestContentLength",ok_void},
    {"sceHttpDeleteConnection",ok_void}, {"sceHttpDeleteRequest",ok_void},
    {"sceHttpAbortWaitRequest",ok_void}, {"sceHttpDestroyEpoll",ok_void},
    {"sceHttpSetEpoll",ok_void}, {"sceHttpUnsetEpoll",ok_void}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_signed_out}, {"sceNpGetNpId",np_signed_out},
    {"sceNpRegisterStateCallback",np_register}, {"sceNpUnregisterStateCallback",ok_void},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",ok_void}, {"sceNpCheckCallback",ok_void},
    {"sceNpSetNpTitleId",ok_void}, {"sceNpNotifyPlusFeature",ok_void}, {"sceNpSetContentRestriction",ok_void},
    {"sceNpCreateAsyncRequest",np_request}, {"sceNpDeleteRequest",ok_void}, {"sceNpAbortRequest",ok_void},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",np_signed_out},
    {"sceNpGetParentalControlInfo",np_signed_out}, {"sceNpCheckPlus",np_signed_out},
    {"sceNpGetGamePresenceStatus",np_signed_out},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateAsyncRequest",np_request}, {"sceNpAuthDeleteRequest",ok_void},
    {"sceNpAuthPollAsync",np_poll}, {"sceNpAuthGetAuthorizationCode",np_signed_out},
    {"sceNpLookupCreateTitleCtx",np_request}, {"sceNpLookupDeleteTitleCtx",ok_void},
    {"sceNpLookupCreateAsyncRequest",np_request_ctx}, {"sceNpLookupDeleteRequest",ok_void},
    {"sceNpLookupAbortRequest",ok_void}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void}, {"sceNpScoreAbortRequest",ok_void},
    {"sceNpWebApiInitialize",lib_init_id}, {"sceNpWebApiTerminate",ok_void},
    {"sceNpWebApiCreateContext",np_signed_out},
    {"sceNpWebApiCreateRequest",np_signed_out}, {"sceNpWebApiSendRequest",np_signed_out},
    {"sceNpWebApiDeleteRequest",ok_void}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",ok_void}, {"sceNpWebApiReadData",np_signed_out},
    {"sceNpWebApiGetHttpStatusCode",np_signed_out}, {"sceNpWebApiGetHttpResponseHeaderValue",np_signed_out},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",np_signed_out},
    {"sceNpWebApiCreatePushEventFilter",np_signed_out}, {"sceNpWebApiDeletePushEventFilter",ok_void},
    {"sceNpWebApiRegisterPushEventCallback",np_signed_out}, {"sceNpWebApiUnregisterPushEventCallback",ok_void},
    {"sceNpWebApiUtilityParseNpId",np_signed_out},
    {"sceNpMatching2ContextStart",np_signed_out}, {"sceNpMatching2ContextStop",ok_void},
    {"sceNpMatching2DestroyContext",ok_void},
    {"sceNpMatching2RegisterContextCallback",ok_void}, {"sceNpMatching2RegisterLobbyEventCallback",ok_void},
    {"sceNpMatching2RegisterRoomEventCallback",ok_void}, {"sceNpMatching2RegisterSignalingCallback",ok_void},
    {"sceNpMatching2SetDefaultRequestOptParam",ok_void},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2JoinRoom",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out}, {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out}, {"sceNpMatching2SignalingGetPingInfo",np_signed_out},
    {"sceNpSignalingDeleteContext",ok_void}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",ok_void}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",ok_void},
    {"sceVoiceCreatePort",voice_port}, {"sceVoiceDeletePort",ok_void},
    {"sceVoiceConnectIPortToOPort",ok_void}, {"sceVoiceDisconnectIPortFromOPort",ok_void},
    {"sceVoiceStart",ok_void}, {"sceVoiceStop",ok_void}, {"sceVoiceGetPortInfo",voice_info},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",np_signed_out},
    {"sceNpSignalingInitialize",ok_void}, {"sceNpSignalingTerminate",ok_void},
    {"sceNpSignalingCreateContext",np_signed_out},
    {"sceCommonDialogInitialize",common_init},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceSaveDataDialogGetStatus",save_status}, {"sceSaveDataDialogGetResult",save_result},
    {"sceSaveDataDialogIsReadyToDisplay",save_ready}, {"sceSaveDataDialogClose",save_close},
    {"sceSaveDataDialogProgressBarSetValue",save_progress},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_open},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"scePlayGoInitialize",playgo_init}, {"scePlayGoOpen",playgo_open}, {"scePlayGoGetChunkId",playgo_chunk_ids},
    {"scePlayGoGetLocus",playgo_locus}, {"scePlayGoSetInstallSpeed",playgo_speed},
    {"sceMouseInit",ok_void}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
    {"sceAudioInOpen",audio_in_open},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceVoiceInit",ok_void}, {"sceVoiceEnd",ok_void},
};
/* Co-op (BB_ONLINE=1): network and PSN calls go to the shadNet libraries in bbgpu when they
 * implement them. Trophies, voice and the store/profile dialogs stay here. */
static int online_symbol(const char *symbol) {
    static const char *const online[]={"sceNet","sceNp","sceHttp","sceSsl"};
    static const char *const local[]={"sceNpTrophy","sceNpCommerce","sceNpProfileDialog"};
    int match=0;
    for (size_t i=0;i<sizeof(online)/sizeof(*online);++i)
        if (!strncmp(symbol,online[i],strlen(online[i]))) match=1;
    for (size_t i=0;i<sizeof(local)/sizeof(*local);++i)
        if (!strncmp(symbol,local[i],strlen(local[i]))) match=0;
    return match;
}
uintptr_t runtime_services_resolve(const char *name) {
    static int online=-1;
    if (online<0) { const char *v=getenv("BB_ONLINE"); online=v && v[0]=='1'; }
    if (online) {
        const char *symbol=runtime_symbol(name);
        if (symbol && online_symbol(symbol)) {
            uintptr_t address=bbgpu_resolve(name);
            if (address) return address;
            printf("Co-op: %s has no online version; using the offline one\n",symbol);
        }
    }
    return RUNTIME_LOOKUP(exports,name);
}
