#define _POSIX_C_SOURCE 199309L
#include <assert.h>
#include <stdlib.h>
#include "render/android.h"
#include "render/egl.h"
#include "render/gles2.h"
#include "types/wlr_matrix.h"
#include "wlr/types/wlr_android_wlegl.h"
#include <drm_fourcc.h>

static const struct wlr_renderer_impl renderer_impl;

bool wlr_renderer_is_android(struct wlr_renderer *wlr_renderer) {
	return wlr_renderer->impl == &renderer_impl;
}

struct wlr_android_renderer *android_get_renderer(
		struct wlr_renderer *wlr_renderer) {
	assert(wlr_renderer_is_android(wlr_renderer));
	struct wlr_android_renderer *renderer = wl_container_of(wlr_renderer, renderer, wlr_renderer);
	return renderer;
}

static struct wlr_gles2_renderer *android_get_gles2_renderer(struct wlr_renderer *wlr_renderer) {
	assert(wlr_renderer_is_android(wlr_renderer));
	struct wlr_android_renderer *renderer = wl_container_of(wlr_renderer, renderer, wlr_renderer);
	return gles2_get_renderer(renderer->wlr_gles_renderer);
}

struct wlr_egl *wlr_android_renderer_get_egl(struct wlr_renderer *wlr_renderer) {
	struct wlr_gles2_renderer *gles2_renderer = android_get_gles2_renderer(wlr_renderer);

	return gles2_renderer->egl;
}

//
// wl_drm compat
// Reintroduced and reworked for 0.17.x from these reverted commits:
// * render/egl: remove EGL_WL_bind_wayland_display support (8a4957570f2d546cad033371db0c2463459536ce)
// * render/gles2: use wlr_drm for wl_drm implementation (4e07d4cbf9c104625d419b9123dca0ef402472e7)
//
static struct wlr_texture *gles2_texture_from_wl_drm(struct wlr_renderer *wlr_renderer,
		struct wl_resource *resource) {
	struct wlr_gles2_renderer *renderer = android_get_gles2_renderer(wlr_renderer);

	if (!renderer->procs.glEGLImageTargetTexture2DOES) {
		return NULL;
	}

	struct wlr_egl_context prev_ctx;
	wlr_egl_save_context(&prev_ctx);
	wlr_egl_make_current(renderer->egl);

	EGLint fmt;
	int width, height;

	if (!renderer->egl->procs.eglQueryWaylandBufferWL(renderer->egl->display, resource,
			EGL_TEXTURE_FORMAT, &fmt)) {
		return NULL;
	}

	renderer->egl->procs.eglQueryWaylandBufferWL(renderer->egl->display, resource, EGL_WIDTH, &width);
	renderer->egl->procs.eglQueryWaylandBufferWL(renderer->egl->display, resource, EGL_HEIGHT, &height);

	const EGLint attribs[] = {
		EGL_WAYLAND_PLANE_WL, 0,
		EGL_NONE,
	};
	EGLImageKHR image = renderer->egl->procs.eglCreateImageKHR(renderer->egl->display, renderer->egl->context,
		EGL_WAYLAND_BUFFER_WL, resource, attribs);


	if (image == EGL_NO_IMAGE_KHR) {
		wlr_log(WLR_ERROR, "Failed to create EGL image from wl_drm resource");
		goto error_ctx;
	}

	struct wlr_gles2_texture *texture =
		gles2_texture_create(renderer, width, height);
	if (texture == NULL) {
		goto error_image;
	}

	texture->drm_format = DRM_FORMAT_INVALID; // texture can't be written anyways
	texture->image = image;

	switch (fmt) {
	case EGL_TEXTURE_RGB:
		texture->has_alpha = false;
		break;
	case EGL_TEXTURE_RGBA:
	case EGL_TEXTURE_EXTERNAL_WL:
		texture->has_alpha = true;
		break;
	default:
		wlr_log(WLR_ERROR, "Invalid or unsupported EGL buffer format");
		goto error_texture;
	}

	texture->target = GL_TEXTURE_EXTERNAL_OES;

	push_gles2_debug(renderer);

	glGenTextures(1, &texture->tex);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture->tex);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	renderer->procs.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
		texture->image);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

	pop_gles2_debug(renderer);

	wlr_egl_restore_context(&prev_ctx);

	return &texture->wlr_texture;

