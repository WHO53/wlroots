#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include <android/hardware/gralloc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_android_wlegl.h>
#include <wlr/util/log.h>

#define WLR_ANDROID_WLEGL_VERSION 2

int hybris_gralloc_allocate(int width, int height, int format, int usage, buffer_handle_t *handle,
	uint32_t *stride);
int hybris_gralloc_release(buffer_handle_t handle, int was_allocated);

static void buffer_handle_release(struct wl_listener *listener, void *data) {
	struct wlr_android_wlegl_buffer *buffer = wl_container_of(listener, buffer, release);

	if (buffer->inner.resource != NULL) {
		wl_buffer_send_release(buffer->inner.resource);
    }
}

static void buffer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_impl = {
	.destroy = buffer_handle_destroy,
};

static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct wlr_android_wlegl_buffer *buffer = (struct wlr_android_wlegl_buffer *)wlr_buffer;
	free(buffer);
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
};


static void android_wlegl_buffer_inner_incref(struct android_native_base_t *base) {
	struct wlr_android_wlegl_buffer_remote_buffer *buffer;

	buffer = wl_container_of(base, buffer, base);

	int result = __sync_fetch_and_add(&buffer->refcount, 1);
}

static void android_wlegl_buffer_inner_decref(struct android_native_base_t *base) {
	struct wlr_android_wlegl_buffer_remote_buffer *buffer;

	buffer = wl_container_of(base, buffer, base);

	if (__sync_fetch_and_sub(&buffer->refcount, 1) > 1) {
		return;
	}

	hybris_gralloc_release(buffer->base.handle, buffer->allocated);
	free(buffer);
}

static void server_wlegl_buffer_destroy(struct wl_resource *resource)
{
	struct wlr_android_wlegl_buffer *buffer = wl_container_of(wl_resource_get_user_data(resource), buffer, inner);
	android_wlegl_buffer_inner_decref(&buffer->inner.native_buffer->base.common);
	buffer->inner.resource = NULL;
	wlr_buffer_drop(&buffer->base);
}

static struct wlr_android_wlegl_buffer * server_wlegl_buffer_create_server(struct wl_client *client, int32_t width,
	int32_t height, int32_t stride, int32_t format, int32_t usage,
	buffer_handle_t handle,
	struct wlr_android_wlegl *android_wlegl)
{
	struct wlr_android_wlegl_buffer *buffer = calloc(1, sizeof(*buffer));

	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);

	buffer->base.accessing_data_ptr = false;

	buffer->inner.android_wlegl = android_wlegl;
	buffer->inner.resource = wl_resource_create(client, &wl_buffer_interface, 1, 0);
	wl_resource_set_implementation(buffer->inner.resource, &wl_buffer_impl, &buffer->inner, server_wlegl_buffer_destroy);

	buffer->inner.native_buffer = calloc(1, sizeof(*buffer->inner.native_buffer));

	buffer->inner.native_buffer->refcount = 1;
	buffer->inner.native_buffer->allocated = true;

	buffer->inner.native_buffer->base.width = width;
	buffer->inner.native_buffer->base.height = height;
    buffer->inner.native_buffer->base.stride = stride;
    buffer->inner.native_buffer->base.format = format;
    buffer->inner.native_buffer->base.usage = usage;

    buffer->inner.native_buffer->base.common.magic = ANDROID_NATIVE_BUFFER_MAGIC;
    buffer->inner.native_buffer->base.common.version = sizeof(ANativeWindowBuffer);
	buffer->inner.native_buffer->base.common.incRef = android_wlegl_buffer_inner_incref;
	buffer->inner.native_buffer->base.common.decRef = android_wlegl_buffer_inner_decref;
	memset(buffer->inner.native_buffer->base.common.reserved, 0, sizeof(buffer->inner.native_buffer->base.common.reserved));
	buffer->inner.native_buffer->base.handle = handle;

	buffer->release.notify = buffer_handle_release;
    wl_signal_add(&buffer->base.events.release, &buffer->release);

	return buffer;
}

static void android_wlegl_create_handle(struct wl_client *client, struct wl_resource *resource, uint32_t id, int32_t num_fds, struct wl_array *ints) {
	wlr_log(WLR_ERROR, "create handle called: this is not supported by this implementation!");
}

static void android_wlegl_create_buffer(struct wl_client *client, struct wl_resource *resource,
		    uint32_t id, int32_t width, int32_t height, int32_t stride,
		    int32_t format, int32_t usage, struct wl_resource *handle) {
	wlr_log(WLR_ERROR, "create buffer called: this is not supported by this implementation!");
}

