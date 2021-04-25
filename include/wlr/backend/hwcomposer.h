#ifndef WLR_BACKEND_HWCOMPOSER_H
#define WLR_BACKEND_HWCOMPOSER_H

#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>

/**
 * Creates a hwcomposer backend. A hwcomposer backend has no outputs or inputs by
 * default.
 */
struct wlr_backend *wlr_hwcomposer_backend_create(struct wl_display *display,
	struct wlr_session *session, wlr_renderer_create_func_t create_renderer_func);
/**
 * Create a new hwcomposer output backed by an in-memory EGL framebuffer. You can
 * read pixels from this framebuffer via wlr_renderer_read_pixels but it is
 * otherwise not displayed.
 */
struct wlr_output *wlr_hwcomposer_add_output(struct wlr_backend *backend);

bool wlr_backend_is_hwcomposer(struct wlr_backend *backend);
bool wlr_output_is_hwcomposer(struct wlr_output *output);

#endif
