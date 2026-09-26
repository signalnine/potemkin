# Build potemkin targets. Run with: bash tools/build.sh [test|init|all]
# Q27 points at a q27 checkout (sources + build/pf4.o); defaults to q27-master.
set -euo pipefail
cd "$(dirname "$0")/.."
Q27=${Q27:-/mnt/ai/projects/q27-master}
NVCC=${NVCC:-/usr/local/cuda/bin/nvcc}
ARCH=${ARCH:-86}
what=${1:-all}
mkdir -p build
CXXF="-O1 -g -std=c++17 -Wall -Wextra -I$Q27/third_party"

if [[ $what == test || $what == all ]]; then
  g++ $CXXF -o build/test_host tests/test_host.cpp src/host/host.cpp
  g++ $CXXF -o build/test_harness tests/test_harness.cpp src/harness/harness.cpp src/host/host.cpp
  ./build/test_host | tail -1
  ./build/test_harness | tail -1
fi

if [[ $what == init || $what == all ]]; then
  # The 12g engine shape (W_MAX=8, 256-row prefill); pair with slim packs.
  $NVCC -O2 -std=c++17 -gencode arch=compute_$ARCH,code=sm_$ARCH -Xcompiler -Wall \
    -DQ27_W_MAX=8 -DQ27_PF_T=256 -Xcompiler -pthread -I"$Q27/src" -I"$Q27/third_party" \
    src/main.cpp src/q27/backend_q27.cu src/harness/harness.cpp src/host/host.cpp \
    "$Q27"/src/{dflash2,blocks,prefill,kernels,spec3,vgemm,device_model}.cu \
    "$Q27/src/loader.cpp" "$Q27/src/tokenizer.cpp" "$Q27/build/pf4.o" \
    -Xcompiler -static-libstdc++,-static-libgcc -o build/q27-init-sm$ARCH
  echo "built build/q27-init-sm$ARCH"
fi
