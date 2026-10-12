#!/usr/bin/env bash
# Cross-build OLSRd2 (OONF) for the Zynq reference board: ARMv7 hard-float, glibc 2.28.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DOCKER=${DOCKER:-docker}
IMAGE=${IMAGE:-f01ak5-armbuild}
OONF_SRC=${OONF_SRC:-$HOME/projects/OONF}
OONF_REF=${OONF_REF:-v0.15.1}
OUT="$HERE/out"

[ -d "$OONF_SRC/.git" ] || { echo "build-arm: no OONF git tree at $OONF_SRC (set OONF_SRC)" >&2; exit 1; }
mkdir -p "$OUT"

$DOCKER build -t "$IMAGE" -f "$HERE/Dockerfile.arm" "$HERE"

$DOCKER run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e OONF_REF="$OONF_REF" \
    -v "$OONF_SRC:/src:ro" -v "$HERE:/tools:ro" -v "$OUT:/out" \
    "$IMAGE" bash -euo pipefail -c '
exec > >(tee /out/build.log) 2>&1
CC=arm-linux-gnueabihf-gcc
section() { printf "\n=== %s\n" "$*"; }

section "Date (UTC)";      date -u +%Y-%m-%dT%H:%M:%SZ
section "Build host";      cat /etc/debian_version; uname -m
section "Compiler";        $CC --version | head -1
section "Target C library"; dpkg -s libc6-dev-armhf-cross | grep -E "^(Package|Version):"
section "CMake";           cmake --version | head -1

section "OONF source"
git clone -q /src /tmp/oonf
git -C /tmp/oonf checkout -q "$OONF_REF"
git -C /tmp/oonf describe --always --long --tags
git -C /tmp/oonf rev-parse HEAD

section "Configure"
CMAKE_ARGS="-DCMAKE_TOOLCHAIN_FILE=/tools/arm-linux-gnueabihf.cmake -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS=-fcommon"
echo "cmake -S /tmp/oonf -B /tmp/oonf/build $CMAKE_ARGS"
cmake -S /tmp/oonf -B /tmp/oonf/build $CMAKE_ARGS

section "Build"
cmake --build /tmp/oonf/build --target olsrd2_static -- -j"$(nproc)"
cp /tmp/oonf/build/olsrd2_static /out/olsrd2

section "file";               file /out/olsrd2
section "Needed libraries";   arm-linux-gnueabihf-readelf -d /out/olsrd2 | grep NEEDED
section "Interpreter";        arm-linux-gnueabihf-readelf -l /out/olsrd2 | grep -o "interpreter: [^]]*"
section "Highest GLIBC version needed"
arm-linux-gnueabihf-objdump -T /out/olsrd2 | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
section "Size";               ls -l /out/olsrd2
section "sha256";             sha256sum /out/olsrd2
'
echo "build-arm: done, binary and log in $OUT"
