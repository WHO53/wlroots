#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <assert.h>
#include <libinput.h>
#include <stdlib.h>
#include <stdio.h>
#include <wlr/backend/interface.h>
#include <wlr/backend/session.h>
#include <wlr/util/log.h>
#include "backend/libinput.h"
#include "util/env.h"

#if WLR_HAS_DROIDIAN_EXTENSIONS
#include <unistd.h>
#endif // WLR_HAS_DROIDIAN_EXTENSIONS

static struct wlr_libinput_backend *get_libinput_backend_from_backend(
		struct wlr_backend *wlr_backend) {
	assert(wlr_backend_is_libinput(wlr_backend));
	struct wlr_libinput_backend *backend = wl_container_of(wlr_backend, backend, backend);
	return backend;
}

static int libinput_open_restricted(const char *path,
		int flags, void *_backend) {
#if WLR_HAS_DROIDIAN_EXTENSIONS
	// Droidian: avoid going through wlr_session to avoid take/pause/release
	// loops with ever-changing file descriptors on sleep "loops".
	// This doesn't assume a multi-seat environment, but we don't care
	// about that for now.
	//
	// This is equivalent to the 'noop' wlroots session backend.
	return open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
#else
	struct wlr_libinput_backend *backend = _backend;
	struct wlr_device *dev = wlr_session_open_file(backend->session, path);
	if (dev == NULL) {
		return -1;
	}
	return dev->fd;
#endif // WLR_HAS_DROIDIAN_EXTENSIONS
}

static void libinput_close_restricted(int fd, void *_backend) {
#if WLR_HAS_DROIDIAN_EXTENSIONS
	// Droidian: as above
	close(fd);
#else
	struct wlr_libinput_backend *backend = _backend;

	struct wlr_device *dev;
	bool found = false;
	wl_list_for_each(dev, &backend->session->devices, link) {
		if (dev->fd == fd) {
			found = true;
			break;
		}
	}
	if (found) {
		wlr_session_close_file(backend->session, dev);
	}
#endif // WLR_HAS_DROIDIAN_EXTENSIONS
}

static const struct libinput_interface libinput_impl = {
	.open_restricted = libinput_open_restricted,
	.close_restricted = libinput_close_restricted
};

static int handle_libinput_readable(int fd, uint32_t mask, void *_backend) {
	struct wlr_libinput_backend *backend = _backend;
	int ret = libinput_dispatch(backend->libinput_context);
	if (ret != 0) {
		wlr_log(WLR_ERROR, "Failed to dispatch libinput: %s", strerror(-ret));
		wl_display_terminate(backend->display);
		return 0;
	}
	struct libinput_event *event;
	while ((event = libinput_get_event(backend->libinput_context))) {
		handle_libinput_event(backend, event);
		libinput_event_destroy(event);
	}
	return 0;
}

static enum wlr_log_importance libinput_log_priority_to_wlr(
		enum libinput_log_priority priority) {
	switch (priority) {
	case LIBINPUT_LOG_PRIORITY_ERROR:
		return WLR_ERROR;
	case LIBINPUT_LOG_PRIORITY_INFO:
		return WLR_INFO;
	default:
		return WLR_DEBUG;
	}
}

