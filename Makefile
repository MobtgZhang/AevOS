# ============================================================
#  AevOS — Autonomous Evolving OS
#  Multi-Architecture Makefile (UEFI boot, C17)
#
#  Supported: ARCH = x86_64 | aarch64 | riscv64 | loongarch64
#  Usage:     make ARCH=x86_64
#             make ARCH=aarch64
#             make clean
# ============================================================

ARCH ?= x86_64
SRCDIR := src

# 1=内核内嵌 GGUF 推理；0=llm_runtime 桩 + Cap-IPC 占位（用户态 LLM 服务）
AEVOS_EMBED_LLM ?= 1

# LoongArch Virt EDK2 (optional; override if firmware lives elsewhere)
AEVOS_LOONGARCH_FW ?= /home/mobtgzhang/Firmware/LoongArchVirtMachine

# 预编译 UEFI 固件目录（与 https://retrage.github.io/edk2-nightly/ 命名一致）。
# 运行: make fetch-uefi-firmware ARCH=<arch>
AEVOS_UEFI_FW ?= firmware/uefi
EDK2_NIGHTLY_BIN := https://retrage.github.io/edk2-nightly/bin

# ============================================================
#  Architecture-specific configuration
# ============================================================

ifeq ($(ARCH),x86_64)
  CROSS_PREFIX   := x86_64-elf-
  CROSS_PREFIX2  := x86_64-linux-gnu-
  ARCH_SUBDIR    := x86_64
  KCFLAGS_ARCH   := -mno-red-zone -mcmodel=large -fno-pic -fno-pie
  BOOT_CFLAGS_ARCH := -mno-red-zone -fshort-wchar -maccumulate-outgoing-args
  KERNEL_LDS     := $(SRCDIR)/kernel/arch/x86_64/kernel.lds
  BOOT_LDS       := $(SRCDIR)/boot/linker_boot.lds
  EFI_TARGET_FMT := efi-app-x86_64
  EFI_BOOT_NAME  := BOOTX64.EFI
  BOOT_ELF_FMT   := elf64-x86-64
  QEMU_CMD       := qemu-system-x86_64
  QEMU_EXTRA     :=
  QEMU_RUN_DEVS  := -device usb-ehci -device usb-mouse -vga std

else ifeq ($(ARCH),aarch64)
  CROSS_PREFIX   := aarch64-elf-
  CROSS_PREFIX2  := aarch64-linux-gnu-
  ARCH_SUBDIR    := aarch64
  KCFLAGS_ARCH   := -mno-outline-atomics
  BOOT_CFLAGS_ARCH :=
  KERNEL_LDS     := $(SRCDIR)/kernel/arch/aarch64/kernel.lds
  BOOT_LDS       := $(SRCDIR)/boot/linker_boot.lds
  EFI_TARGET_FMT := efi-app-aarch64
  EFI_BOOT_NAME  := BOOTAA64.EFI
  BOOT_ELF_FMT   := elf64-littleaarch64
  QEMU_CMD       := qemu-system-aarch64
  QEMU_EXTRA     := -M virt -cpu cortex-a72 -smp 2 \
                    -device virtio-gpu-pci \
                    -device virtio-keyboard-pci \
                    -device virtio-mouse-pci
  QEMU_RUN_DEVS  :=

else ifeq ($(ARCH),riscv64)
  CROSS_PREFIX   := riscv64-elf-
  CROSS_PREFIX2  := riscv64-linux-gnu-
  ARCH_SUBDIR    := riscv64
  KCFLAGS_ARCH   := -march=rv64gc -mabi=lp64d -mcmodel=medany
  # PE/COFF EFI .so 链接需要 PIC，否则 HI20 重定位在 -shared 下失败
  BOOT_CFLAGS_ARCH := -fPIC
  KERNEL_LDS     := $(SRCDIR)/kernel/arch/riscv64/kernel.lds
  BOOT_LDS       := $(SRCDIR)/boot/linker_boot.lds
  EFI_TARGET_FMT := efi-app-riscv64
  EFI_BOOT_NAME  := BOOTRISCV64.EFI
  BOOT_ELF_FMT   := elf64-littleriscv
  QEMU_CMD       := qemu-system-riscv64
  QEMU_EXTRA     := -M virt -smp 1 \
                    -device virtio-gpu-pci \
                    -device virtio-keyboard-pci \
                    -device virtio-mouse-pci
  QEMU_RUN_DEVS  :=

