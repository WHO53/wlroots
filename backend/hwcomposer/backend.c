#include "util/signal.h"
#include <stdlib.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>
#include "backend/hwcomposer.h"

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	wlr_log(WLR_INFO, "Starting hwcomposer backend");

	struct wlr_hwcomposer_output *output;
	wl_list_for_each(output, &backend->outputs, link) {
		wl_event_source_timer_update(output->vsync_timer, output->frame_delay);
		wlr_output_update_enabled(&output->wlr_output, true);
		wlr_signal_emit_safe(&backend->backend.events.new_output,
			&output->wlr_output);
	}

	backend->started = true;
	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	if (!wlr_backend) {
		return;
	}

	wl_list_remove(&backend->display_destroy.link);

	struct wlr_hwcomposer_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &backend->outputs, link) {
		wlr_output_destroy(&output->wlr_output);
	}

	wlr_signal_emit_safe(&wlr_backend->events.destroy, backend);

	wlr_renderer_destroy(backend->renderer);
	wlr_egl_finish(&backend->egl);
	free(backend);
}

static struct wlr_renderer *backend_get_renderer(
		struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	return backend->renderer;
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_renderer = backend_get_renderer,
};

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_hwcomposer_backend *backend =
		wl_container_of(listener, backend, display_destroy);
	backend_destroy(&backend->backend);
}

struct wlr_backend *wlr_hwcomposer_backend_create(struct wl_display *display,
		wlr_renderer_create_func_t create_renderer_func) {
	wlr_log(WLR_INFO, "Creating hwcomposer backend");

	struct wlr_hwcomposer_backend *backend =
		calloc(1, sizeof(struct wlr_hwcomposer_backend));
	if (!backend) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_hwcomposer_backend");
		return NULL;
	}
	wlr_backend_init(&backend->backend, &backend_impl);
	backend->display = display;
	wl_list_init(&backend->outputs);

	hwcomposer_api_init(backend);

	static EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};

	if (!create_renderer_func) {
		create_renderer_func = wlr_renderer_autocreate;
	}

	backend->renderer = create_renderer_func(&backend->egl, EGL_PLATFORM_ANDROID_KHR,
		NULL, config_attribs, HAL_PIXEL_FORMAT_RGBA_8888);

	if (!backend->renderer) {
		wlr_log(WLR_ERROR, "Failed to create renderer");
		free(backend);
		return NULL;
	}

	backend->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &backend->display_destroy);

	return &backend->backend;
}

bool wlr_backend_is_hwcomposer(struct wlr_backend *backend) {
	return backend->impl == &backend_impl;
}
