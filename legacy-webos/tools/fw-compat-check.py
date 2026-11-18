#!/usr/bin/env python3
"""Check, per webOS firmware, whether a legacy-webos package can link.

Uses webosbrew's firmware symbol inventory (package webosbrew-toolbox-fw-symbols
from https://github.com/webosbrew/dev-toolbox-cli/releases): one directory per
firmware image with info.json, index.json (soname -> file) and one JSON per
shared library {name, needed[], symbols[]}.

For every ELF given (LG's engine/embedder, plugin libraries, ...) it lists the
system libraries each needs and the functions it imports, then checks each
firmware for:
  - missing libraries (after the soname retargets legacy-webos applies),
  - imported symbols that no loaded system library exports.
Libraries legacy-webos bundles (glibc, the C++ runtime) and symbols its shims
provide are excluded. The inventory proves a symbol exists; it says nothing
about symbol versions, kernel versions or protocol versions.

  fw-compat-check.py --db <data dir> [--webos4] [--require LO[:HI]] <elf>...

--require makes this a test (used by CI): exit 1 if any firmware with a
release in [LO, HI) has a missing library or symbol, e.g. --require 5 for the
normal package, --require 4.4:7 with --webos4.

--webos4 checks a package built with COMPAT_WEBOS4=1. Pass the bundled
libsystemd.so.0 and libopus.so.0 as ELFs to check their own dependencies too.
"""
import argparse
import json
import os
import re
import subprocess
import sys

BUNDLED = re.compile(r"^(ld-linux\.so\.3|libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|"
                     r"librt\.so\.1|libresolv\.so\.2|libutil\.so\.1|libanl\.so\.1|libstdc\+\+\.so\.6|"
                     r"libgcc_s\.so\.1|libatomic\.so\.1|libflutter_.*|libapp\.so|lib.*_plugin\.so|"
                     r"libwl-marshal-flags-shim\.so|libwebos-foreign-compat\.so|libwv-compat\.so)$")
# libraries legacy-webos may satisfy with either of several system sonames
# (libwayland-egl.so.1 is a bundled forwarder to .so.1 or .so.0)
ALTERNATIVES = {"libwayland-egl.so.1": ["libwayland-egl.so.1", "libwayland-egl.so.0"]}
# provided by legacy-webos shims
SHIMMED = {"wl_proxy_marshal_flags", "WebPMalloc", "NDL_MediaInitWithWindowId",
           "NDL_MediaUnsubscribeEvents", "NDL_MediaSetDrmOperation", "NDL_MediaMuteDisplay",
           "NDL_MediaSetPlayer"}  # single-player fallback where the firmware lacks it
# with --webos4 (packages built with COMPAT_WEBOS4=1): libsystemd and libopus are bundled and
# libcurl.so.4 is a forwarder to the firmware's libcurl.so.5
WEBOS4_BUNDLED = {"libsystemd.so.0", "libopus.so.0"}
WEBOS4_ALTERNATIVES = {"libcurl.so.4": ["libcurl.so.4", "libcurl.so.5"]}
WEBOS4_SHIMMED = {"wl_webos_foreign_interface", "wl_webos_exported_interface",
                  "wl_webos_imported_interface"}
IGNORE_SYM = re.compile(r"^(_ITM_|__gmon_start__|_Jv_|__cxa_|__gxx_personality|_Unwind_|__aeabi_|"
                        r"__atomic_)|7flutter|^Flutter|PluginRegisterWithRegistrar$")  # libatomic, LG's own libraries and the plugin libraries are bundled


def elf_info(path):
    """(needed sonames, undefined non-weak symbols) of an ELF, via readelf."""
    dyn = subprocess.run(["readelf", "-dW", path], capture_output=True, text=True).stdout
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dyn)
    syms = subprocess.run(["readelf", "--dyn-syms", "-W", path], capture_output=True, text=True).stdout
    undef = set()
    for line in syms.splitlines():
        f = line.split()
        if len(f) >= 8 and f[6] == "UND" and f[4] != "WEAK":
            name = f[7]
            if "@" in name:
                name, ver = name.split("@", 1)
                if re.match(r"@?(GLIBC|GLIBCXX|CXXABI|GCC)_", ver):
                    continue
            if name and not IGNORE_SYM.search(name):
                undef.add(name)
    return needed, undef


