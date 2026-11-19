#!/usr/bin/env python3
"""Check the structure of a legacy (compat) IPK built by make-compat-ipk.sh.

These are the facts an app's start on an older TV depends on, none of which a
successful build proves:
  - the app binary's ELF interpreter is the bundled loader,
  - DT_RPATH (not DT_RUNPATH, which the jailer's LD_LIBRARY_PATH overrides)
    puts lib/compat first, on the binary and on the bundled libraries,
  - the shims and bundled glibc pieces are direct NEEDED entries,
  - every bundled library the package needs is in it,
  - the bundled loader skips /etc/ld.so.preload,
  - the webOS 4 extras are there exactly when built with COMPAT_WEBOS4=1,
  - the webOS 3 extras are there exactly when built with COMPAT_WEBOS3=1.

Usage: tools/check-compat-ipk.py [--webos4 [--webos3]] <app.ipk>
Exit status 1 with a list of problems if any check fails.
"""
import argparse
import io
import json
import posixpath
import struct
import sys
import tarfile

GLIBC = ["ld-linux.so.3", "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2",
         "librt.so.1", "libresolv.so.2", "libutil.so.1", "libanl.so.1",
         "libnss_files.so.2", "libnss_dns.so.2"]
CXX = ["libstdc++.so.6", "libgcc_s.so.1", "libatomic.so.1"]
ALWAYS = GLIBC + CXX + ["libwl-marshal-flags-shim.so", "libwayland-egl.so.1"]
EXE_NEEDED = ["libwl-marshal-flags-shim.so", "libpthread.so.0", "libdl.so.2", "librt.so.1"]
EXE_RPATH = "$ORIGIN/lib/compat:$ORIGIN/lib:$ORIGIN"
LIB_RPATH = "$ORIGIN/compat:$ORIGIN"
WEBOS4_ONLY = ["libsystemd.so.0", "libopus.so.0", "libcurl.so.4", "libwebos-foreign-compat.so"]
WEBOS3_ONLY = ["libwebos3-compat.so", "libluna-service2++.so.3", "libudev.so.1", "libwebp.so.7",
               "libwebpdemux.so.2", "libsharpyuv.so.0"]
INSTALL_ROOT = "/media/developer/apps/usr/palm/applications"

DT_NEEDED, DT_STRTAB, DT_RPATH, DT_RUNPATH = 1, 5, 15, 29
PT_LOAD, PT_DYNAMIC, PT_INTERP = 1, 2, 3


class Elf:
    """Just enough of a 32-bit little-endian ELF reader for the dynamic section."""

    def __init__(self, data):
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise ValueError("not a 32-bit little-endian ELF")
        self.data = data
        phoff, = struct.unpack_from("<I", data, 28)
        phentsize, phnum = struct.unpack_from("<HH", data, 42)
        self.phdrs = [struct.unpack_from("<8I", data, phoff + i * phentsize) for i in range(phnum)]
        self.interp = None
        self.needed, self.rpath, self.runpath = [], None, None
        for p in self.phdrs:
            if p[0] == PT_INTERP:
                self.interp = data[p[1]:p[1] + p[4]].split(b"\0")[0].decode()
        dyn = next((p for p in self.phdrs if p[0] == PT_DYNAMIC), None)
        if dyn is None:
            return
        entries = []
        for off in range(dyn[1], dyn[1] + dyn[4], 8):
            tag, val = struct.unpack_from("<iI", data, off)
            if tag == 0:
                break
            entries.append((tag, val))
        strtab = self.offset(next(v for t, v in entries if t == DT_STRTAB))
        s = lambda v: data[strtab + v:data.index(b"\0", strtab + v)].decode()
        for t, v in entries:
            if t == DT_NEEDED:
                self.needed.append(s(v))
            elif t == DT_RPATH:
                self.rpath = s(v)
            elif t == DT_RUNPATH:
                self.runpath = s(v)

    def offset(self, vaddr):
        for p in self.phdrs:
            if p[0] == PT_LOAD and p[2] <= vaddr < p[2] + p[5]:
                return vaddr - p[2] + p[1]
        raise ValueError(f"address {vaddr:#x} not in a loaded segment")


