# Flutter webOS Legacy

**Run Flutter apps on older LG webOS TVs.** This is the Webos-Flutter-Legacy-TV
edition of `flutter-webos`: LG's CLI, which targets webOS 26 only, plus
[`legacy-webos/`](legacy-webos/), which repackages the same build so it runs on
older TV releases. Verified on webOS 6.5.3.

**Website:** https://webos-flutter-legacy-tv.github.io

```sh
cd legacy-webos && WEBOS_NDK_ACCEPT_EULA=1 ./setup.sh && . ./env.sh
cd my_app && flutter-webos build webos --release --legacy
#   build/webos/arm/release/ipk/*.ipk                   webOS 26
#   build/webos-compat/release/<id>_<version>_arm.ipk   older webOS
```

| Start here | |
| --- | --- |
| [Get started](https://webos-flutter-legacy-tv.github.io/get-started.html) | Build an existing app for older TVs |
| [Development setup](https://webos-flutter-legacy-tv.github.io/development.html) | Work on the project, step by step |
| [Compatibility](https://webos-flutter-legacy-tv.github.io/compatibility.html) | Status per webOS release, what works, performance |
| [legacy-webos/README.md](legacy-webos/README.md) | Technical details: shims, packager, tools, known issues |
| [Porting guide](legacy-webos/docs/PORTING-GUIDE.md) | Moving an app over: configuration, plugins, testing |

Plugins with fixes for older webOS are in
[Webos-Flutter-Legacy-TV/plugins](https://github.com/Webos-Flutter-Legacy-TV/plugins).
Bug reports, TV compatibility reports and pull requests are welcome: see
[CONTRIBUTING](https://github.com/Webos-Flutter-Legacy-TV/.github/blob/main/CONTRIBUTING.md).

### Relationship to LG's flutter-webos

This repository follows [lg-flutter-webos/flutter-webos](https://github.com/lg-flutter-webos/flutter-webos)
and regularly merges its changes. Everything outside `legacy-webos/` is LG's
code, kept as close to upstream as possible; problems that also happen with
LG's unmodified SDK on webOS 26 belong upstream. This is a community project,
not affiliated with or endorsed by LG Electronics.

---

# Upstream documentation (LG's flutter-webos)

# Flutter for webOS

Flutter-webOS is an extension to the Flutter SDK for developing Flutter applications for webOS. 

It provides the necessary tools and libraries to build, package, and run Flutter apps on webOS-powered devices.


## Quick Start 

```
flutter-webos doctor -v
flutter-webos precache -f
flutter-webos devices 

flutter-webos create --platforms webos helloworld
helloworld$ flutter-webos build webos --release
helloworld$ flutter-webos run -d <your_device_id>
```

For detailed installation steps, please refer to the instructions below.

## Required Installations

[Getting Started with Flutter for webOS](./doc/getting-started.md)
- Flutter-webOS CLI
- Flutter-webOS SDK
- webOS NDK

## License
This project is licensed under the [LICENSE](./LICENSE) file.