else ifeq ($(ARCH),loongarch64)
  CROSS_PREFIX   := loongarch64-unknown-linux-gnu-
  CROSS_PREFIX2  := loongarch64-linux-gnu-
  ARCH_SUBDIR    := loongarch64
  KCFLAGS_ARCH   := -mno-lsx -mno-lasx
  BOOT_CFLAGS_ARCH := -mno-lsx -mno-lasx
  KERNEL_LDS     := $(SRCDIR)/kernel/arch/loongarch64/kernel.lds
  BOOT_LDS       := $(SRCDIR)/boot/linker_boot.lds
  EFI_TARGET_FMT := pei-loongarch64
  EFI_BOOT_NAME  := BOOTLOONGARCH64.EFI
  BOOT_ELF_FMT   := elf64-loongarch
  QEMU_CMD       := qemu-system-loongarch64
  QEMU_EXTRA     := -M virt -cpu la464 -smp 2 \
                    -device virtio-gpu-pci \
                    -device virtio-keyboard-pci \
                    -device virtio-mouse-pci
  QEMU_RUN_DEVS  :=

else
  $(error Unsupported ARCH=$(ARCH). Choose: x86_64, aarch64, riscv64, loongarch64)
endif

# ---- Toolchain (try bare-metal, then linux-gnu, then host) ----
CC      := $(CROSS_PREFIX)gcc
AS      := $(CROSS_PREFIX)as
LD      := $(CROSS_PREFIX)ld
OBJCOPY := $(CROSS_PREFIX)objcopy

ifeq ($(shell which $(CC) 2>/dev/null),)
  CC      := $(CROSS_PREFIX2)gcc
  AS      := $(CROSS_PREFIX2)as
  LD      := $(CROSS_PREFIX2)ld
  OBJCOPY := $(CROSS_PREFIX2)objcopy
endif
ifeq ($(shell which $(CC) 2>/dev/null),)
  CC      := $(CROSS_PREFIX2)gcc-14
  AS      := $(CROSS_PREFIX2)as
  LD      := $(CROSS_PREFIX2)ld
  OBJCOPY := $(CROSS_PREFIX2)objcopy
endif
ifeq ($(shell which $(CC) 2>/dev/null),)
  CC      := gcc
  AS      := as
  LD      := ld
  OBJCOPY := objcopy
endif

# ============================================================
#  Common flags
# ============================================================

CFLAGS_COMMON := -std=c17 -Wall -Wextra -Werror=implicit-function-declaration \
                 -O2 -pipe -I$(SRCDIR)/include -I$(SRCDIR)

# ---- Kernel flags ----
KCFLAGS := $(CFLAGS_COMMON) \
           -ffreestanding -nostdlib -nostdinc \
           -fno-stack-protector \
           $(KCFLAGS_ARCH) \
           -DAEVOS_KERNEL \
           -DAEVOS_EMBED_LLM=$(AEVOS_EMBED_LLM)

LIBGCC := $(shell $(CC) $(KCFLAGS_ARCH) -print-libgcc-file-name 2>/dev/null)
KLDFLAGS := -nostdlib -static -T $(KERNEL_LDS)

# ---- Boot (UEFI) flags ----
GNUEFI_DIR   := /usr/lib
GNUEFI_INC   := /usr/include/efi
GNUEFI_CRT0  := $(GNUEFI_DIR)/crt0-efi-x86_64.o

ifeq ($(ARCH),x86_64)
BOOT_CFLAGS := -std=c17 -Wall -Wextra -O2 \
               -ffreestanding -fno-stack-protector \
               -fpic -fshort-wchar \
               -I$(SRCDIR)/include \
               $(BOOT_CFLAGS_ARCH)
