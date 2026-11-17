# Source me:  . legacy-webos/env.sh
# Puts flutter-webos, ares, patchelf on PATH and points flutter-webos at the
# LG NDK installed by setup.sh. Set TV_HOST (and TV_SSH_KEY if not
# ~/.ssh/lg_webos) for the tools that talk to a TV.
FW="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$FW/../bin:$FW/toolchains/ares/node_modules/.bin:$FW/toolchains/pyenv/bin:$PATH"
export WEBOS_FLUTTER_NDK_ENV="$FW/toolchains/lg-ndk-11.2.0/environment-setup-ca9v1-webosmllib32-linux-gnueabi"
