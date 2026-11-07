/* What zrnmetrocore.c uses of xdg-shell, as no-ops: tests/swipe-cost.c runs the
 * core with no compositor at all. */
#ifndef FAKE_XDG_SHELL_H
#define FAKE_XDG_SHELL_H
#include <stdint.h>
#include <wayland-client.h>
struct xdg_wm_base; struct xdg_surface; struct xdg_toplevel;
enum { XDG_TOPLEVEL_STATE_ACTIVATED = 4 };
static const struct wl_interface xdg_wm_base_interface = { .name = "xdg_wm_base" };
struct xdg_wm_base_listener { void (*ping)(void *, struct xdg_wm_base *, uint32_t); };
struct xdg_surface_listener { void (*configure)(void *, struct xdg_surface *, uint32_t); };
struct xdg_toplevel_listener {
	void (*configure)(void *, struct xdg_toplevel *, int32_t, int32_t, struct wl_array *);
	void (*close)(void *, struct xdg_toplevel *);
};
static char fake_proxy_xdg[8];
#define xdg_wm_base_add_listener(a, b, c) ((void)(b), 0)
#define xdg_surface_add_listener(a, b, c) ((void)(b), 0)
#define xdg_toplevel_add_listener(a, b, c) ((void)(b), 0)
#define xdg_wm_base_pong(a, b) ((void)0)
#define xdg_wm_base_get_xdg_surface(a, b) ((struct xdg_surface *)fake_proxy_xdg)
#define xdg_surface_get_toplevel(a) ((struct xdg_toplevel *)fake_proxy_xdg)
#define xdg_surface_ack_configure(a, b) ((void)0)
#define xdg_toplevel_set_title(a, b) ((void)0)
#define xdg_toplevel_set_app_id(a, b) ((void)0)
#endif
