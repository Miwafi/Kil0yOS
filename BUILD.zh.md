# Kil0yOS 编译指南

本文档说明项目的全部构建目标、编译选项、产物位置与用途。

## 1. 环境要求

编译必须在 **WSL** 中执行（Windows 侧只存放源码）：

| 工具 | 用途 | 缺失时安装（Ubuntu/WSL） |
|------|------|--------------------------|
| `gcc` (x86_64) | 内核与用户程序编译 | `sudo apt install build-essential` |
| `nasm` | boot.asm / isr.asm 汇编 | `sudo apt install nasm` |
| `ld` (binutils) | 内核链接（linker.ld 脚本） | 随 build-essential |
| `grub-mkrescue` | 生成 ISO | `sudo apt install grub-pc-bin xorriso mtools` |
| `mtools` (`mformat`/`mcopy`) | U 盘镜像 FAT16 操作 | `sudo apt install mtools` |
| `qemu-system-x86_64` | 本地运行测试 | `sudo apt install qemu-system-x86` |
| `musl-gcc`（可选） | Linux-ABI 静态用户程序 | 手动构建 musl（默认 `~/musl/bin/musl-gcc`） |

## 2. 快速开始

```bash
# WSL 中，项目根目录
make            # 等价于 make all → make iso，产出 build/kil0yos.iso
make run        # QEMU 启动该 ISO（rtl8139 网卡 + 串口输出到终端）
make usb        # 产 出 build/kil0yos-usb.img，用于真机 U 盘（BIOS 启动）
```

默认目标 `all` 即 `iso`。VMware/QEMU/刻光盘用 ISO；**真机 U 盘必须用 `make usb` 的镜像**（原因见第 5 节）。

## 3. 构建目标详解

| 目标 | 命令 | 作用 | 主要产物 |
|------|------|------|----------|
| `all`（默认） | `make` | 同 `iso` | `build/kil0yos.iso` |
| `iso` | `make iso` | 链接内核 + 组装 ISO 目录树 + `grub-mkrescue` 打包混合引导 ISO（已加 `--mbr-force-bootable`） | `build/kil0yos.iso`（约 17MB） |
| `usb` | `make usb` | 链接内核 + `tools/make_usb.sh` 手工组装真机 U 盘镜像（MBR + 30MB FAT16 + GRUB） | `build/kil0yos-usb.img`（31MB） |
| `run` | `make run` | 依赖 ISO，用 QEMU 启动 | 无（运行时行为） |
| `disk` | `make disk` | 生成 2MB 空硬盘镜像（供内核内 ext2 格式化测试） | `disk.img` |
| `clean` | `make clean` | 删除整个 `build/` | 无 |

`make iso` 与 `make usb` 都以 `build/kernel.bin` 为依赖：内核源码变动后两者都会自动重链，但 **ISO 目录树/U 盘镜像内容会重新打包**（usb 目标声明为 `.PHONY`，每次都执行）。

## 4. 产物一览

```
build/
├── kernel.bin            ← 内核 ELF（boot.o + 全部内核.o + 用户程序 blob 链接而成，约 5.6MB）
│                           GRUB 通过 multiboot2 直接加载此文件
├── kil0yos.iso           ← 混合引导 ISO（VMware / QEMU / 光盘）
├── kil0yos-usb.img       ← 真机 U 盘镜像（Rufus DD 模式写入）
├── kernel/.../*.o        ← 各内核源文件目标文件（含 -MMD 生成的 .d 依赖）
├── boot/boot.o           ← 32 位引导存根（进入 long mode + multiboot2 头）
├── kernel/core/isr_asm.o ← 中断入口汇编
├── kernel/core/ap_trampoline.bin/o ← SMP 应用核启动跳板（objcopy 转 ELF 嵌入）
├── grub-core.img         ← make_usb 过程中的中间产物（构建后自动删除）
└── user/
    ├── hello.o / pong.o  ← 自定义 ABI 用户程序（freestanding 编译）
    ├── hello.bin 等      ← user.ld 链接后的用户程序
    └── hello-lnx, mini, mmt, nettest, busybox, hello-dyn, hello-glibc,
        hello-pthread, probe-ld   ← Linux-ABI ELF（条件构建，见第 6 节）
```

### 4.1 关键中间产物说明

| 产物 | 生成方式 | 说明 |
|------|----------|------|
| `build/kernel.bin` | `ld -T linker.ld` 链接 | 内核本体。加载地址、段布局由 `linker.ld` 决定 |
| `build/user_blob_*.o` | `printf incbin` + `nasm` | 把用户程序二进制作为 `.rodata` 段嵌进内核，符号 `user_<名>_start/_end` 标记边界；内核启动时安装到 `/bin` |
| `build/iso/boot/kil0yos.bin` | `cp kernel.bin` | ISO 内的内核副本（grub.cfg 里 `multiboot2 /boot/kil0yos.bin`） |
| `build/iso/boot/grub/grub.cfg` | `cp grub.cfg` | ISO 菜单（timeout=2） |
| `kil0yos-usb.img` 内部 | mtools 写入 | MBR=GRUB boot.img（分区表手工 patch，4 槽清零后写槽 1）；LBA1=core.img（early config 直接引导内核）；LBA2048=FAT16（`/boot/kil0yos.bin`、`/boot/grub/grub.cfg`、`/boot/grub/i386-pc/*.mod`） |

