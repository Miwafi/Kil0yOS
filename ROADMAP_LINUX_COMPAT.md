# Linux 程序兼容路线图

> 目标：让 Kil0yOS 能够运行 Linux 用户态程序，最终接入 Debian/APT（.deb）软件生态。
> 技术基线：x86-64 Long Mode / Multiboot2+GRUB / ring3 进程 / 自研 fs（内存态）/ UDP 网络栈。

## 总体路线

```
Phase 0        Phase 1         Phase 2          Phase 3            Phase 4
ELF64 loader → musl 静态程序 → busybox 跑通  →  glibc 动态链接  →  dpkg/APT（.deb 生态）
+最小 syscall   +tar 包分发      +磁盘文件系统     +TCP/DNS 栈        +Debian 软件仓库
```

## Phase 0 — ELF64 加载与最小 syscall（起点）

| # | 任务 | 内容 | 依赖 | 验收标准 |
|---|------|------|------|----------|
| 0.1 | ELF64 loader | 解析 ELF header / Program Headers，按 `PT_LOAD` 映射段、清零 `p_memsz>p_filesz`（BSS）、设置入口点；并存保留 raw binary 路径 | VMM 页表 | WSL 编译的 `musl-gcc -static hello` 成功输出 |
| 0.2 | mmap/munmap | 用户地址空间分配器（VMM 上层），匿名页映射 | 0.1 | musl malloc 内部 brk/mmap 不返回错误 |
| 0.3 | mprotect | 页权限修改（musl 栈保护/Guard page 使用） | 0.2 | 读写测试页属性生效 |
| 0.4 | brk | 传统堆接口（musl malloc 首选） | 0.2 | 连续分配 1MB 无失败 |
| 0.5 | 辅助 syscall | `getpid/exit_group/getuid(stub)/clock_gettime/getrandom(软随机)/arch_prctl(SET_FS)/set_tid_address/rt_sigaction(stub)/ioctl(TTY 默认值)` | 0.1 | musl 运行时初始化全程无 ENOSYS |
| 0.6 | exec 路径重构 | `execve` 语义（当前 exec 直接替换内核 shell 上下文的部分逻辑迁移到用户进程模型） | 0.1 | exec ELF 后返回 shell 不崩溃 |

## Phase 1 — busybox 里程碑（Phase 1 完成 = "能跑 Linux 程序"）

| # | 任务 | 内容 | 依赖 | 验收标准 |
|---|------|------|------|----------|
| 1.1 | argv/environ 传递 | 内核栈布局：`argc/argv[]/envp[]/auxv`（AT_PAGESZ 等最小 auxv） | 0.6 | busybox 能读到自身名称 |
| 1.2 | TTY 字符设备 | stdin/stdout/stderr 指向键盘+VGA 的行规程（含 `\n`→`\r\n`、退格） | 0.5 | `echo hello` 交互正常 |
| 1.3 | busybox 移植 | WSL 中 `musl-gcc -static` 全功能或裁剪配置编译；tar 包格式：`.tar.gz` + `manifest`（包名/版本/文件列表） | 0.x 全部 | `ls/cp/cat/echo/mkdir` 可用 |
| 1.4 | 简单下载器 | 基于 UDP 的 TFTP 客户端（或 raw HTTP over 简化 TCP）从主机拉取包 | 1.3, 网络栈 | 内核内拉取 busybox 并安装到 /bin |
| 1.5 | fork/vfork 语义 | busybox `sh` 依赖进程复制；可先实现轻量 vfork+exec | 进程模型 | busybox sh 能执行外部命令 |

## Phase 2 — 持久化文件系统

| # | 任务 | 内容 | 依赖 | 验收标准 |
|---|------|------|------|----------|
| 2.1 | ext2 只读驱动 | 磁盘文件系统（ATA PIO 即可，VMware/QEMU 均好模拟）；或 squashfs（更简单、只读） | 1.3 | 重启后 /bin 内容仍在 |
| 2.2 | 读写支持 | ext2 rw（bitmaps/inode 分配）或 overlay：内存层写、磁盘层读 | 2.1 | dpkg 可安装文件 |
| 2.3 | VFS 统一 | 内存 fs 与磁盘 fs 挂载树统一（当前 fs_resolve_path 改为可挂载） | 2.1 | `/` 磁盘 + `/tmp` 内存 |

## Phase 3 — glibc 动态链接 + TCP/DNS

> .deb 生态以 glibc 为核心，Phase 3 从 musl 动态链接升级为 **glibc 动态链接**。
> glibc 的加载器和运行时要求显著更高（IFUNC、TLS 初始化镜像、更宽的 syscall 面），
> musl 动态链接可作为 3.1 的中间里程碑先行验证加载器框架。

| # | 任务 | 内容 | 依赖 | 验收标准 |
|---|------|------|------|----------|
| 3.0 | musl 动态链接（中间里程碑） | `.interp`=/lib/ld-musl-x86_64.so.1、`DT_NEEDED` 递归、GOT/PLT 重定位；用 musl 验证动态加载器框架 | 1.x | 动态 hello 跑通 |
| 3.1 | glibc 动态 ELF 加载 | `.interp`=/lib/ld-linux-x86_64.so.2；IFUNC resolver（需要 AT_HWCAP auxv）、TLS_INIT/TCB（`arch_prctl ARCH_SET_FS`）、`DT_DEBUG` | 3.0 | Debian 的动态 hello（glibc）跑通 |
| 3.2 | futex + rseq stub | glibc 线程/锁基础；`set_robust_list`/`rseq` 可先 stub 返回成功 | 3.1 | 动态链接 pthread 程序不死锁 |
| 3.3 | TCP 栈 | 三次握手、滑窗、重传（最小实现即可） | 网络栈 | 与 Linux 主机 3 次握手成功 |
| 3.4 | DNS 解析器 | UDP 53，glibc resolver 走 `/etc/resolv.conf`（NSS 可先绕过：直接读文件配置） | 3.3 | busybox `nslookup` 成功 |
| 3.5 | busybox wget | 基于 3.3/3.4 | 3.4 | `wget` 拉取 HTTP 文件 |

