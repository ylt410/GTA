#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -pthread \
  "$root/Tests/audio_decoder_cache.c" -o "$tmp/audio_decoder_cache"
"$tmp/audio_decoder_cache"