## 5. 启动介质选择

| 环境 | 用哪个 | 写入方式 |
|------|--------|----------|
| VMware / VirtualBox / QEMU | `build/kil0yos.iso` | 直接挂载/`-cdrom` |
| 光盘（真机 BIOS） | `build/kil0yos.iso` | 刻录软件选择"刻录镜像" |
| **U 盘（真机 BIOS）** | `build/kil0yos-usb.img` | Rufus **DD 模式** / Etcher / `dd if=... of=/dev/sdX bs=4M` |
| U 盘（错误方式） | — | 资源管理器复制文件、UltraISO 重新造盘都会破坏自建 MBR |

**为什么真机 U 盘不用 ISO**：grub-mkrescue 的混合 ISO 是 ISO9660-over-USB，真机 BIOS 对这条路径几乎没有测试（int 13h 几何乱报会让 GRUB 探测文件系统时崩溃）；而 MBR+FAT16 是所有 Linux LiveUSB 的标准路径。VM 里 ISO 启动正常不能代表真机可用。

## 6. 条件构建（用户程序自动检测）

`make` 时脚本探测宿主环境，存在则构建并嵌入内核：

| 程序 | 依赖 | 说明 |
|------|------|------|
| `hello` / `pong` | 无（gcc 自身即可） | 自定义 ABI，freestanding，用户态syscall进入内核 |
| `mini` / `mmt` / `nettest` | gcc | Linux-ABI freestanding 探针（syscall / brk-mmap / TCP） |
| `hello-lnx` / `probe-ld` | `musl-gcc` | musl 静态 ELF |
| `hello-dyn` + `ld-musl blob` | `musl-gcc` + `~/musl/lib/libc.so` | musl 动态 PIE，ldso 随内核部署 |
| `hello-glibc` + `ld-linux/libc blob` | gcc + WSL 内 glibc | glibc 动态 PIE（Phase 3.1） |
| `hello-pthread` | gcc + libpthread | pthread 原语验收 |
| `busybox` | `~/busybox-1.36.1/busybox` | musl 静态多调用二进制，装到 `/bin/busybox` |

缺失依赖只跳过对应程序，不影响其余构建。构建日志中的 `[user] /bin/xxx installed` 行可确认本次嵌入情况。

## 7. 运行与调试

### 7.1 make run 参数解析

```
qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M \
    -display none -serial stdio \
    -netdev user,id=net0 -device rtl8139,netdev=net0
```

| 参数 | 说明 |
|------|------|
| `-m 512M` | 内存 512MB |
| `-display none` | 不开窗口，只看串口 |
| `-serial stdio` | 内核串口日志直通终端（Ctrl+C 退出） |
| `-netdev user -device rtl8139` | slirp 用户态网络 + RTL8139 网卡（DHCP 可获取 10.0.2.15） |

### 7.2 测真机镜像（U 盘启动路径）

```bash
# 把 U 盘镜像当硬盘启动，等价真机 MBR→GRUB→内核路径
qemu-system-x86_64 -drive format=raw,file=build/kil0yos-usb.img \
    -boot c -m 512M -display none -serial stdio
```

### 7.3 常见问题

- **`-monitor unix:...` 报 `Failed to bind socket`**：WSL2 的 unix socket 不能建在 `/mnt/c`（Windows 挂载盘）上。把 socket 路径放到 WSL 原生目录（如 `/tmp/qmon`），或直接用 `-monitor stdio`。
- **`make: Nothing to be done for 'iso'`**：改了 Makefile 配方但依赖未变时 make 不会重跑，`rm -f build/kil0yos.iso` 后再 `make iso`（usb 目标已声明 `.PHONY`，无此问题）。
- **QEMU 里看不到 GRUB 输出**：GRUB 默认只在 VGA 显示；U 盘镜像的 GRUB 已镜像到串口。纯 VGA 场景可用 monitor 命令 `screendump` 截图分析。
- **时钟偏斜警告**（`Clock skew detected`）：Windows/WSL 文件时间戳差异导致，可 `make clean` 后重建。

## 8. 完整流程示例

```bash
# 开发循环（VMware/QEMU）
make && make run

# 真机验证
make usb
# Windows 侧用 Rufus（DD 模式）把 build/kil0yos-usb.img 写入 U 盘
# BIOS 从 U 盘启动，无人值守直接进内核（GRUB early config 引导）

# 改内核后重新出 U 盘镜像
make usb   # 自动重链 kernel.bin 并重建镜像
```
