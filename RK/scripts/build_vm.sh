#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
export RK_CXX=${RK_CXX:-/usr/bin/aarch64-linux-gnu-g++}
export RK_SYSROOT=${RK_SYSROOT:-/home/alientek/rk3568_linux_sdk/debian/binary}
export RKNN_INCLUDE_DIR=${RKNN_INCLUDE_DIR:-/home/alientek/rk3568_linux_sdk/external/rknpu2/runtime/RK356X/Linux/librknn_api/include}
task_qemu=${RK_QEMU:-/home/alientek/rk3568_lane_suite_20261005/host_tools/usr/bin/qemu-aarch64}
export LD_LIBRARY_PATH="$PWD/lib:$RK_SYSROOT/usr/lib/aarch64-linux-gnu"
cmake -S . -B build-signal -DCMAKE_BUILD_TYPE=Release -DBUILD_APP=ON -DBUILD_TESTING=ON \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_CROSSCOMPILING_EMULATOR="$task_qemu;-L;$RK_SYSROOT"
cmake --build build-signal --parallel "${BUILD_JOBS:-4}"
(cd build-signal && ctest --output-on-failure)
"$task_qemu" -L "$RK_SYSROOT" build-signal/rk_signal_control --help
