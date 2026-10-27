#!/bin/sh
# No game files, network server or iOS device required.
set -eu
cd "$(dirname "$0")/.."
mkdir -p Build/Probe
xcrun clang -std=c11 -O2 -DPG_LOCAL_ASSET_TESTING -I Source -c Source/NativeLocalAssetReader.c -o Build/Probe/local-asset-reader.o
xcrun clang -std=c11 -O2 -DPG_LOCAL_ASSET_TESTING -fobjc-arc -fblocks -I Source -I Generated -I Tools/wabt-1.0.42/include \
  -I Tools/wabt-1.0.42/share/wabt/wasm2c -framework Foundation Tests/local_asset_reads.m Build/Probe/local-asset-reader.o -o Build/Probe/local-asset-reads
Build/Probe/local-asset-reads
