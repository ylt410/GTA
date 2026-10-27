#!/bin/sh
# BC1 -> ASTC rewrite checked on this Mac's GPU (needs an Apple silicon Mac and Tools/bcdec from prepare.py).
set -eu
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/bc1_astc_check"
xcrun clang -O2 -fobjc-arc -ITools/bcdec -ISource Tests/bc1_astc_metal.m Source/BC1ToASTC.c -framework Foundation -framework Metal -o "$out"
"$out"
