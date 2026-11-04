#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
/home/alientek/anaconda3/envs/yolo11_export/bin/python conversion/export_detector.py
/home/alientek/anaconda3/envs/rknn_tookit2/bin/python conversion/build_models.py
