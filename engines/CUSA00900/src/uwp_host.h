/* SPDX-License-Identifier: GPL-2.0-or-later
 * Private C ABI shared by the UWP host and its native runtime. */
#ifndef BB_UWP_HOST_H
#define BB_UWP_HOST_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y, l2, r2, connected, reserved;
} BbUwpPad;
typedef struct {
    uint32_t size, version;
    void *context;
    void (*extent)(void *, uint32_t *, uint32_t *);
    int32_t (*attach_swapchain)(void *, void *);
    void (*read_input)(void *, BbUwpPad *);
    int (*closed)(void *);
    int (*text_begin)(void *, const char *, const char *, uint32_t);
    int (*text_poll)(void *, char *, uint64_t);
    void (*text_cancel)(void *);
} BbUwpHost;
int bbgpu_uwp_set_host(const BbUwpHost *host);
int bbgpu_uwp_active(void);
void bbgpu_uwp_extent(uint32_t *width, uint32_t *height);
int bbgpu_uwp_closed(void);
int bbgpu_uwp_input(BbUwpPad *pad);
int bbgpu_uwp_text_begin(const char *, const char *, uint32_t);
int bbgpu_uwp_text_poll(char *, uint64_t);
void bbgpu_uwp_text_cancel(void);
#ifdef __cplusplus
}
#endif
#endif
