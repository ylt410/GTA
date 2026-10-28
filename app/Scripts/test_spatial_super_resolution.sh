#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
app_dir="$(cd "$script_dir/.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

swiftc "$app_dir/App/SpatialSuperResolution.swift" \
  "$script_dir/SpatialSuperResolutionSizingCheck.swift" \
  -o "$tmp_dir/spatial-super-resolution-sizing-check"
"$tmp_dir/spatial-super-resolution-sizing-check"