error_texture:
	wl_list_remove(&texture->link);
	free(texture);
error_image:
	wlr_egl_destroy_image(renderer->egl, image);
error_ctx:
	wlr_egl_restore_context(&prev_ctx);
	return NULL;
}
//
// End wl_drm compat
//

static void destroy_buffer(struct wlr_android_buffer *buffer) {
	wl_list_remove(&buffer->link);
	wlr_addon_finish(&buffer->addon);

	free(buffer);
}

static void handle_buffer_destroy(struct wlr_addon *addon) {
	struct wlr_android_buffer *buffer =
		wl_container_of(addon, buffer, addon);
	destroy_buffer(buffer);
}

static const struct wlr_addon_interface buffer_addon_impl = {
	.name = "wlr_android_buffer",
	.destroy = handle_buffer_destroy,
};

static struct wlr_android_buffer *get_or_create_buffer(struct wlr_android_renderer *renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_addon *addon =
		wlr_addon_find(&wlr_buffer->addons, renderer, &buffer_addon_impl);
	if (addon) {
		struct wlr_android_buffer *buffer = wl_container_of(addon, buffer, addon);
		return buffer;
	}

	struct wlr_android_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	buffer->buffer = wlr_buffer;
	buffer->renderer = renderer;

	if (!renderer->window) {
		wlr_log(WLR_ERROR, "No native window set");
		buffer->egl_surface = EGL_NO_SURFACE;
	} else {
		buffer->egl_surface = eglCreateWindowSurface(renderer->egl->display,
			renderer->egl->config, renderer->window, NULL);
		if (buffer->egl_surface == EGL_NO_SURFACE) {
			wlr_log(WLR_ERROR, "Failed to recreate EGL surface");
			return NULL;
		}

		wlr_addon_init(&buffer->addon, &wlr_buffer->addons, renderer,
			&buffer_addon_impl);

		wl_list_insert(&renderer->buffers, &buffer->link);
	}

	wlr_log(WLR_DEBUG, "Created WindowSurface for buffer %dx%d",
			wlr_buffer->width, wlr_buffer->height);

	return buffer;
}

static void android_destroy(struct wlr_renderer *wlr_renderer) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	struct wlr_android_buffer *buffer, *buffer_tmp;
	wl_list_for_each_safe(buffer, buffer_tmp, &renderer->buffers, link) {
		destroy_buffer(buffer);
	}

	renderer->wlr_gles_renderer->impl->destroy(renderer->wlr_gles_renderer);
}

static bool android_bind_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	if (wlr_buffer == NULL) {
		return true;
	}

	struct wlr_android_buffer *buffer = get_or_create_buffer(renderer, wlr_buffer);
	if (buffer == NULL) {
		return false;
	}

	return wlr_egl_make_current_with_surface(renderer->egl, buffer->egl_surface, &buffer->buffer_age);
}

static bool android_begin(struct wlr_renderer *wlr_renderer, uint32_t width,
		uint32_t height) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);
	bool ret = renderer->wlr_gles_renderer->impl->begin(renderer->wlr_gles_renderer, width, height);

	if (ret) {
		matrix_projection(android_get_gles2_renderer(wlr_renderer)->projection, width, height,
			WL_OUTPUT_TRANSFORM_NORMAL);
	}

	return ret;
}

static void android_end(struct wlr_renderer *wlr_renderer) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	renderer->wlr_gles_renderer->impl->end(renderer->wlr_gles_renderer);
}

