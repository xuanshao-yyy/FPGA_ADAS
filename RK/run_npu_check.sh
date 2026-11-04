#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec ./rk_signal_control --headless --check \
  --model models/traffic_yolo11n_640x640_i8.rknn \
  --front-model models/traffic_yolo11n_640x384_i8.rknn \
  --lane-model models/small4_pad1_i8.rknn --tensor-output native \
  --light-rgb-model models/light_rgb_128_i8_kl256.rknn \
  --output-root results/MODEL_STARTUP_CHECK "$@"
