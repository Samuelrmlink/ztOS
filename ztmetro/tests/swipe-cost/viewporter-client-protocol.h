/* (see xdg-shell-client-protocol.h here) */
#ifndef FAKE_VIEWPORTER_H
#define FAKE_VIEWPORTER_H
#include <wayland-client.h>
struct wp_viewporter; struct wp_viewport;
static const struct wl_interface wp_viewporter_interface = { .name = "wp_viewporter" };
#define wp_viewporter_get_viewport(a, b) ((struct wp_viewport *)NULL)
#define wp_viewport_set_source(a, b, c, d, e) ((void)0)
#define wp_viewport_set_destination(a, b, c) ((void)0)
#endif
