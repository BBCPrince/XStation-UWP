/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TLOU_RUNTIME_TEXT_INPUT_H
#define TLOU_RUNTIME_TEXT_INPUT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t size, version;
    void *context;
    int (*begin)(void *context, const char *initial, const char *prompt);
    int (*poll)(void *context, char *text, uint64_t capacity);
} TlouTextInputHost;
/* Register once, before game startup. Poll: 0 pending, 1 confirmed, 2 cancelled. */
#ifndef TLOU_TEXT_INPUT_EXPORT
#define TLOU_TEXT_INPUT_EXPORT
#endif
TLOU_TEXT_INPUT_EXPORT int tlou_uwp_set_text_input_host(const TlouTextInputHost *host);
int runtime_text_input_begin(const char *initial, const char *prompt);
int runtime_text_input_poll(char *text, uint64_t capacity);
#ifdef __cplusplus
}
#endif
#endif
