#!/bin/bash
# Build LEDrum for the Pi Sidecar: ledrum.a64.rmp (an RMPA package, AArch64).
# Needs aarch64-linux-gnu-gcc / g++ and python3. Everything else is in this repository.
set -e
cd "$(dirname "$0")"

A64="aarch64-linux-gnu-"
FLAGS="-mcpu=cortex-a72 -O2 -fno-pic -fno-pie -mcmodel=small -ffixed-x18 -mno-outline-atomics \
  -ffunction-sections -fdata-sections -fno-common -fno-exceptions -fno-unwind-tables \
  -fno-asynchronous-unwind-tables -fno-math-errno -Isdk -Isdk/rmp -Ishim -Wno-unused-variable"
CFLAGS="-ffreestanding -fno-tree-loop-distribute-patterns"
B=build
mkdir -p "$B"

${A64}gcc $FLAGS $CFLAGS -c -o $B/ledrum.o          ledrum.c
${A64}gcc $FLAGS $CFLAGS -c -o $B/wavetable.o       lxr/wavetable.c
${A64}gcc $FLAGS $CFLAGS -c -o $B/transientTables.o lxr/transientTables.c
${A64}gcc $FLAGS $CFLAGS -c -o $B/rmp_libc.o        sdk/rmp/shim/rmp_libc.c
${A64}g++ $FLAGS -fno-builtin -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
  -c -o $B/rmp_cxx.o sdk/rmp/shim/rmp_cxx.cpp

${A64}g++ $FLAGS -static -nostdlib -nostartfiles \
  -Wl,--emit-relocs -Wl,--gc-sections -Wl,-z,max-page-size=4096 \
  -Wl,--no-warn-rwx-segments -Wl,-T,sdk/rmp/plugin_a64.ld \
  -o $B/ledrum.a64.elf \
  $B/ledrum.o $B/wavetable.o $B/transientTables.o $B/rmp_libc.o $B/rmp_cxx.o -lgcc

python3 tools/rmp_pack.py $B/ledrum.a64.elf manifest.json ledrum.a64.rmp
echo "Built ledrum.a64.rmp"