else
BOOT_CFLAGS := -std=c17 -Wall -Wextra -O2 \
               -ffreestanding -fno-stack-protector \
               -fno-pic -fno-pie -fshort-wchar \
               -I$(SRCDIR)/include \
               $(BOOT_CFLAGS_ARCH)
endif

ifeq ($(ARCH),x86_64)
  BOOT_GNUEFI_CRT0 := $(GNUEFI_DIR)/crt0-efi-x86_64.o
  BOOT_LDFLAGS := -nostdlib -znocombreloc -shared -Bsymbolic \
                  -T /usr/lib/elf_x86_64_efi.lds \
                  -L$(GNUEFI_DIR) -lgnuefi -lefi
else
  BOOT_GNUEFI_CRT0 :=
  BOOT_LDFLAGS := -nostdlib -znocombreloc -shared -Bsymbolic \
                  -T $(BOOT_LDS)
endif

# ============================================================
#  Directory structure
# ============================================================

BOOT_DIR     := $(SRCDIR)/boot
KERNEL_DIR   := $(SRCDIR)/kernel
KERNEL_ARCH  := $(SRCDIR)/kernel/arch/$(ARCH_SUBDIR)
KERNEL_MM    := $(KERNEL_DIR)/mm
KERNEL_SCHED := $(KERNEL_DIR)/sched
KERNEL_DRV   := $(KERNEL_DIR)/drivers
KERNEL_FS    := $(KERNEL_DIR)/fs
KERNEL_NET   := $(KERNEL_DIR)/net
AGENT_DIR    := $(SRCDIR)/agent
LLM_DIR      := $(SRCDIR)/llm
UI_DIR       := $(SRCDIR)/ui
POSIX_DIR    := $(SRCDIR)/posix
LIB_DIR      := $(SRCDIR)/lib
DB_DIR       := $(SRCDIR)/db
CONTAINER_DIR := $(SRCDIR)/container
LINUX_DIR     := $(SRCDIR)/linux
IPC_DIR       := $(SRCDIR)/kernel/ipc
EVOLUTION_DIR := $(SRCDIR)/evolution
LLM_DIR       := $(SRCDIR)/llm
TOOLS_DIR    := $(SRCDIR)/tools

BUILD_DIR    := build/$(ARCH)
BOOT_BUILD   := $(BUILD_DIR)/boot
KERNEL_BUILD := $(BUILD_DIR)/kernel

# ============================================================
#  Source collection
# ============================================================

