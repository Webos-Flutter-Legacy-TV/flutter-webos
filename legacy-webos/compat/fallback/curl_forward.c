// libcurl.so.4 stand-in for firmware that ships curl as libcurl.so.5 only
// (webOS 4: libcurl.so.5.4.0, the same curl ABI under LG's soname).
//
// Plugins such as webos_image_texture are linked against libcurl.so.4. This
// library has no code of its own: build-shims.sh gives it the SONAME
// libcurl.so.4 and a DT_NEEDED entry for libcurl.so.5, so loading it brings
// in the firmware's curl, and the plugin's curl_* references resolve there
// (LG's binaries import curl symbols unversioned).
//
// The packager places it in lib/compat/fallback/, which is searched after the
// firmware's own library folders, so it is only used where libcurl.so.4 is
// missing.
static const char curl_forward_note[] __attribute__((used)) =
    "legacy-webos libcurl.so.4 -> libcurl.so.5 forwarder";
