#!/bin/bash
set -euo pipefail
APP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/bike-input-axes.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
swiftc "$APP_ROOT/App/BikeInputAxes.swift" "$APP_ROOT/Tests/bike_input_axes.swift" -o "$WORK/checks"
"$WORK/checks"
