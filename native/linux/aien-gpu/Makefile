# aien-gpu: the Linux host shim for our own GB10 GPU driver (L332 rung 2). Build only; loading it is a
# coordinator-scheduled window step (docs/GPU_DRIVER.md, evidence/own-gpu-driver/rung2/RUNBOOK.md).
#   make            the module (needs /lib/modules/$(uname -r)/build)
#   make tools      aien_gpu_check and aien_gsp_boot (user space; the start-up tool links the AGPL driver core)
#   make sim-test   aien_gsp_boot built against the simulated SEC2 + GSP, plain and sanitized, then
#                   evidence/own-gpu-driver/rung2/window1_sim_test.sh on both
KDIR ?= /lib/modules/$(shell uname -r)/build
CORE := $(CURDIR)/../../kernel
OUT ?= $(CURDIR)/build
CC ?= cc
TCFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -Werror \
	-I$(CORE)/gpu -I$(CORE)/dev -I$(CURDIR)
BOOT_SRCS := aien_gsp_boot.c $(CORE)/gpu/gsp_mmio.c $(CORE)/gpu/gsp_seq.c $(CORE)/dev/gb10_gsp_boot.c
SIM_TEST := $(CURDIR)/../../../evidence/own-gpu-driver/rung2/window1_sim_test.sh
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
tools: $(OUT)/aien_gpu_check $(OUT)/aien_gsp_boot
$(OUT)/aien_gpu_check: aien_gpu_check.c aien_gpu_uapi.h
	@mkdir -p $(OUT)
	$(CC) $(TCFLAGS) -o $@ aien_gpu_check.c
$(OUT)/aien_gsp_boot: $(BOOT_SRCS) aien_gpu_uapi.h
	@mkdir -p $(OUT)
	$(CC) $(TCFLAGS) -o $@ $(BOOT_SRCS)
$(OUT)/aien_gsp_boot_sim: $(BOOT_SRCS) $(CORE)/gpu/gsp_sim.c
	@mkdir -p $(OUT)
	$(CC) $(TCFLAGS) -DAIEN_GSP_BOOT_SIM -o $@ $(BOOT_SRCS) $(CORE)/gpu/gsp_sim.c
$(OUT)/aien_gsp_boot_sim_san: $(BOOT_SRCS) $(CORE)/gpu/gsp_sim.c
	@mkdir -p $(OUT)
	$(CC) $(TCFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -DAIEN_GSP_BOOT_SIM -o $@ \
		$(BOOT_SRCS) $(CORE)/gpu/gsp_sim.c
sim-test: $(OUT)/aien_gsp_boot_sim $(OUT)/aien_gsp_boot_sim_san
	bash $(SIM_TEST) $(OUT)/aien_gsp_boot_sim $(CURDIR)/aien_gpu.c
	bash $(SIM_TEST) $(OUT)/aien_gsp_boot_sim_san $(CURDIR)/aien_gpu.c
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf $(OUT)
.PHONY: all tools sim-test clean
