/* SPDX-License-Identifier: GPL-2.0-or-later */
#define TLOU_TEXT_INPUT_EXPORT __declspec(dllexport)
#include "runtime_text_input.h"
#include "../gpu/bbgpu.h"
#include <pthread.h>
#include <string.h>
static pthread_mutex_t text_lock=PTHREAD_MUTEX_INITIALIZER;
static TlouTextInputHost text_host;
int tlou_uwp_set_text_input_host(const TlouTextInputHost *host) {
    if (!host || host->size!=sizeof(*host) || host->version!=1 || !host->context ||
        !host->begin || !host->poll) return 87;
    pthread_mutex_lock(&text_lock);
    int result=text_host.begin ? 170 : 0;
    if (!result) text_host=*host;
    pthread_mutex_unlock(&text_lock);
    return result;
}
static TlouTextInputHost current_host(void) {
    pthread_mutex_lock(&text_lock);
    TlouTextInputHost host=text_host;
    pthread_mutex_unlock(&text_lock);
    return host;
}
int runtime_text_input_begin(const char *initial,const char *prompt) {
    if (!initial || !prompt) return 0;
    TlouTextInputHost host=current_host();
    return host.begin ? host.begin(host.context,initial,prompt) : bbgpu_text_input_begin(initial,prompt);
}
int runtime_text_input_poll(char *text,uint64_t capacity) {
    if (!text || !capacity) return 2;
    TlouTextInputHost host=current_host();
    int result=host.poll ? host.poll(host.context,text,capacity) : bbgpu_text_input_poll(text,capacity);
    if (result<0 || result>2) result=2;
    if (result==1 && !memchr(text,0,(size_t)capacity)) { text[0]=0; result=2; }
    return result;
}