/* Ref: libhybris/hybris/platforms/common/server_wlegl.cpp */
static void android_wlegl_get_server_buffer_handle(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, int32_t width, int32_t height,
	int32_t format, int32_t usage) {

	if (width == 0 || height == 0) {
		wl_resource_post_error(resource, 0, "invalid buffer size: %u,%u\n", width, height);
		return;
	}

	struct wlr_android_wlegl *android_wlegl = wl_resource_get_user_data(resource);

	struct wl_resource *buffer_resource = wl_resource_create(client, &android_wlegl_server_buffer_handle_interface,
		wl_resource_get_version(resource), id);

	buffer_handle_t _handle;
	int _stride;

	usage |= GRALLOC_USAGE_HW_COMPOSER;

	if (format == 0) format = HAL_PIXEL_FORMAT_RGBA_8888;

	int r = hybris_gralloc_allocate(width, height, format, usage, &_handle, (uint32_t*)&_stride);
	if (r) {
		wlr_log(WLR_ERROR, "failed to allocate buffer");
		wl_resource_destroy(buffer_resource);
		return;
	}

	struct wlr_android_wlegl_buffer *buffer = server_wlegl_buffer_create_server(client, width, height,
		_stride, format, usage, _handle, android_wlegl);

	struct wl_array ints;
	int *ints_data;
	wl_array_init(&ints);
	ints_data = (int*) wl_array_add(&ints, _handle->numInts * sizeof(int));
	memcpy(ints_data, _handle->data + _handle->numFds, _handle->numInts * sizeof(int));

	android_wlegl_server_buffer_handle_send_buffer_ints(buffer_resource, &ints);
	wl_array_release(&ints);

	for (int i = 0; i < _handle->numFds; i++) {
		android_wlegl_server_buffer_handle_send_buffer_fd(buffer_resource, _handle->data[i]);
	}

	android_wlegl_server_buffer_handle_send_buffer(buffer_resource, buffer->inner.resource, format, _stride);
	wl_resource_destroy(buffer_resource);

}

static const struct android_wlegl_interface android_wlegl_impl = {
	.create_handle = android_wlegl_create_handle,
	.create_buffer = android_wlegl_create_buffer,
	.get_server_buffer_handle = android_wlegl_get_server_buffer_handle,
};

static void android_wlegl_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_android_wlegl *android_wlegl = data;

	struct wl_resource *resource = wl_resource_create(client,
		&android_wlegl_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &android_wlegl_impl, android_wlegl, NULL);
}

bool wlr_android_wlegl_buffer_is_instance(struct wlr_buffer *wlr_buffer) {
	return wlr_buffer->impl == &buffer_impl;
}

static bool buffer_resource_is_instance(struct wl_resource *resource) {
	return wl_resource_instance_of(resource, &wl_buffer_interface,
		&wl_buffer_impl);
}

static struct wlr_android_wlegl_buffer *wlr_android_wlegl_buffer_try_from_resource(
		struct wl_resource *resource) {
	struct wlr_android_wlegl_buffer *buffer;
	if (!buffer_resource_is_instance(resource)) {
		return NULL;
	}
	buffer = wl_container_of(wl_resource_get_user_data(resource), buffer, inner);
	return buffer;
}

static struct wlr_buffer *buffer_from_resource(struct wl_resource *resource) {
	struct wlr_android_wlegl_buffer *buffer = wlr_android_wlegl_buffer_try_from_resource(resource);
	assert(buffer != NULL);
	return &buffer->base;
}


static const struct wlr_buffer_resource_interface buffer_resource_interface = {
	.name = "wlr_android_wlegl_buffer",
	.is_instance = buffer_resource_is_instance,
    .from_resource = buffer_from_resource,
};

struct wlr_android_wlegl *wlr_android_wlegl_create(struct wl_display *display,
		struct wlr_renderer *renderer) {

	struct wlr_android_wlegl *android_wlegl = calloc(1, sizeof(*android_wlegl));
	if (android_wlegl == NULL) {
		return NULL;
	}

	wl_signal_init(&android_wlegl->events.destroy);

	android_wlegl->global = wl_global_create(display, &android_wlegl_interface, WLR_ANDROID_WLEGL_VERSION,
		android_wlegl, android_wlegl_bind);
	if (android_wlegl->global == NULL) {
		goto error;
	}

	//android_wlegl->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &android_wlegl->display_destroy);

	wlr_buffer_register_resource_interface(&buffer_resource_interface);

	return android_wlegl;

error:
	free(android_wlegl);
	return NULL;
}

