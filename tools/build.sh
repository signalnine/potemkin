# Build potemkin targets. Run with: bash tools/build.sh [test|api|init|all]
#   test  /sbin/init + CPU unit tests          (gcc, g++)
#   api   q27-init for llm=api, no CUDA        (g++, libssl-dev)
#   init  q27-init with the q27 GPU engine     (CUDA 12.8+, a q27 checkout)
#   all   test + init
# Q27 points at a q27 checkout (its sources and build/pf4.o), needed for init only.
set -euo pipefail
cd "$(dirname "$0")/.."
Q27=${Q27:-../q27}
NVCC=${NVCC:-/usr/local/cuda/bin/nvcc}
ARCH=${ARCH:-86}
what=${1:-all}
mkdir -p build
CXXF="-O1 -g -std=c++17 -Wall -Wextra -Ithird_party"
SSL=/usr/lib/x86_64-linux-gnu
need_ssl() {
  [[ -f $SSL/libssl.a && -f $SSL/libcrypto.a ]] && return
  echo "build.sh: static OpenSSL not found in $SSL (install libssl-dev)" >&2; exit 1
}

if [[ $what == api ]]; then
  need_ssl
  g++ -O2 -std=c++17 -Wall -Wextra -Ithird_party -DPK_TLS -pthread \
    src/main.cpp src/q27/backend_stub.cpp src/api/backend_api.cpp src/harness/harness.cpp src/host/host.cpp \
    $SSL/libssl.a $SSL/libcrypto.a -ldl -static-libstdc++ -static-libgcc -o build/q27-init-api.tmp
  mv build/q27-init-api.tmp build/q27-init-api
  echo "built build/q27-init-api (llm=api only)"
fi

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
  if [[ ! -f $Q27/src/engine.cuh || ! -f $Q27/build/pf4.o ]]; then
    echo "build.sh: no q27 checkout with build/pf4.o at Q27=$Q27 (see README, 'With a GPU')" >&2; exit 1
  fi
  need_ssl
  # PROFILE=12g: the 12 GB-and-under shape (sm_86 only, W_MAX=8, 256-row
  # prefill; pair with slim packs). PROFILE=w8: tri-arch for 24 GB cards.
  # PROFILE=full: q27's default tri-arch shape (W_MAX=12) for 32 GB cards.
  PROFILE=${PROFILE:-12g}
  TRI="-gencode arch=compute_86,code=sm_86 -gencode arch=compute_89,code=sm_89 -gencode arch=compute_120,code=sm_120"
  if [[ $PROFILE == full ]]; then
    GEN=$TRI; SHAPE=""
  elif [[ $PROFILE == w8 ]]; then
    # 24 GB cards: the width-12 graph set OOMs at setup there (q27 README).
    GEN=$TRI; SHAPE="-DQ27_W_MAX=8"
  else
    GEN="-gencode arch=compute_$ARCH,code=sm_$ARCH"
    SHAPE="-DQ27_W_MAX=8 -DQ27_PF_T=256"
  fi
  $NVCC -O2 -std=c++17 $GEN -Xcompiler -Wall \
    $SHAPE -Xcompiler -pthread -I"$Q27/src" -I"$Q27/third_party" \
    -Ithird_party -DPK_TLS src/main.cpp src/q27/backend_q27.cu src/api/backend_api.cpp src/harness/harness.cpp src/host/host.cpp \
    "$Q27"/src/{dflash2,blocks,prefill,kernels,spec3,vgemm,device_model}.cu \
    "$Q27/src/loader.cpp" "$Q27/src/tokenizer.cpp" "$Q27/build/pf4.o" \
    $SSL/libssl.a $SSL/libcrypto.a \
    -Xcompiler -static-libstdc++,-static-libgcc -o build/q27-init-$PROFILE.tmp
  mv build/q27-init-$PROFILE.tmp build/q27-init-$PROFILE
  echo "built build/q27-init-$PROFILE"
fi
