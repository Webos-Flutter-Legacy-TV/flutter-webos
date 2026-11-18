// Generated from webos-foreign.xml of webOS OSE's webos-wayland-extensions
// (https://github.com/webosose/webos-wayland-extensions, Apache-2.0; the
// protocol's copyright notice is kept in compat/webos4/NOTICE). Do not edit.
//
// webOS 4's libwayland-webos-client lacks these interfaces, but LG's media
// library references them as data, so without them the app does not load.
// The compositor there does not offer wl_webos_foreign, so they are never
// used at run time (no video placement on webOS 4).
#include <stddef.h>
#include <wayland-util.h>

extern const struct wl_interface wl_region_interface;
extern const struct wl_interface wl_surface_interface;
extern const struct wl_interface wl_webos_foreign_interface;
extern const struct wl_interface wl_webos_exported_interface;
extern const struct wl_interface wl_webos_imported_interface;

static const struct wl_interface *types[] = {
	&wl_webos_exported_interface,
	&wl_surface_interface,
	NULL,
	&wl_webos_imported_interface,
	NULL,
	NULL,
	&wl_region_interface,
	&wl_region_interface,
	&wl_region_interface,
	&wl_region_interface,
	&wl_region_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&wl_surface_interface,
	&wl_surface_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&wl_surface_interface,
	&wl_surface_interface,
};

static const struct wl_message wl_webos_foreign_requests[] = {
	{ "destroy", "", types + 0 },
	{ "export_element", "nou", types + 0 },
	{ "import_element", "nsu", types + 3 },
};

static const struct wl_message wl_webos_exported_requests[] = {
	{ "destroy", "", types + 6 },
	{ "set_exported_window", "oo", types + 6 },
	{ "set_crop_region", "2ooo", types + 8 },
	{ "set_property", "2ss", types + 11 },
};

static const struct wl_message wl_webos_exported_events[] = {
	{ "window_id_assigned", "su", types + 13 },
};

static const struct wl_message wl_webos_imported_requests[] = {
	{ "set_surface_alignment", "u", types + 15 },
	{ "destroy", "", types + 16 },
	{ "attach_punchthrough", "", types + 16 },
	{ "set_punchthrough", "s", types + 16 },
	{ "detach_punchthrough", "", types + 17 },
	{ "attach_surface", "o", types + 17 },
	{ "detach_surface", "o", types + 18 },
	{ "attach_punchthrough_with_context", "2s", types + 19 },
	{ "set_z_index", "2i", types + 20 },
};

static const struct wl_message wl_webos_imported_events[] = {
	{ "destination_region_changed", "uu", types + 21 },
	{ "punchthrough_attached", "2s", types + 23 },
	{ "punchthrough_detached", "2s", types + 24 },
	{ "surface_attached", "2o", types + 25 },
	{ "surface_detached", "2o", types + 26 },
};

__attribute__((visibility("default"))) const struct wl_interface wl_webos_foreign_interface = {
	"wl_webos_foreign", 1,
	3, wl_webos_foreign_requests,
	0, NULL,
};

__attribute__((visibility("default"))) const struct wl_interface wl_webos_exported_interface = {
	"wl_webos_exported", 2,
	4, wl_webos_exported_requests,
	1, wl_webos_exported_events,
};

__attribute__((visibility("default"))) const struct wl_interface wl_webos_imported_interface = {
	"wl_webos_imported", 2,
	9, wl_webos_imported_requests,
	5, wl_webos_imported_events,
};