static void android_clear(struct wlr_renderer *wlr_renderer, const float color[static 4]) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	renderer->wlr_gles_renderer->impl->clear(renderer->wlr_gles_renderer, color);
}

static void android_scissor(struct wlr_renderer *wlr_renderer, struct wlr_box *box) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);
	struct wlr_gles2_renderer *gles2_renderer = android_get_gles2_renderer(wlr_renderer);

	if (box != NULL) {
		struct wlr_box gl_box;
		wlr_box_transform(&gl_box, box, WL_OUTPUT_TRANSFORM_FLIPPED_180,
			gles2_renderer->viewport_width, gles2_renderer->viewport_height);
		renderer->wlr_gles_renderer->impl->scissor(renderer->wlr_gles_renderer, &gl_box);
	} else {
		renderer->wlr_gles_renderer->impl->scissor(renderer->wlr_gles_renderer, NULL);
	}
}

static bool android_render_subtexture_with_matrix(struct wlr_renderer *wlr_renderer,
		struct wlr_texture *wlr_texture, const struct wlr_fbox *box,
		const float matrix[static 9], float alpha) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	return renderer->wlr_gles_renderer->impl->render_subtexture_with_matrix(renderer->wlr_gles_renderer, wlr_texture, box, matrix, alpha);
}

static void android_render_quad_with_matrix(struct wlr_renderer *wlr_renderer,
		const float color[static 4], const float matrix[static 9]) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	renderer->wlr_gles_renderer->impl->render_quad_with_matrix(renderer->wlr_gles_renderer, color, matrix);
}

static const uint32_t *android_get_shm_texture_formats(
		struct wlr_renderer *wlr_renderer, size_t *len) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	return renderer->wlr_gles_renderer->impl->get_shm_texture_formats(renderer->wlr_gles_renderer, len);
}

static const struct wlr_drm_format_set *android_get_dmabuf_texture_formats(
	struct wlr_renderer *wlr_renderer) {
	return NULL;
}

static const struct wlr_drm_format_set *android_get_render_formats(struct wlr_renderer *wlr_renderer) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	return &renderer->shm_formats;
}

static enum wl_shm_format android_preferred_read_format(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	return renderer->wlr_gles_renderer->impl->preferred_read_format(renderer->wlr_gles_renderer);
}

static bool android_read_pixels(struct wlr_renderer *wlr_renderer,
		uint32_t drm_format, uint32_t stride,
		uint32_t width, uint32_t height, uint32_t src_x, uint32_t src_y,
		uint32_t dst_x, uint32_t dst_y, void *data) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	return renderer->wlr_gles_renderer->impl->read_pixels(renderer->wlr_gles_renderer, drm_format, stride, width, height, src_x, src_y, dst_x, dst_y, data);
}

static int android_get_drm_fd(struct wlr_renderer *wlr_renderer) {
	return -1;
}

static uint32_t android_get_render_buffer_caps(struct wlr_renderer *wlr_renderer) {
	return WLR_BUFFER_CAP_SHM | WLR_BUFFER_CAP_DATA_PTR;
}

static struct wlr_texture *android_texture_from_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);
	struct wlr_texture *texture;

	if (wlr_android_wlegl_buffer_is_instance(wlr_buffer)) {
		texture = gles2_texture_from_wl_drm(wlr_renderer, ((struct wlr_android_wlegl_buffer *)wlr_buffer)->inner.resource);
	} else {
		texture = renderer->wlr_gles_renderer->impl->texture_from_buffer(renderer->wlr_gles_renderer, wlr_buffer);
	}

	return texture;
}

static struct wlr_render_timer *android_render_timer_create(struct wlr_renderer *wlr_renderer) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	return renderer->wlr_gles_renderer->impl->render_timer_create(renderer->wlr_gles_renderer);
}

static void android_set_nativewindow(struct wlr_renderer *wlr_renderer, EGLNativeWindowType window) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	renderer->window = window;
}