class Firmware:
    def __init__(self, path):
        self.path = path
        with open(os.path.join(path, "info.json")) as f:
            self.info = json.load(f)
        with open(os.path.join(path, "index.json")) as f:
            self.index = json.load(f)
        self._libs = {}

    @property
    def release(self):
        return self.info.get("release") or self.info.get("version") or os.path.basename(self.path)

    def lib(self, soname):
        if soname not in self._libs:
            rec = None
            fn = self.index.get(soname)
            if fn:
                p = os.path.join(self.path, fn if fn.endswith(".json") else fn + ".json")
                if os.path.isfile(p):
                    with open(p) as f:
                        rec = json.load(f)
            self._libs[soname] = rec
        return self._libs[soname]

    def pick(self, soname):
        """The soname this firmware provides for a needed library, or None."""
        for alt in ALTERNATIVES.get(soname, [soname]):
            if self.lib(alt) is not None:
                return alt
        return None

    def closure(self, sonames):
        """Libraries loaded for these sonames, with their transitive deps."""
        seen, stack = {}, list(sonames)
        while stack:
            s = stack.pop()
            if s in seen:
                continue
            rec = self.lib(s)
            seen[s] = rec
            if rec:
                stack.extend(n for n in rec.get("needed", []) if n not in seen)
        return seen

    def has_file(self, pattern):
        return sorted(k for k in self.index if re.search(pattern, k))


def release_key(r):
    return tuple(int(x) for x in re.findall(r"\d+", str(r))[:3]) or (0,)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help="fw-symbols data directory")
    ap.add_argument("--webos4", action="store_true",
                    help="package built with COMPAT_WEBOS4=1 (bundled libsystemd and libopus, libcurl.so.4 forwarder)")
    ap.add_argument("--require", metavar="LO[:HI]",
                    help="exit 1 unless every firmware with LO <= release < HI passes")
    ap.add_argument("elf", nargs="+")
    args = ap.parse_args()
    lo = hi = None
    if args.require:
        a, _, b = args.require.partition(":")
        lo, hi = release_key(a), release_key(b) if b else None
    if args.webos4:
        ALTERNATIVES.update(WEBOS4_ALTERNATIVES)

    elfs = {}
    for p in args.elf:
        needed, undef = elf_info(p)
        needed = [n for n in needed if not BUNDLED.match(n)
                  and not (args.webos4 and n in WEBOS4_BUNDLED)]
        if args.webos4:
            undef = {u for u in undef if not u.startswith(("sd_", "opus_"))} - WEBOS4_SHIMMED
        elfs[os.path.basename(p)] = (needed, undef - SHIMMED)

    fws = []
    for d in os.listdir(args.db):
        if os.path.isfile(os.path.join(args.db, d, "info.json")):
            fws.append(Firmware(os.path.join(args.db, d)))
    fws.sort(key=lambda f: release_key(f.release))

    failed = []
    for fw in fws:
        print(f"\n=== webOS {fw.release}  ({os.path.basename(fw.path)})")
        cdm = fw.has_file(r"libwidevine_ce_cdm_shared")
        print(f"  Widevine CE CDM: {', '.join(cdm) if cdm else 'not present'}")
        ok = True
        for name, (needed, undef) in elfs.items():
            picked = {s: fw.pick(s) for s in needed}
            missing_libs = [s for s, p in picked.items() if p is None]
            loaded = fw.closure([p for p in picked.values() if p])
            exported = set()
            for rec in loaded.values():
                if rec:  # inventory names carry version suffixes: udev_new@LIBUDEV_183
                    exported.update(sym.split("@", 1)[0] for sym in rec.get("symbols", []))
            # symbols that would come from bundled libraries are not checked
            # EGL comes from the vendor GPU driver (libEGL forwards to it), which the
            # inventory does not always list in full.
            has_egl = fw.lib("libEGL.so.1") is not None
            missing_syms = sorted(s for s in undef if s not in exported and not (has_egl and s.startswith("egl")))
            # only flag symbols that plausibly belong to a system library
            if not missing_libs and not missing_syms:
                continue
            ok = False
            print(f"  {name}:")
            if missing_libs:
                print(f"    missing libraries: {', '.join(missing_libs)}")
            if missing_syms:
                shown = ", ".join(missing_syms[:12]) + (f" ... (+{len(missing_syms) - 12})" if len(missing_syms) > 12 else "")
                print(f"    unresolved symbols ({len(missing_syms)}): {shown}")
        if ok:
            print("  all libraries and symbols present")
        elif lo is not None and release_key(fw.release) >= lo and (hi is None or release_key(fw.release) < hi):
            failed.append(f"webOS {fw.release} ({os.path.basename(fw.path)})")

    if lo is not None:
        inrange = [f for f in fws if release_key(f.release) >= lo and (hi is None or release_key(f.release) < hi)]
        if not inrange:
            print(f"\n--require {args.require}: no firmware in that range", file=sys.stderr)
            return 1
        if failed:
            print(f"\n--require {args.require}: failed on {', '.join(failed)}", file=sys.stderr)
            return 1
        print(f"\n--require {args.require}: all {len(inrange)} firmware images in range pass", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
