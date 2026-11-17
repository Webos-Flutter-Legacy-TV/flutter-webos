# LG webOS NDK 11.2.0 vs webosbrew (openlgtv/buildroot-nc4 2026.08-webos)

Compared 2026-11-18. Paths are relative to `toolchains/`.

| | LG NDK 11.2.0 (`lg-ndk-11.2.0`) | webosbrew (`webosbrew/arm-webos-linux-gnueabi_sdk-buildroot`) |
| --- | --- | --- |
| Source / license | LG, LG SDK License Agreement (no redistribution, no reverse engineering) | Community, open source (buildroot) |
| Build system | Yocto/OE SDK, distro `starfish` 11.2.0 (webOS 26) | Buildroot 2026.08 |
| Target triple | `arm-webosmllib32-linux-gnueabi` | `arm-webos-linux-gnueabi` |
| Compilers | GCC 13.3.0 **and clang/lld 18.1.6** | GCC 16.2.0 only (no clang) |
| Binutils | LLD 18.1.6 / GNU ld | GNU ld 2.46.1 |
| CPU / ABI defaults | cortex-a9, NEON, softfp, `-mthumb` (set in env script) | cortex-a9, neon-fp16, softfp, ARM mode (built into gcc) |
| **glibc** | **2.39** | **2.12.2** |
| libstdc++ | 6.0.32 (`GLIBCXX_3.4.32`) | 6.0.36 (`GLIBCXX_3.4.36`) |
| Kernel headers | 6.6 | 3.10 |
| Sysroot size | 610 MB (also ships aarch64 `lib64`) | 463 MB |
| Install size | 2.1 GB | 1.2 GB |
| pkg-config modules | 86 | 145 |
| Wayland client / webOS Wayland | **not shipped** | 1.11.0 + `wayland-webos-*` protocol headers |
| luna-service2, webosi18n, xkbcommon, fontconfig | **not shipped** | shipped |
| Media | NDL (`ndl`, `ndlmedia`), NDL_directmedia | NDL_directmedia, AcbAPI, uMediaServer, GStreamer, PulseAudio |
| Other only-here | systemd, gbm, webp, opus, zstd, python 3.12 | Qt5, SDL2 (both have SDL2), libpng/jpeg, freetype, lttng, python 3.14 |
| CMake toolchain file | none (env script; flutter-webos drives cmake) | `share/buildroot/toolchainfile.cmake` |

## What it means for the port

- **LG NDK = the webOS 26 world.** Its glibc 2.39 / libstdc++ 3.4.32 are exactly what LG's
  Flutter binaries require, so it is the source of the runtime we bundle for option B.
  It deliberately omits Wayland/Luna/xkbcommon dev files: the embedder is prebuilt, so
  you cannot build a replacement embedder with it.
- **webosbrew = the "every release" world.** glibc 2.12.2 + kernel headers 3.10 means
  anything it builds runs on every webOS release (glibc is backward compatible). Its
  Wayland 1.11.0 matches webOS 6 exactly, and it ships Luna and webOS Wayland protocol
  headers, which makes it the right base for option D (own embedder) and for compat shims.
- **ABI compatible:** both are cortex-a9 / softfp / NEON, so objects from either link together.
- **Gap for option D:** the Flutter engine must be built with clang (C++20). webosbrew has
  no clang; use LG's clang 18 or upstream clang with `--sysroot`/`--gcc-toolchain` pointed
  at the webosbrew sysroot. Building a modern engine against glibc 2.12 will need patches
  (missing newer libc functions); a middle floor (e.g. glibc 2.28 for webOS 5–6) is easier.
- **Licensing:** glibc and libstdc++ are LGPL/GPL-with-exception OSS, which the LG
  agreement (s. 2.6) carves out, so bundling them from the LG sysroot should be OK;
  LG's own closed libraries must not be redistributed outside LG's terms.