static bool android_swap_buffers(struct wlr_renderer *wlr_renderer, pixman_region32_t *damage,
		struct wlr_output *output) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	struct wlr_android_buffer *buffer, *buffer_tmp;
	wl_list_for_each_safe(buffer, buffer_tmp, &renderer->buffers, link) {
		if (!buffer || buffer->egl_surface == EGL_NO_SURFACE || buffer->output != output)
			continue;

		return wlr_egl_swap_buffers(renderer->egl, buffer->egl_surface, buffer->is_damaged ? damage : NULL);
	}

	return true;
}

static bool android_set_damage_region(struct wlr_renderer *wlr_renderer, pixman_region32_t *damage) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	struct wlr_android_buffer *buffer, *buffer_tmp;
	wl_list_for_each_safe(buffer, buffer_tmp, &renderer->buffers, link) {
		if (!buffer || buffer->egl_surface == EGL_NO_SURFACE)
			continue;

		buffer->is_damaged = wlr_egl_set_damage_region(renderer->egl, buffer->egl_surface, damage);
		return buffer->is_damaged;
	}

	return true;
}

static int android_get_buffer_age(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	struct wlr_addon *addon =
		wlr_addon_find(&wlr_buffer->addons, renderer, &buffer_addon_impl);
	if (addon) {
		struct wlr_android_buffer *buffer = wl_container_of(addon, buffer, addon);
		return buffer->buffer_age;
	}

	return -1;
}

static const struct wlr_renderer_impl renderer_impl = {
	.destroy = android_destroy,
	.bind_buffer = android_bind_buffer,
	.begin = android_begin,
	.end = android_end,
	.clear = android_clear,
	.scissor = android_scissor,
	.render_subtexture_with_matrix = android_render_subtexture_with_matrix,
	.render_quad_with_matrix = android_render_quad_with_matrix,
	.get_shm_texture_formats = android_get_shm_texture_formats,
	.get_dmabuf_texture_formats = android_get_dmabuf_texture_formats,
	.get_render_formats = android_get_render_formats,
	.preferred_read_format = android_preferred_read_format,
	.read_pixels = android_read_pixels,
	.get_drm_fd = android_get_drm_fd,
	.get_render_buffer_caps = android_get_render_buffer_caps,
	.texture_from_buffer = android_texture_from_buffer,
	.render_timer_create = android_render_timer_create,
	.set_nativewindow = android_set_nativewindow,
	.swap_buffers = android_swap_buffers,
	.set_damage_region = android_set_damage_region,
	.get_buffer_age = android_get_buffer_age,
};

struct wlr_renderer *wlr_android_renderer_create(void) {
	struct wlr_android_renderer *renderer = calloc(1, sizeof(*renderer));
	if (renderer == NULL) {
		return NULL;
	}

	struct wlr_egl *egl = wlr_egl_create_for_android();
	if (egl == NULL) {
		wlr_log(WLR_ERROR, "Could not initialize EGL");
		return NULL;
	}

	renderer->wlr_gles_renderer = wlr_gles2_renderer_create(egl);
	if (!renderer->wlr_gles_renderer) {
		wlr_log(WLR_ERROR, "Failed to create GLES2 renderer");
		wlr_egl_destroy(egl);
		return NULL;
	}

	wl_list_init(&renderer->buffers);
	wlr_renderer_init(&renderer->wlr_renderer, &renderer_impl);
	renderer->egl = egl;
	wlr_drm_format_set_add(&renderer->shm_formats, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR);
	wlr_drm_format_set_add(&renderer->shm_formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);

	return &renderer->wlr_renderer;
}

bool wlr_android_renderer_init_wl_display(struct wlr_renderer *wlr_renderer,
		struct wl_display *wl_display) {
	struct wlr_android_renderer *renderer = android_get_renderer(wlr_renderer);

	wlr_android_wlegl_create(wl_display, wlr_renderer);

	return true;
}