BOOT_CSRC := $(wildcard $(BOOT_DIR)/*.c)
BOOT_COBJS := $(patsubst $(BOOT_DIR)/%.c, $(BOOT_BUILD)/%.o, $(BOOT_CSRC))

# Non-x86 arches need a reloc stub for PE/COFF (gnu-efi CRT provides this on x86_64)
ifneq ($(ARCH),x86_64)
  BOOT_RELOC_SRC := $(BOOT_DIR)/reloc_dummy.S
  BOOT_RELOC_OBJ := $(BOOT_BUILD)/reloc_dummy.o
else
  BOOT_RELOC_OBJ :=
endif

BOOT_OBJS := $(BOOT_COBJS) $(BOOT_RELOC_OBJ)

LLM_ALWAYS_SRCS := $(LLM_DIR)/llm_api_client.c \
                   $(LLM_DIR)/llm_tool_router.c \
                   $(LLM_DIR)/llm_syscall.c \
                   $(LLM_DIR)/llm_ipc.c

LLM_EMBED_SRCS := $(LLM_DIR)/llm_runtime.c \
                  $(LLM_DIR)/gguf_loader.c \
                  $(LLM_DIR)/safetensors_loader.c \
                  $(LLM_DIR)/quantize.c \
                  $(LLM_DIR)/simd_kernels.c

ifeq ($(AEVOS_EMBED_LLM),1)
  LLM_KERNEL_SRCS := $(LLM_ALWAYS_SRCS) $(LLM_EMBED_SRCS)
else
  LLM_KERNEL_SRCS := $(LLM_ALWAYS_SRCS) $(LLM_DIR)/llm_runtime_stub.c
endif

KERNEL_CSRC := $(wildcard $(KERNEL_DIR)/*.c) \
               $(wildcard $(KERNEL_ARCH)/*.c) \
               $(wildcard $(KERNEL_MM)/*.c) \
               $(wildcard $(KERNEL_SCHED)/*.c) \
               $(wildcard $(KERNEL_DRV)/*.c) \
               $(wildcard $(KERNEL_FS)/*.c) \
               $(wildcard $(KERNEL_NET)/*.c) \
               $(wildcard $(IPC_DIR)/*.c) \
               $(wildcard $(AGENT_DIR)/*.c) \
               $(LLM_KERNEL_SRCS) \
               $(wildcard $(UI_DIR)/*.c) \
               $(wildcard $(POSIX_DIR)/*.c) \
               $(wildcard $(LIB_DIR)/*.c) \
               $(wildcard $(DB_DIR)/*.c) \
               $(wildcard $(CONTAINER_DIR)/*.c) \
               $(wildcard $(LINUX_DIR)/*.c) \
               $(wildcard $(EVOLUTION_DIR)/*.c)

KERNEL_ASRC := $(wildcard $(KERNEL_ARCH)/*.S) \
               $(wildcard $(KERNEL_SCHED)/*.S)

KERNEL_COBJS := $(patsubst %.c, $(BUILD_DIR)/%.o, $(KERNEL_CSRC))
KERNEL_AOBJS := $(patsubst %.S, $(BUILD_DIR)/%.S.o, $(KERNEL_ASRC))
KERNEL_OBJS  := $(KERNEL_COBJS) $(KERNEL_AOBJS)

# ---- Output files ----
BOOT_EFI    := $(BUILD_DIR)/aevos_boot.efi
BOOT_SO     := $(BUILD_DIR)/aevos_boot.so
KERNEL_ELF  := $(BUILD_DIR)/kernel.elf
DISK_IMAGE  := $(BUILD_DIR)/aevos.img

# RISC-V virt + EDK2：默认启动项常为 PXE；virtio-blk + bootindex 优先从 ESP 加载 BOOTRISCV64.EFI
QEMU_DISK_ARGS := -drive format=raw,file=$(DISK_IMAGE)
ifeq ($(ARCH),riscv64)
  QEMU_DISK_ARGS := -drive if=none,format=raw,id=aevosdisk,file=$(DISK_IMAGE) \
                    -device virtio-blk-pci,drive=aevosdisk,bootindex=1
endif

OVMF_VARS_WORK := $(BUILD_DIR)/ovmf_vars.fd

# UEFI pflash firmware paths（优先 AEVOS_UEFI_FW 下 edk2-nightly 文件名）
ifeq ($(ARCH),x86_64)
  OVMF_CODE := $(firstword $(foreach f,$(AEVOS_UEFI_FW)/DEBUGX64_OVMF_CODE.fd $(AEVOS_UEFI_FW)/RELEASEX64_OVMF_CODE.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF.fd /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd,$(if $(wildcard $(f)),$(f),)))
  ifneq ($(findstring 4M,$(notdir $(OVMF_CODE))),)
    OVMF_VARS_SRC := $(firstword $(foreach f,/usr/share/OVMF/OVMF_VARS_4M.fd,$(if $(wildcard $(f)),$(f),)))
  else ifneq ($(OVMF_CODE),)
    OVMF_VARS_SRC := $(firstword $(foreach f,$(AEVOS_UEFI_FW)/DEBUGX64_OVMF_VARS.fd $(AEVOS_UEFI_FW)/RELEASEX64_OVMF_VARS.fd /usr/share/OVMF/OVMF_VARS.fd,$(if $(wildcard $(f)),$(f),)))
  endif
else ifeq ($(ARCH),aarch64)
  OVMF_CODE := $(firstword $(foreach f,$(AEVOS_UEFI_FW)/DEBUGAARCH64_QEMU_EFI.fd $(AEVOS_UEFI_FW)/RELEASEAARCH64_QEMU_EFI.fd /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/edk2/aarch64/QEMU_EFI.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd,$(if $(wildcard $(f)),$(f),)))
  OVMF_VARS_SRC := $(firstword $(foreach f,$(AEVOS_UEFI_FW)/DEBUGAARCH64_QEMU_VARS.fd $(AEVOS_UEFI_FW)/RELEASEAARCH64_QEMU_VARS.fd /usr/share/AAVMF/AAVMF_VARS.fd /usr/share/edk2/aarch64/vars-template-pflash.raw,$(if $(wildcard $(f)),$(f),)))
else ifeq ($(ARCH),riscv64)
  OVMF_CODE := $(firstword $(foreach f,/usr/share/qemu-efi-riscv64/RISCV_VIRT_CODE.fd $(AEVOS_UEFI_FW)/DEBUGRISCV64_VIRT.fd $(AEVOS_UEFI_FW)/RELEASERISCV64_VIRT.fd /usr/share/OVMF/OVMF_CODE_RISCV64.fd /usr/share/qemu/firmware/edk2-riscv64-code.fd,$(if $(wildcard $(f)),$(f),)))
  OVMF_VARS_SRC := $(firstword $(foreach f,/usr/share/qemu-efi-riscv64/RISCV_VIRT_VARS.fd /usr/share/OVMF/OVMF_VARS_RISCV64.fd /usr/share/qemu/firmware/edk2-riscv64-vars.fd,$(if $(wildcard $(f)),$(f),)))
else ifeq ($(ARCH),loongarch64)
  ifneq ($(wildcard $(AEVOS_LOONGARCH_FW)/QEMU_EFI.fd),)
    OVMF_CODE := $(AEVOS_LOONGARCH_FW)/QEMU_EFI.fd
    OVMF_VARS_SRC := $(AEVOS_LOONGARCH_FW)/QEMU_VARS.fd
  else
    OVMF_CODE := $(firstword $(foreach f,$(AEVOS_UEFI_FW)/DEBUGLOONGARCH64_QEMU_EFI.fd $(AEVOS_UEFI_FW)/RELEASELOONGARCH64_QEMU_EFI.fd /usr/share/OVMF/OVMF_CODE_LOONGARCH64.fd /usr/share/edk2/loongarch64/QEMU_EFI.fd,$(if $(wildcard $(f)),$(f),)))
    OVMF_VARS_SRC := $(firstword $(foreach f,$(AEVOS_UEFI_FW)/DEBUGLOONGARCH64_QEMU_VARS.fd $(AEVOS_UEFI_FW)/RELEASELOONGARCH64_QEMU_VARS.fd /usr/share/OVMF/OVMF_VARS_LOONGARCH64.fd /usr/share/edk2/loongarch64/QEMU_VARS.fd,$(if $(wildcard $(f)),$(f),)))
  endif
endif

OVMF_IS_COMBINED := $(if $(findstring OVMF.fd,$(notdir $(OVMF_CODE))),$(if $(findstring CODE,$(notdir $(OVMF_CODE))),,yes),)

ifneq ($(OVMF_IS_COMBINED),)
  QEMU_FIRMWARE := -bios $(OVMF_CODE)
else ifneq ($(OVMF_CODE),)
  ifneq ($(OVMF_VARS_SRC),)
    QEMU_FIRMWARE := -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
                     -drive if=pflash,format=raw,file=$(OVMF_VARS_WORK)
  else
    QEMU_FIRMWARE := -bios $(OVMF_CODE)
  endif
else
  QEMU_FIRMWARE :=
endif

# LoongArch virt uses -bios
ifeq ($(ARCH),loongarch64)
  ifneq ($(OVMF_CODE),)
    QEMU_FIRMWARE := -bios $(OVMF_CODE)
  endif
endif

# ---- Host tools ----
MKFS_AEVOS_SRC := $(TOOLS_DIR)/mkfs_aevos.c
MKFS_AEVOS     := build/mkfs_aevos

# ============================================================
#  Targets
# ============================================================

.PHONY: all clean dirs boot kernel image tools run info help devtools-ui fetch-uefi-firmware

all: dirs boot kernel image
	@echo ""
	@echo "  Build complete for ARCH=$(ARCH)"
	@echo "  Kernel:  $(KERNEL_ELF)"
	@echo "  Boot:    $(BOOT_EFI)"
	@echo "  Image:   $(DISK_IMAGE)"
	@echo ""

help:
	@echo "AevOS Build System — Multi-architecture UEFI kernel"
	@echo ""
	@echo "  make                  Build for x86_64 (default)"
	@echo "  make ARCH=aarch64     Build for ARM64"
	@echo "  make ARCH=riscv64     Build for RISC-V 64"
	@echo "  make ARCH=loongarch64 Build for LoongArch 64"
	@echo "  make run              Build and run in QEMU (current ARCH)"
	@echo "  LoongArch firmware:   AEVOS_LOONGARCH_FW (default: ~/Firmware/LoongArchVirtMachine)"
	@echo "  make clean            Remove all build artifacts"
	@echo "  make fetch-uefi-firmware  从 edk2-nightly 拉取当前 ARCH 的 QEMU UEFI 固件到 AEVOS_UEFI_FW"
	@echo "  make devtools-ui      Host TypeScript Cursor 工作台 (Vite, devtools/cursor-workbench)"
	@echo ""

# ---- Download UEFI images matching edk2-nightly filenames (DEBUG) ----
fetch-uefi-firmware:
	@echo "  FETCH UEFI [$(ARCH)] -> $(AEVOS_UEFI_FW)  ($(EDK2_NIGHTLY_BIN))"
	@mkdir -p $(AEVOS_UEFI_FW)
ifeq ($(ARCH),x86_64)
	@curl -fL --retry 3 -o $(AEVOS_UEFI_FW)/DEBUGX64_OVMF_CODE.fd $(EDK2_NIGHTLY_BIN)/DEBUGX64_OVMF_CODE.fd
	@curl -fL --retry 3 -o $(AEVOS_UEFI_FW)/DEBUGX64_OVMF_VARS.fd $(EDK2_NIGHTLY_BIN)/DEBUGX64_OVMF_VARS.fd
else ifeq ($(ARCH),aarch64)
	@curl -fL --retry 3 -o $(AEVOS_UEFI_FW)/DEBUGAARCH64_QEMU_EFI.fd $(EDK2_NIGHTLY_BIN)/DEBUGAARCH64_QEMU_EFI.fd
	@curl -fL --retry 3 -o $(AEVOS_UEFI_FW)/DEBUGAARCH64_QEMU_VARS.fd $(EDK2_NIGHTLY_BIN)/DEBUGAARCH64_QEMU_VARS.fd
else ifeq ($(ARCH),riscv64)
	@curl -fL --retry 3 -o $(AEVOS_UEFI_FW)/DEBUGRISCV64_VIRT.fd $(EDK2_NIGHTLY_BIN)/DEBUGRISCV64_VIRT.fd
else ifeq ($(ARCH),loongarch64)
	@curl -fL --retry 3 -o $(AEVOS_UEFI_FW)/DEBUGLOONGARCH64_QEMU_EFI.fd $(EDK2_NIGHTLY_BIN)/DEBUGLOONGARCH64_QEMU_EFI.fd
	@curl -fL --retry 3 -o $(AEVOS_UEFI_FW)/DEBUGLOONGARCH64_QEMU_VARS.fd $(EDK2_NIGHTLY_BIN)/DEBUGLOONGARCH64_QEMU_VARS.fd
else
	$(error fetch-uefi-firmware: unsupported ARCH=$(ARCH))
endif
	@echo "  Done."

# ---- Create build directory tree ----
dirs:
	@mkdir -p $(BOOT_BUILD)
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)
	@mkdir -p $(BUILD_DIR)/$(KERNEL_ARCH)
	@mkdir -p $(BUILD_DIR)/$(KERNEL_MM)
	@mkdir -p $(BUILD_DIR)/$(KERNEL_SCHED)
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DRV)
	@mkdir -p $(BUILD_DIR)/$(KERNEL_FS)
	@mkdir -p $(BUILD_DIR)/$(KERNEL_NET)
	@mkdir -p $(BUILD_DIR)/$(KERNEL_DIR)/ipc
	@mkdir -p $(BUILD_DIR)/$(LINUX_DIR)
	@mkdir -p $(BUILD_DIR)/$(AGENT_DIR)
	@mkdir -p $(BUILD_DIR)/$(LLM_DIR)
	@mkdir -p $(BUILD_DIR)/$(UI_DIR)
	@mkdir -p $(BUILD_DIR)/$(POSIX_DIR)
	@mkdir -p $(BUILD_DIR)/$(LIB_DIR)
	@mkdir -p $(BUILD_DIR)/$(DB_DIR)
	@mkdir -p $(BUILD_DIR)/$(CONTAINER_DIR)
	@mkdir -p $(BUILD_DIR)/$(EVOLUTION_DIR)
	@mkdir -p $(BUILD_DIR)/$(TOOLS_DIR)

# ============================================================
#  Boot (UEFI EFI application)
# ============================================================

boot: dirs $(BOOT_EFI)

$(BOOT_BUILD)/%.o: $(BOOT_DIR)/%.c
	@echo "  CC  [boot/$(ARCH)]  $<"
	@$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BOOT_BUILD)/reloc_dummy.o: $(BOOT_DIR)/reloc_dummy.S
	@echo "  AS  [boot/$(ARCH)]  $<"
	@$(CC) -c $< -o $@

$(BOOT_SO): $(BOOT_OBJS)
	@echo "  LD  [boot/$(ARCH)]  $@"
	@$(LD) $(BOOT_GNUEFI_CRT0) $(BOOT_OBJS) $(BOOT_LDFLAGS) -o $@

$(BOOT_EFI): $(BOOT_SO)
	@echo "  EFI [boot/$(ARCH)]  $@"
ifeq ($(ARCH),x86_64)
	@$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata \
	            -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc \
	            --target=$(EFI_TARGET_FMT) -S $< $@
else
	@$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata \
	            -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc \
	            -j .got -j .got.plt \
	            --target=$(EFI_TARGET_FMT) --subsystem=efi-app -S $< $@
	@python3 $(SRCDIR)/tools/fix_pe_reloc.py $@
endif

# ============================================================
#  Kernel
# ============================================================

kernel: dirs $(KERNEL_ELF)

$(BUILD_DIR)/%.o: %.c
	@echo "  CC  [kern/$(ARCH)]  $<"
	@mkdir -p $(dir $@)
	@$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD_DIR)/%.S.o: %.S
	@echo "  AS  [kern/$(ARCH)]  $<"
	@$(CC) $(KCFLAGS) -c $< -o $@

ifeq ($(ARCH),x86_64)
$(BUILD_DIR)/$(LLM_DIR)/simd_kernels.o: $(LLM_DIR)/simd_kernels.c
	@echo "  CC  [simd/$(ARCH)]  $<"
	@$(CC) $(KCFLAGS) -msse2 -mavx2 -mfma -c $< -o $@ 2>/dev/null || \
	 $(CC) $(KCFLAGS) -c $< -o $@

$(BUILD_DIR)/$(LLM_DIR)/quantize.o: $(LLM_DIR)/quantize.c
	@echo "  CC  [simd/$(ARCH)]  $<"
	@$(CC) $(KCFLAGS) -msse2 -mavx2 -mfma -c $< -o $@ 2>/dev/null || \
	 $(CC) $(KCFLAGS) -c $< -o $@
endif

$(KERNEL_ELF): $(KERNEL_OBJS)
	@echo "  LD  [kern/$(ARCH)]  $@"
	@$(LD) $(KLDFLAGS) $(KERNEL_OBJS) $(LIBGCC) -o $@
	@echo "  Kernel: $@ ($$(stat -c%s $@ 2>/dev/null || stat -f%z $@) bytes)"

# ============================================================
#  Host: TypeScript IDE shell (Wayland-style protocol demo)
# ============================================================

devtools-ui:
	@echo "  Starting cursor-workbench (npm install if needed)…"
	@cd devtools/cursor-workbench && npm install && npm run dev

# ============================================================
#  Host tools
# ============================================================

tools: $(MKFS_AEVOS)

$(MKFS_AEVOS): $(MKFS_AEVOS_SRC) | dirs
	@mkdir -p $(dir $@)
	@echo "  HOSTCC     $@"
	@gcc -O2 -Wall -o $@ $<

# ============================================================
#  Disk image
# ============================================================

image: boot kernel
	@echo "  IMAGE [$(ARCH)]  $(DISK_IMAGE)"
	@dd if=/dev/zero of=$(DISK_IMAGE) bs=1M count=64 2>/dev/null
	@mkfs.fat -F 32 $(DISK_IMAGE) 2>/dev/null || true
	@mmd -i $(DISK_IMAGE) ::EFI 2>/dev/null || true
	@mmd -i $(DISK_IMAGE) ::EFI/BOOT 2>/dev/null || true
	@mmd -i $(DISK_IMAGE) ::EFI/AevOS 2>/dev/null || true
	@mcopy -i $(DISK_IMAGE) $(BOOT_EFI) ::EFI/BOOT/$(EFI_BOOT_NAME) 2>/dev/null || true
	@mcopy -i $(DISK_IMAGE) $(KERNEL_ELF) ::EFI/AevOS/kernel.elf 2>/dev/null || true
	@printf 'FS0:\\EFI\\BOOT\\$(EFI_BOOT_NAME)\r\n' > $(BUILD_DIR)/startup.nsh
	@mcopy -i $(DISK_IMAGE) $(BUILD_DIR)/startup.nsh ::startup.nsh 2>/dev/null || true
	@echo "  Done: $(DISK_IMAGE)"

# ============================================================
#  Run in QEMU
# ============================================================

run: image
	@echo "  QEMU [$(ARCH)]  $(DISK_IMAGE)"
	@test -n "$(OVMF_CODE)" || (echo "  ERROR: UEFI firmware not found for $(ARCH). Install ovmf." && false)
	@if [ -n "$(OVMF_VARS_SRC)" ]; then cp -f "$(OVMF_VARS_SRC)" "$(OVMF_VARS_WORK)"; fi
	$(QEMU_CMD) \
	    $(QEMU_EXTRA) \
	    $(QEMU_FIRMWARE) \
	    $(QEMU_DISK_ARGS) \
	    -m 4G \
	    -serial stdio \
	    -netdev user,id=net0 \
	    -device virtio-net-pci,netdev=net0,disable-legacy=on \
	    $(QEMU_RUN_DEVS)

# ============================================================
#  Clean
# ============================================================

clean:
	@echo "  CLEAN"
	@rm -rf build

# ============================================================
#  Info / Debug
# ============================================================

info:
	@echo "Architecture:   $(ARCH)"
	@echo "AEVOS_EMBED_LLM:$(AEVOS_EMBED_LLM)  (0=userspace LLM IPC path)"
	@echo "Toolchain:      $(CC)"
	@echo "Source dir:     $(SRCDIR)"
	@echo "Kernel LDS:     $(KERNEL_LDS)"
	@echo "AEVOS_UEFI_FW:  $(AEVOS_UEFI_FW)  (see https://retrage.github.io/edk2-nightly/)"
	@echo "AEVOS_LOONGARCH_FW: $(AEVOS_LOONGARCH_FW)"
	@echo "OVMF_CODE:      $(OVMF_CODE)"
	@echo "OVMF_VARS_SRC:  $(OVMF_VARS_SRC)"
	@echo "QEMU_FIRMWARE:  $(QEMU_FIRMWARE)"
	@echo "Boot sources:   $(BOOT_CSRC)"
	@echo "Boot objects:   $(BOOT_OBJS)"
	@echo "Kernel sources: $(KERNEL_CSRC)"
	@echo "Kernel ASM:     $(KERNEL_ASRC)"
	@echo "Kernel objects: $(KERNEL_OBJS)"
