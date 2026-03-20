# AevOS 文档（简体中文）

**AevOS**（Autonomous Evolving OS，自主演进操作系统）是一款**裸机**操作系统，在**不依赖 Linux 或其他宿主系统**的情况下原生运行 AI 智能体。系统通过 UEFI 启动，自行管理内存与调度，内嵌类 llama.cpp 的 LLM 推理运行时，并提供类似 Cursor 的**帧缓冲 Shell 界面**。

---

## 项目特点

| 方面 | 说明 |
|------|------|
| **启动** | UEFI 引导（`aevos_boot.efi`），读取 `boot.json`，加载 `kernel.elf`，初始化 GOP 帧缓冲后进入内核 |
| **内核** | C17 微内核：物理/虚拟内存、四级页表、协程式调度、NVMe/GPU 帧缓冲/HID 等驱动、简易 VFS、网络栈 |
| **智能体** | 每智能体对话历史（环形缓冲）、向量记忆（HNSW 语义召回）、技能引擎（运行时生成/编译/热加载 C 代码） |
| **大模型** | GGUF 模型、Q4/Q8 量化、AVX2 SIMD、流式输出 token |
| **界面** | 深色主题帧缓冲 UI：侧栏文件浏览、AI 对话（流式）、内置终端、鼠标指针、状态栏 |

设计目标之一：从上电到可交互 Shell **约 2 秒内**（与根目录 README 描述一致）。

---

## 架构分层

1. **第 0 层** — UEFI 引导加载器  
2. **第 1 层** — 微内核（内存、调度、驱动、VFS、网络）  
3. **第 2 层** — LLM 运行时（GGUF 推理，供智能体系统调用）  
4. **第 3 层** — AI 智能体核心（历史记录按时间排序、记忆、技能，使用 SQLite3 数据库）  
5. **第 4 层** — Shell 用户界面（帧缓冲，鼠标移动/左右键点击，键盘驱动）

ASCII 示意图见仓库根目录 [README.md](../../README.md)。

---

## 环境依赖

- **交叉 GCC**：如 `x86_64-elf-gcc`（Makefile 亦支持回退到宿主 `gcc`）
- **GNU-EFI**：构建 UEFI 引导所需头文件与库
- **mtools**：生成 FAT32 磁盘镜像
- **QEMU 与固件**：x86_64 上常用 OVMF 做 UEFI 客户机

**Ubuntu / Debian 示例：**

```bash
sudo apt install gcc gnu-efi mtools qemu-system-x86 ovmf
```

**龙芯 LoongArch 64**：需龙架构交叉工具链；QEMU 使用 EDK2 pflash，路径可通过 `AEVOS_LOONGARCH_FW` 覆盖（见 Makefile）。

---

## 编译构建

在仓库根目录执行：

```bash
# 完整构建：引导程序 + 内核 + 磁盘镜像
make

# 指定架构
make ARCH=aarch64
make ARCH=riscv64
make ARCH=loongarch64

# 单独目标
make boot      # 仅 UEFI 引导
make kernel    # 仅内核
make tools     # 宿主端工具（如 mkfs、skill_packager）
make image     # 磁盘镜像

make info      # 打印当前构建配置
```

支持的 `ARCH`：`x86_64`、`aarch64`、`riscv64`、`loongarch64`。

---

## 运行（QEMU）

```bash
make run
```

使用当前 `ARCH`（默认 `x86_64`），x86_64 下配合 OVMF 等 UEFI 固件，**4GB 内存**，使用生成的磁盘镜像，**串口输出接到标准输入输出**。

**龙架构固件路径示例：**

```bash
AEVOS_LOONGARCH_FW=/path/to/LoongArchVirtMachine make ARCH=loongarch64 run
```

---

## 启动配置（`boot.json`）

将 `boot.json` 放在 EFI 系统分区路径 `\EFI\AevOS\boot.json`。

示例：

```json
{
  "model_path": "/models/qwen-7b-q4.gguf",
  "n_ctx": 32768,
  "n_threads": 4,
  "use_gpu": false,
  "target_fps": 60,
  "screen_width": 1920,
  "screen_height": 1080
}
```

若文件不存在，将使用合理默认值。

---

## 目录结构（摘要）

| 路径 | 作用 |
|------|------|
| `src/boot/` | UEFI 引导 |
| `src/kernel/` | 内核与各架构、内存、调度、驱动、文件系统、网络 |
| `src/agent/` | 智能体核心 |
| `src/llm/` | LLM 运行时 |
| `src/ui/` | 帧缓冲 Shell |
| `src/lib/` | 独立环境 libc 子集 |
| `src/tools/` | 宿主端工具 |
| `src/include/aevos/` | 公共头文件（`config.h`、`boot_info.h` 等） |

---

## 版本与许可

- 当前基线版本：**v0.1.0**（路线图见根目录 README 版本表）。
- 许可证：**LGPL-2.1**。

---

*English: [Documentation in English](../en/README.md).*