static void log_libinput(struct libinput *libinput_context,
		enum libinput_log_priority priority, const char *fmt, va_list args) {
	enum wlr_log_importance importance = libinput_log_priority_to_wlr(priority);
	static char wlr_fmt[1024];
	snprintf(wlr_fmt, sizeof(wlr_fmt), "[libinput] %s", fmt);
	_wlr_vlog(importance, wlr_fmt, args);
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_libinput_backend *backend =
		get_libinput_backend_from_backend(wlr_backend);
	wlr_log(WLR_DEBUG, "Starting libinput backend");

	backend->libinput_context = libinput_udev_create_context(&libinput_impl,
		backend, backend->session->udev);
	if (!backend->libinput_context) {
		wlr_log(WLR_ERROR, "Failed to create libinput context");
		return false;
	}

	if (libinput_udev_assign_seat(backend->libinput_context,
			backend->session->seat) != 0) {
		wlr_log(WLR_ERROR, "Failed to assign libinput seat");
		return false;
	}

	// TODO: More sophisticated logging
	libinput_log_set_handler(backend->libinput_context, log_libinput);
	libinput_log_set_priority(backend->libinput_context, LIBINPUT_LOG_PRIORITY_ERROR);

	int libinput_fd = libinput_get_fd(backend->libinput_context);

	handle_libinput_readable(libinput_fd, WL_EVENT_READABLE, backend);
	if (!env_parse_bool("WLR_LIBINPUT_NO_DEVICES") && wl_list_empty(&backend->devices)) {
		wlr_log(WLR_ERROR, "libinput initialization failed, no input devices");
		wlr_log(WLR_ERROR, "Set WLR_LIBINPUT_NO_DEVICES=1 to suppress this check");
		return false;
	}

	struct wl_event_loop *event_loop =
		wl_display_get_event_loop(backend->display);
	if (backend->input_event) {
		wl_event_source_remove(backend->input_event);
	}
	backend->input_event = wl_event_loop_add_fd(event_loop, libinput_fd,
			WL_EVENT_READABLE, handle_libinput_readable, backend);
	if (!backend->input_event) {
		wlr_log(WLR_ERROR, "Failed to create input event on event loop");
		return false;
	}
	wlr_log(WLR_DEBUG, "libinput successfully initialized");
	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	if (!wlr_backend) {
		return;
	}
	struct wlr_libinput_backend *backend =
		get_libinput_backend_from_backend(wlr_backend);

	struct wlr_libinput_input_device *dev, *tmp;
	wl_list_for_each_safe(dev, tmp, &backend->devices, link) {
		destroy_libinput_input_device(dev);
	}

	wlr_backend_finish(wlr_backend);

	wl_list_remove(&backend->display_destroy.link);
	wl_list_remove(&backend->session_destroy.link);
	wl_list_remove(&backend->session_signal.link);

	if (backend->input_event) {
		wl_event_source_remove(backend->input_event);
	}
	libinput_unref(backend->libinput_context);
	free(backend);
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
};

bool wlr_backend_is_libinput(struct wlr_backend *b) {
	return b->impl == &backend_impl;
}

static void session_signal(struct wl_listener *listener, void *data) {
	struct wlr_libinput_backend *backend =
		wl_container_of(listener, backend, session_signal);
	struct wlr_session *session = backend->session;

	if (!backend->libinput_context) {
		return;
	}

	if (session->active) {
		libinput_resume(backend->libinput_context);

		// HACK: Forcibly process events if there are any queued
		// On some devices it has been observed event processing
		// getting stuck on resume.
		enum libinput_event_type next_event =
			libinput_next_event_type(backend->libinput_context);
		if (next_event != LIBINPUT_EVENT_NONE) {
			handle_libinput_readable(libinput_get_fd(backend->libinput_context),
				WL_EVENT_READABLE, backend);
		}
	} else {
		libinput_suspend(backend->libinput_context);
	}
}

static void handle_session_destroy(struct wl_listener *listener, void *data) {
	struct wlr_libinput_backend *backend =
		wl_container_of(listener, backend, session_destroy);
	backend_destroy(&backend->backend);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_libinput_backend *backend =
		wl_container_of(listener, backend, display_destroy);
	backend_destroy(&backend->backend);
}

struct wlr_backend *wlr_libinput_backend_create(struct wl_display *display,
		struct wlr_session *session) {
	struct wlr_libinput_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		wlr_log(WLR_ERROR, "Allocation failed: %s", strerror(errno));
		return NULL;
	}
	wlr_backend_init(&backend->backend, &backend_impl);

	wl_list_init(&backend->devices);

	backend->session = session;
	backend->display = display;

	backend->session_signal.notify = session_signal;
	wl_signal_add(&session->events.active, &backend->session_signal);

	backend->session_destroy.notify = handle_session_destroy;
	wl_signal_add(&session->events.destroy, &backend->session_destroy);

	backend->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &backend->display_destroy);

	return &backend->backend;
}

struct libinput_device *wlr_libinput_get_device_handle(
		struct wlr_input_device *wlr_dev) {
	struct wlr_libinput_input_device *dev = NULL;
	switch (wlr_dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		dev = device_from_keyboard(wlr_keyboard_from_input_device(wlr_dev));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		dev = device_from_pointer(wlr_pointer_from_input_device(wlr_dev));
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		dev = device_from_switch(wlr_switch_from_input_device(wlr_dev));
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		dev = device_from_touch(wlr_touch_from_input_device(wlr_dev));
		break;
	case WLR_INPUT_DEVICE_TABLET_TOOL:
		dev = device_from_tablet(wlr_tablet_from_input_device(wlr_dev));
		break;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		dev = device_from_tablet_pad(wlr_tablet_pad_from_input_device(wlr_dev));
		break;
	}
	return dev->handle;
}

uint32_t usec_to_msec(uint64_t usec) {
	return (uint32_t)(usec / 1000);
}
