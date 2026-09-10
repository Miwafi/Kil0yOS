<div align="center">
  <img src="assets/banner.svg" alt="kil0yOSnotCtOS" width="100%" />
  <h1>kil0yOS</h1>
  <p><strong>一个 64 位 x86-64 操作系统</strong></p>

  <p>
    <a href="#">
      <img src="https://img.shields.io/github/stars/Miwafi/Kil0yOS?style=social" alt="GitHub stars" />
    </a>
    <a href="https://github.com/Miwafi/Kil0yOS/issues">
      <img src="https://img.shields.io/github/issues/Miwafi/Kil0yOS?style=social" alt="GitHub issues" />
    </a>
    <a href="https://github.com/Miwafi/Kil0yOS">
      <img src="https://img.shields.io/github/repo-size/Miwafi/Kil0yOS?style=social" alt="GitHub repo size" />
    </a>
  </p>

  <p>
    <a href="README.md"><strong>English</strong></a> |
    <a href="README.zh.md"><strong>简体中文</strong></a>
  </p>
</div>

## 这是什么

用 C 和汇编从写的 x86-64 操作系统。GRUB2 引导，BIOS 和 UEFI 都能启动，附赠一个Linux ELF程序兼容层（

## 功能

内核：

- x86-64 长模式，4 级分页（自动探测 5 级），每进程独立地址空间
- 位图 PMM、按需 VMM，堆带哨兵校验
- 时间片轮转调度，Ring 3 用户程序走自己的系统调用
- PS/2 键盘鼠标、PIT/RTC、ACPI 关机
- BIOS 下 VGA 文本控制台，UEFI 下 GOP 帧缓冲控制台（1024x768x32）

Linux 兼容层：

- 能跑 Linux x86-64 ELF 程序：静态、动态 PIE、`PT_INTERP` 解释器加载支持
- busybox 1.36.1（musl 静态，约 390 个 applet）、musl 动态 PIE、glibc 动态 PIE 能跑
- 约 60 个 Linux 系统调用：`openat`、`statx`、`getdents64`、`mmap`、`brk`、`readv`/`writev`、`poll`、socket 系列
- `fork`/`vfork`/`clone`/`wait4`/`execve`，带 TTY 行规程

网络：

- E1000 和 RTL8139 网卡驱动
- Ethernet、ARP、IPv4、ICMP、UDP、TCP（滑动窗口、重传、流控）
- DHCP、TFTP、DNS、HTTP/1.1（busybox `wget` 可用）

GUI：

- 平铺桌面（模式 13h），带 shell、系统面板和一只猫
- 乒乓球游戏作为 Ring 3 程序跑在图形系统调用上

## 构建和运行

需要 gcc、nasm、ld、grub-mkrescue、qemu-system-x86_64。Linux-ABI 测试程序（busybox、musl/glibc 构建）在 WSL/Debian 宿主上编译，脚本在 `tools/` 里。

```bash
make        # 产出 build/kil0yos.iso
make run    # 无头启动，串口控制台接到 stdio
qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M   # 带窗口
```

ISO 也能在 VMware/VirtualBox 里启动；要走 UEFI 路径，在虚拟机设置里启用 EFI 固件。

## Shell

基础文件和系统命令：`ls`、`cd`、`pwd`、`mkdir`、`rm`、`touch`、`cat`、`edit`、`clear`、`echo`（支持 `>` 重定向）、`whoami`、`date`、`time`、`version`、`help`、`shutdown`（ACPI S5）。

有意思的几个：

- `net`、`ping` — 网络信息（`net ifconfig`、`net netstat`）和 ICMP 回显
- `tftp` — 通过 TFTP 拉文件装到 `/bin`
- `dpkg` — `dpkg -i file.deb`、`-r pkg`、`-l`、`-L pkg`
- `kilget` — 仓库客户端，等价 apt-get：`update`、`install`、`show`、`list`、`installed`（`apt-get` 是别名）
- `exec` — 跑用户程序，比如 `exec /bin/pong.bin`

不认识的命令会落到 busybox（`/bin/busybox <cmd>`），所以 `find`、`grep`、`wget`、`nslookup`、`vi`、`ps` 这些都能用。

装包全流程：

```text
$ echo deb http://10.0.2.2:8000 . > /etc/kilget/sources.list
$ kilget update
$ kilget install libc6
kilget: downloading libc6_2.35-0ubuntu3_amd64.deb ...
 [kilget] installed libc6
$ exec /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
...真实 glibc 动态链接器的 usage banner...
```

## GUI

运行 `gui` 进入平铺桌面，方向键选左侧菜单，回车切换面板。

### 交互式 Shell

图形化 shell，支持 `ls`、`cd`、`mkdir`、`touch`、`pwd`、`shutdown` 等。

![Shell GUI](assets/shellgui.png)

### 猫

每个操作系统都得有一只。

![=^._.^=](assets/mew.png)

### 系统面板

![System GUI](assets/systemgui.png)

### 乒乓球

`exec /bin/pong.bin` 和 AI 对打，先拿 5 分的赢。W/S 移动挡板，ESC 回 shell。走 Ring 3 图形系统调用渲染，增量刷新不闪屏。

## 项目结构

```
src/boot/             引导入口（Multiboot2、长模式、UEFI、汇编）
src/kernel/core/      内核核心：进程、ELF 加载器、Linux 系统调用表、VFS 垫片、TTY
src/kernel/drivers/   设备驱动（键盘、鼠标、磁盘、PCI、VGA、电源……）
src/kernel/fs/        多后端 VFS：FAT 内存盘 + ext2 根 + 写覆盖层
src/kernel/mm/        PMM / VMM / 堆
src/kernel/net/       TCP/IP 协议栈、DHCP、TFTP、DNS、HTTP，E1000 + RTL8139 驱动
src/kernel/pkg/       Debian 包：dpkg、kilget、ar/tar/gzip/sha256
src/kernel/sched/     调度器
src/kernel/shell/     Shell 和终端
src/kernel/timer/     PIT
src/kernel/usb/       UHCI 主机控制器 + USB HID
user/                 Linux-ABI 测试程序
tools/                构建和验收工具（busybox 构建、QEMU 无头测试）
CHANGELOG.md          发布日志
ROADMAP_LINUX_COMPAT.md   Linux 兼容路线图（阶段 0-4）
```

## 许可证

GPL-2.0

## 后记

```
莫斯科的归雁降落自北方
```
```
织工的手串起记忆的海洋
```
```
待到春去夏来打孔带中就要流过黄河长江
```
