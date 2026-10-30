# aien-gpu: Linux host shim for our own GB10 GPU driver

Part of ledger L332 (docs/GPU_DRIVER.md). A Linux kernel module that will host our GB10 driver core
(native/kernel/gpu) in place of NVIDIA's nvidia.ko and nvidia-uvm.ko.

Today it binds the GB10 only when loaded with `allow_bind=1` (never while nvidia.ko owns the GPU), reads two identity
registers (no write), loads NVIDIA's GSP firmware file and checks its size, allocates one DMA-coherent arena and offers it
by mmap of /dev/aien-gpu. `aien_gpu_check.c` is the user-space check for that.

Build: `make` (needs /lib/modules/$(uname -r)/build). Loading it is a coordinator-scheduled window step:
evidence/own-gpu-driver/rung2/RUNBOOK.md. Licence GPL-2.0-only because a Linux module using GPL-only kernel symbols must
be; the driver core stays AGPL.
