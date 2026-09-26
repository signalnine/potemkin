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
  gcc -O2 -Wall -Wextra -static -o build/init src/init/init.c
  g++ $CXXF -o build/test_host tests/test_host.cpp src/host/host.cpp
  g++ $CXXF -o build/test_harness tests/test_harness.cpp src/harness/harness.cpp src/host/host.cpp
  g++ $CXXF -pthread -o build/test_api tests/test_api.cpp src/api/backend_api.cpp src/harness/harness.cpp src/host/host.cpp
  # Tests spawn, signal and read devices; run each binary in its own session
  # with a memory cap so a bad test cannot take the terminal down with it.
  for t in test_host test_harness test_api; do
    setsid bash -c "ulimit -v 4000000; timeout -s KILL 600 ./build/$t" < /dev/null 2>&1 | tail -1
  done
fi

if [[ $what == init || $what == all ]]; then
  # PROFILE=12g: the 3060 engine shape (sm_86 only, W_MAX=8, 256-row prefill;
  # pair with slim packs). PROFILE=full: q27's default tri-arch shape
  # (sm_86/89/120, W_MAX=12, 1024-row prefill) for 24 GB+ cards.
  PROFILE=${PROFILE:-12g}
  TRI="-gencode arch=compute_86,code=sm_86 -gencode arch=compute_89,code=sm_89 -gencode arch=compute_120,code=sm_120"
  if [[ $PROFILE == full ]]; then
    GEN=$TRI; SHAPE=""; ARCH=full
  elif [[ $PROFILE == w8 ]]; then
    # 24 GB cards: the width-12 graph set OOMs at setup there (q27 README).
    GEN=$TRI; SHAPE="-DQ27_W_MAX=8"; ARCH=w8
  else
    GEN="-gencode arch=compute_$ARCH,code=sm_$ARCH"
    SHAPE="-DQ27_W_MAX=8 -DQ27_PF_T=256"
  fi
  $NVCC -O2 -std=c++17 $GEN -Xcompiler -Wall \
    $SHAPE -Xcompiler -pthread -I"$Q27/src" -I"$Q27/third_party" \
    -DPK_TLS src/main.cpp src/q27/backend_q27.cu src/api/backend_api.cpp src/harness/harness.cpp src/host/host.cpp \
    "$Q27"/src/{dflash2,blocks,prefill,kernels,spec3,vgemm,device_model}.cu \
    "$Q27/src/loader.cpp" "$Q27/src/tokenizer.cpp" "$Q27/build/pf4.o" \
    /usr/lib/x86_64-linux-gnu/libssl.a /usr/lib/x86_64-linux-gnu/libcrypto.a \
    -Xcompiler -static-libstdc++,-static-libgcc -o build/q27-init-sm$ARCH.tmp
  mv build/q27-init-sm$ARCH.tmp build/q27-init-sm$ARCH
  echo "built build/q27-init-sm$ARCH"
fi
