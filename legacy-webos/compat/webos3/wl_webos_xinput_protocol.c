// Generated from protocol/webos-extension.xml of webOS OSE's
// webos-wayland-extensions (https://github.com/webosose/webos-wayland-extensions,
// Apache-2.0; the protocol's copyright notice is kept in compat/webos3/NOTICE)
// with wayland-scanner private-code. Do not edit.
//
// webOS 3's libwayland-webos-client lacks these interfaces, but LG's embedder
// references them as data, so without them the app does not load. Exported
// (the scanner's private-code marks them hidden).
#include <stdlib.h>
#include <stdint.h>
#include <wayland-util.h>


extern const struct wl_interface wl_webos_xinput_interface;

static const struct wl_interface *types[] = {
	NULL,
	NULL,
	NULL,
	&wl_webos_xinput_interface,
};

static const struct wl_message wl_webos_xinput_requests[] = {
	{ "invoke_action", "uuu", types + 0 },
	{ "activated", "s", types + 0 },
	{ "deactivated", "", types + 0 },
};

static const struct wl_message wl_webos_xinput_events[] = {
	{ "activation_request", "s", types + 0 },
	{ "deactivated", "", types + 0 },
};

__attribute__((visibility("default"))) const struct wl_interface wl_webos_xinput_interface = {
	"wl_webos_xinput", 1,
	3, wl_webos_xinput_requests,
	2, wl_webos_xinput_events,
};

static const struct wl_message wl_webos_xinput_extension_requests[] = {
	{ "register_input", "n", types + 3 },
};

__attribute__((visibility("default"))) const struct wl_interface wl_webos_xinput_extension_interface = {
	"wl_webos_xinput_extension", 1,
	1, wl_webos_xinput_extension_requests,
	0, NULL,
};