## Phase 4 — dpkg/APT 包管理器（接入 Debian 生态）

> .deb 格式：`ar` 归档（`debian-binary` + `control.tar.*` + `data.tar.*`），
> 成员压缩为 gzip/xz/zstd。APT 生态依赖 glibc 动态链接，Phase 3.1 是硬前提。

| # | 任务 | 内容 | 依赖 | 验收标准 |
|---|------|------|------|----------|
| 4.1 | .deb 解包 | `ar` 归档解析 + tar 解包；压缩算法按需增量实现（先 gzip，xz/zstd 可让仓库 re-pack 规避） | 2.2 | 解开一个本地 .deb 并落地 data.tar 内容 |
| 4.2 | dpkg 移植 | dpkg 本体（C，依赖少）或最小自研 dpkg 前端：`/var/lib/dpkg/status` 数据库 + 安装/卸载/依赖检查 | 4.1 | `dpkg -i` 本地包成功，记录归属数据库 |
| 4.3 | 基础引导集 | debootstrap 思路：手动按序安装 `libc6 → 核心运行库 → dpkg 自身`，建立 glibc 用户态底盘 | 3.1, 4.2 | Debian rootfs 子集可启动 busybox/基础命令 |
| 4.4 | 仓库客户端 | 解析 `Packages` 索引（文件名/依赖/SHA256），wget 下载 .deb 后走 4.2 安装；`sources.list` 配置 | 3.5, 4.2 | `apt-get update` 等价功能（可用自研 `kilget` 前端） |
| 4.5 | apt 完整接入 | apt 本体是 C++（依赖 zlib/bz2/lzma/ssl + libstdc++），两条路：移植 apt，或自研轻量前端长期替代；依赖求解先做拓扑排序（apt 的求解器暂不移植） | 4.4 | `apt-get install` 任一 CLI 软件并运行 |

## 依赖关系与关键路径

```
0.1 → 0.2 → 0.3/0.4 → 0.5 → 0.6 → 1.1 → 1.2 → 1.3 → 1.5 → 3.0 → 3.1 → 3.2 → 4.1 → 4.2 → 4.3
                                        └→ 1.4             └→ 3.3 → 3.4 → 3.5 → 4.4 ↗
2.1 → 2.2 → 2.3 ──────────────────────────────────→ 4.1（.deb 落地目标）
```

关键路径：**0.1 ELF loader → 0.5 syscall 补齐 → 1.3 busybox → 3.1 glibc 动态 → 4.2 dpkg → 4.5 apt install**。

## 风险与决策点

| 风险 | 影响 | 缓解 |
|------|------|------|
| musl 静态程序初始化调用的 syscall 面 > 预期 | Phase 0 返工 | 先用 strace 在 WSL 记录 musl hello/busybox 的 syscall 清单再实现 |
| glibc 的 syscall 面宽（`set_robust_list`/`rseq`/`getrandom`/vDSO 等） | Phase 3 工作量增加 | strace 记录 Debian 二进制的真实清单；缺的先 stub；vDSO 可置 `AT_SYSINFO_EHDR=0` 走纯 syscall |
| glibc IFUNC/TLS 初始化复杂 | 3.1 是全路线最难单点 | 先 musl 动态（3.0）验证加载器框架，再攻 glibc；参考 dragonflybsd/osv 等小型内核实现 |
| apt 本体是 C++ 且依赖重 | Phase 4 尾段受阻 | 先 dpkg + wget + 自研索引解析（4.4 可交付价值）；apt 移植放最后甚至长期用自研前端 |
| .deb 成员压缩 xz/zstd | 解包卡壳 | 自建仓库镜像统一 re-pack 为 gzip；zstd/xz 后置 |
| fork 成本（无 COW） | busybox sh 卡顿 | 先 vfork+exec；COW 在 Phase 2+ 评估 |
| TCP 栈工作量失控 | Phase 3 延期 | 只做主动打开/关闭、无拥塞控制的简单实现（内网/本机测试足够） |
| VMware/QEMU 磁盘差异 | 2.1 调试困难 | ATA PIO 模式两者行为一致，优先于 AHCI |

## 里程碑定义

- **M0**：musl 静态 hello world 在内核中运行（Phase 0 完）
- **M1**：busybox sh 交互可用（Phase 1 完）——*"这是一个能跑 Linux 程序的 OS"*
- **M2**：重启后软件包仍存在（Phase 2 完）
- **M3**：glibc 动态 hello 跑通（Phase 3 前段完）——*".deb 生态的门槛已跨过"*
- **M4**：`dpkg -i` 安装 Debian 软件并运行（Phase 4 中段完）
- **M5**：仓库客户端 `apt-get install` 等价流程装上任意 CLI 软件——*"接入了 Debian 软件生态"*
