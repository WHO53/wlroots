/*
 * Copyright © 2024 Eugenio Paolantonio (g7) <me@medesimo.eu>
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef WLR_TYPES_WLR_ANDROID_WLEGL_H
#define WLR_TYPES_WLR_ANDROID_WLEGL_H

#include <wayland-server-protocol.h>
#include <android/nativebase/nativebase.h>
#include <android/system/window.h>
#include <wlr/types/wlr_buffer.h>
#include "protocol/wayland-android-protocol.h"

struct wlr_renderer;

struct wlr_android_wlegl_buffer_remote_buffer {
	bool allocated;
	uint8_t refcount;
	ANativeWindowBuffer base;
};

struct  wlr_android_wlegl_buffer_inner {
	struct wl_resource *resource;
	struct wlr_android_wlegl *android_wlegl;

	struct wlr_android_wlegl_buffer_remote_buffer *native_buffer;
};

struct wlr_android_wlegl_buffer {
	struct wlr_buffer base;
	struct wlr_android_wlegl_buffer_inner inner;

	struct wl_listener release;
};

struct wlr_android_wlegl {
	struct wl_global *global;

	struct {
		struct wl_signal destroy;
	} events;

	// private state

	struct wl_listener display_destroy;
};

struct wlr_android_wlegl *wlr_android_wlegl_create(struct wl_display *display,
		struct wlr_renderer *renderer);

bool wlr_android_wlegl_buffer_is_instance(struct wlr_buffer *wlr_buffer);

#endif /* WLR_TYPES_WLR_ANDROID_WLEGL_H */
