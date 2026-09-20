#!/bin/bash
# Build euclidier for the Force (armhf) -> bin/euclidier. Usage: build/build.sh [outname]
# First run builds the docker image (slow under QEMU); a foreground run may exceed tool timeouts,
# so run it in the background and poll.
set -e
cd "$(dirname "$0")/.."
OUT=${1:-euclidier}
docker image inspect euclidier-build >/dev/null 2>&1 || docker build --platform linux/arm/v7 -t euclidier-build build
mkdir -p bin
docker run --rm --platform linux/arm/v7 -v "$PWD":/src -w /src euclidier-build bash -c \
  "g++ -w -D__LINUX_ALSA__ -O3 -fPIC -Wno-unused-variable *.cpp -o bin/$OUT -lncurses -lm -ldl -lstdc++ -lasound -lpthread && strip bin/$OUT && ls -l bin/$OUT"