def read_ipk(path):
    """Return {path inside the app dir: (bytes, mode)} from the IPK's data tarball."""
    raw = open(path, "rb").read()
    if raw[:8] != b"!<arch>\n":
        sys.exit(f"{path}: not an ar archive")
    pos, data_tar = 8, None
    while pos < len(raw):
        name = raw[pos:pos + 16].decode().strip().rstrip("/")
        size = int(raw[pos + 48:pos + 58].decode().strip())
        if name.startswith("data.tar"):
            data_tar = raw[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
    if data_tar is None:
        sys.exit(f"{path}: no data.tar in the package")
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data_tar)) as tar:
        for m in tar.getmembers():
            if m.isfile():
                files[posixpath.normpath(m.name).lstrip("./")] = (tar.extractfile(m).read(), m.mode)
            elif m.issym():
                files[posixpath.normpath(m.name).lstrip("./")] = (None, m.linkname)
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--webos4", action="store_true", help="the package was built with COMPAT_WEBOS4=1")
    ap.add_argument("--webos3", action="store_true",
                    help="the package was built with COMPAT_WEBOS3=1 (implies --webos4)")
    ap.add_argument("ipk")
    args = ap.parse_args()
    if args.webos3:
        args.webos4 = True

    files = read_ipk(args.ipk)
    info_path = next((p for p in files if p.endswith("/appinfo.json")
                      and p.count("/") == 4 and p.startswith("usr/palm/applications/")), None)
    if info_path is None:
        sys.exit(f"{args.ipk}: no usr/palm/applications/<id>/appinfo.json")
    app_dir = posixpath.dirname(info_path)
    info = json.loads(files[info_path][0])
    app = {p[len(app_dir) + 1:]: v for p, v in files.items() if p.startswith(app_dir + "/")}
    problems = []
    bad = problems.append

    compat = {p[len("lib/compat/"):] for p in app if p.startswith("lib/compat/")}
    libs = {p[len("lib/"):] for p in app if p.startswith("lib/") and p.count("/") == 1}

    def elf(rel):
        try:
            return Elf(app[rel][0])
        except (ValueError, TypeError, struct.error) as e:
            bad(f"{rel}: {e}")
            return None

    # Bundled runtime and shims.
    for f in ALWAYS:
        if f not in compat:
            bad(f"lib/compat/{f} missing")

    # Loader must skip the firmware's preload file.
    if "ld-linux.so.3" in compat:
        ld = app["lib/compat/ld-linux.so.3"][0]
        if b"/etc/ld.so.preload\0" in ld:
            bad("bundled loader still reads /etc/ld.so.preload")
        if ld.count(b"/etc/ld.so.nopreld\0") != 1:
            bad("bundled loader is missing the /etc/ld.so.nopreld patch")

    # The main executable.
    main_rel = info.get("main")
    exe = None
    if main_rel not in app or app[main_rel][0] is None:
        bad(f"main executable {main_rel!r} not in the package")
    else:
        if not app[main_rel][1] & 0o111:
            bad(f"{main_rel} is not executable (mode {app[main_rel][1]:o})")
        exe = elf(main_rel)
    if exe:
        want = f"{INSTALL_ROOT}/{info['id']}/lib/compat/ld-linux.so.3"
        if exe.interp != want:
            bad(f"{main_rel}: interpreter {exe.interp!r}, expected {want!r}")
        if exe.runpath is not None:
            bad(f"{main_rel}: has DT_RUNPATH {exe.runpath!r} (LD_LIBRARY_PATH would win); needs DT_RPATH")
        if exe.rpath != EXE_RPATH:
            bad(f"{main_rel}: DT_RPATH {exe.rpath!r}, expected {EXE_RPATH!r}")
        for n in EXE_NEEDED:
            if n not in exe.needed:
                bad(f"{main_rel}: {n} not a direct NEEDED entry")

    # Bundled plugin/engine libraries.
    lib_elves = {}
    for name in sorted(libs):
        if not name.endswith(".so") or app["lib/" + name][0] is None:
            continue
        e = elf("lib/" + name)
        if not e:
            continue
        lib_elves[name] = e
        if e.runpath is not None:
            bad(f"lib/{name}: has DT_RUNPATH {e.runpath!r}; needs DT_RPATH")
        if e.rpath != LIB_RPATH:
            bad(f"lib/{name}: DT_RPATH {e.rpath!r}, expected {LIB_RPATH!r}")

    everything = list(lib_elves.values()) + ([exe] if exe else [])
    needs = lambda lib: any(lib in e.needed for e in everything)

    # Media shim when anything uses the old media API.
    if needs("libNDL_media.so"):
        if "libSDL2-2.0.so.0" not in compat:
            bad("libNDL_media.so is used but lib/compat/libSDL2-2.0.so.0 (media shim) is missing")
        if exe and "libSDL2-2.0.so.0" not in exe.needed:
            bad(f"{main_rel}: libSDL2-2.0.so.0 (media shim) not a direct NEEDED entry")

    # Opt-in Widevine.
    if "compat-widevine-cdm" in app and "libwv-compat.so" not in compat:
        bad("compat-widevine-cdm is set but lib/compat/libwv-compat.so is missing")
    if "compat-widevine-l1" in app and "libwv-l1.so" not in compat:
        bad("compat-widevine-l1 is set but lib/compat/libwv-l1.so is missing")

    # webOS 4 extras: present exactly with --webos4 (and only where used).
    present4 = [f for f in WEBOS4_ONLY if f in compat]
    if not args.webos4:
        for f in present4:
            bad(f"lib/compat/{f} is in a package built without COMPAT_WEBOS4=1 "
                "(it would shadow the firmware's library on webOS 5+)")
    else:
        for so in ("libsystemd.so.0", "libopus.so.0"):
            if needs(so) and so not in compat:
                bad(f"COMPAT_WEBOS4: {so} is used but not bundled")
        if needs("libcurl.so.4") and "libcurl.so.4" not in compat:
            bad("COMPAT_WEBOS4: libcurl.so.4 is used but the forwarder is not bundled")
        foreign = any(b"wl_webos_foreign_interface" in app["lib/" + n][0] for n in lib_elves)
        if foreign:
            if "libwebos-foreign-compat.so" not in compat:
                bad("COMPAT_WEBOS4: wl_webos_foreign is used but libwebos-foreign-compat.so is not bundled")
            elif exe and "libwebos-foreign-compat.so" not in exe.needed:
                bad(f"{main_rel}: libwebos-foreign-compat.so not a direct NEEDED entry")

    # webOS 3 extras: present exactly with --webos3.
    present3 = [f for f in WEBOS3_ONLY if f in compat]
    if not args.webos3:
        for f in present3:
            bad(f"lib/compat/{f} is in a package built without COMPAT_WEBOS3=1 "
                "(it would replace the firmware's library on webOS 4+)")
    else:
        if "libwebos3-compat.so" not in compat:
            bad("COMPAT_WEBOS3: lib/compat/libwebos3-compat.so missing")
        elif exe and "libwebos3-compat.so" not in exe.needed:
            bad(f"{main_rel}: libwebos3-compat.so not a direct NEEDED entry")
        for so in ("libluna-service2++.so.3", "libudev.so.1", "libwebp.so.7", "libwebpdemux.so.2"):
            if needs(so) and so not in compat:
                bad(f"COMPAT_WEBOS3: {so} is used but not bundled")
        if "libwebp.so.7" in compat and "libsharpyuv.so.0" not in compat:
            bad("COMPAT_WEBOS3: libwebp.so.7 is bundled without libsharpyuv.so.0")

    # Every bundled library a bundled ELF names must be present: a name that is
    # in lib/compat for one ELF but missing for another is a packaging slip.
    for n in sorted({n for e in everything for n in e.needed}):
        bundled_name = n in ALWAYS or n in ("libSDL2-2.0.so.0", "libwebos-foreign-compat.so", "libwv-compat.so", "libwv-l1.so",
                                            "libwebos3-compat.so")
        if bundled_name and n not in compat and n not in libs:
            bad(f"{n} is needed but not bundled")

    # Symlinks in lib/compat would point at host paths.
    for f in compat:
        if app["lib/compat/" + f][0] is None:
            bad(f"lib/compat/{f} is a symlink ({app['lib/compat/' + f][1]}); bundle the file")

    name = posixpath.basename(args.ipk)
    if problems:
        print(f"check-compat-ipk: {name}: {len(problems)} problem(s)", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        sys.exit(1)
    print(f"check-compat-ipk: {name}: OK ({len(compat)} bundled libraries"
          f"{', webOS 4 extras: ' + ', '.join(present4) if present4 else ''}"
          f"{', webOS 3 extras: ' + ', '.join(present3) if present3 else ''})", file=sys.stderr)


if __name__ == "__main__":
    main()
