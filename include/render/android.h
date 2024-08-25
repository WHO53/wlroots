#ifndef RENDER_ANDROID_H
#define RENDER_ANDROID_H

#include <wlr/render/android.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/addon.h>

struct wlr_output;

struct wlr_android_renderer {
	struct wlr_renderer *wlr_gles_renderer;
	struct wlr_renderer wlr_renderer;

	struct wlr_egl *egl;
	EGLNativeWindowType window;

	struct wl_list buffers; // wlr_gles2_buffer.link
	struct wlr_drm_format_set shm_formats;
};

struct wlr_android_buffer {
	struct wlr_buffer *buffer;
	struct wlr_android_renderer *renderer;
	struct wl_list link; // wlr_gles2_renderer.buffers

	EGLSurface egl_surface;
	struct wlr_output *output;
	bool is_damaged;
	int buffer_age;

	struct wlr_addon addon;
};

struct wlr_android_renderer *android_get_renderer(
	struct wlr_renderer *wlr_renderer);

#endif
