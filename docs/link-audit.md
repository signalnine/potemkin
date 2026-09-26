# Link audit: q27-server-12g (PoC step 1, pk-5vq.1)

2026-09-25. q27-master at `8be624e` (v0.14.1), binary `build/q27-server-12g`
(sm_86 only, W_MAX=8, PF_T=256). Run on the 3090 (sm_86, same arch as the 3060)
with Bonsai 2 `bonsai2-27b-t3-slim.q27` + `qwen38-27b-mtp.tok`, driver 580.119.02,
kernel 6.18.7, CUDA 13.2, glibc 2.39. One chat completion under `strace -f`:
ready in ~6 s, 79 t/s decode.

## Link-time

`readelf -d` NEEDED: `libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6 ld-linux-x86-64.so.2`.
cudart is already static (nvcc default). No cuBLAS/cuDNN.

Fully static (`-Xlinker -static`) does not work and cannot: cudart_static
`dlopen`s `libcuda.so.1`, which is a glibc-linked shared object, so a dynamic
glibc has to be in the image regardless. Verified on a probe:
`-Xcompiler -static-libstdc++,-static-libgcc` leaves only `libc.so.6` +
`ld-linux` NEEDED and runs a kernel correctly. Use that for `q27-init`.

## Runtime (from strace)

Shared objects actually loaded:

| File | Size | Why |
| --- | --: | --- |
| `ld-linux-x86-64.so.2` | 0.2 MB | loader |
| `libc.so.6` | 2.1 MB | q27 + libcuda |
| `libm.so.6` | 0.9 MB | q27 + libcuda |
| `libdl.so.2`, `libpthread.so.0`, `librt.so.1` | 14 KB each | libcuda's NEEDED (glibc 2.34+ stubs) |
| `libstdc++.so.6`, `libgcc_s.so.1` | 2.6 + 0.2 MB | drop with `-static-libstdc++ -static-libgcc` |
| `libcuda.so.580.119.02` (as `libcuda.so.1`) | 96 MB | driver API, must match the kernel module version |
| `libnvidia-ptxjitcompiler.so.1` | 39 MB | only loaded when no SASS matches the GPU (seen when the sm_86 probe hit the 5090). Not needed if every target arch is compiled in; ship it only as a fallback (e.g. Orin before sm_87 lands) |

Device nodes: `/dev/nvidiactl`, `/dev/nvidia0..N`, `/dev/nvidia-uvm`
(`/dev/nvidia-uvm-tools` opened by the probe only). With no udev and no
`nvidia-modprobe`, `/sbin/init` must `insmod nvidia.ko nvidia-uvm.ko` and
`mknod` these itself: nvidiactl = 195:255, nvidiaN = 195:N, nvidia-uvm =
`<major from /proc/devices "nvidia-uvm">`:0 (507 on this box; dynamic).

Pseudo-fs reads: `/proc/{cpuinfo,devices,self/maps,self/status,sys/vm/mmap_min_addr}`,
`/proc/driver/nvidia/params`, `/sys/devices/system/{cpu/online,memory/block_size_bytes,node/*}`,
`/sys/bus/pci/devices/*/numa_node`. So init mounts `/proc` and `/sys`.

Other files: `/etc/ld.so.cache` (optional; absent means default paths),
`/etc/localtime` (optional). Sockets: the HTTP listener (not used in the
console path) and an abstract AF_UNIX socket CUDA uses for UVM fd passing,
which needs no filesystem entry. No NSS lookups with a numeric host, but
`getaddrinfo` is linked (httplib); if the console build drops HTTP, it goes away.
`dlopen`/`dlsym` are linked by cudart, not q27.

## Kernel side

| Item | Size | Notes |
| --- | --: | --- |
| `nvidia.ko` | 18.7 MB | 580.119.02 open module (dkms build) |
| `nvidia-uvm.ko` | 4.1 MB | required (cudaMalloc path) |
| `nvidia-modeset.ko`, `nvidia-drm.ko` | 3.2 + 0.3 MB | not needed for compute; fbcon can use simpledrm/efifb (both `=y` here) |
| GSP firmware `gsp_ga10x.bin` | 72 MB | Ampere and newer; `gsp_tu10x.bin` (30 MB) only for Turing |
| vmlinuz | 16.8 MB | stock distro kernel |

Host kernel config already has what the design needs built in: cgroups,
memcg, pids controller, simpledrm, efifb, fbcon. `btrfs` and `overlay` are
modules (`=m`), so they go in the initramfs too. `ip=dhcp` needs
`CONFIG_IP_PNP_DHCP`, which the stock config lacks: build our own kernel or
have `q27-init` do DHCP.

## Bottom line

Trusted base for the Bonsai/3060 path, excluding weights: ~6 MB glibc +
96 MB libcuda + 23 MB modules + 72 MB GSP firmware + 17 MB kernel + ~11 MB
engine = **~225 MB**, of which 190 MB is NVIDIA. Add 39 MB if the PTX JIT
fallback ships.

## Follow-ups this surfaced

- `q27-init`: link with `-static-libstdc++ -static-libgcc`.
- `/sbin/init`: insmod + mknod for the NVIDIA nodes, mount proc/sys (pk-5vq.3.1).
- `ip=dhcp` is not in the stock kernel config (pk-5vq.3.5 / pk-5vq.10).
- tcc and musl are not installed on the build host; the image needs musl
  `libc.a` + crt objects, not just headers, for tcc to link anything (pk-5vq.2.6).
- The prefix cache wrote nothing for a 73-token prompt (below
  `--prefix-cache-min`); verify the persistence path in pk-5vq.2.7.
