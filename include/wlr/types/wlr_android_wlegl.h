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
