#!/bin/bash

set -euo pipefail

CONF_FLAGS=(
  -i ./in.flac
  -ar 44100
  -af aresample=44100,adumpwave=w=1800:n=3577:f=out.csv
  -c:a libfdk_aac -b:a 160k
  -f mp4
  out.m4a
)

./ffmpeg "${CONF_FLAGS[@]}"