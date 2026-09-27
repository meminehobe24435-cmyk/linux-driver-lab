# linux-driver-lab —— Linux 字符设备驱动学习项目

一个**基于内存虚拟设备**的 Linux 字符设备驱动，用来完整展示内核驱动开发链路：
`misc_register` 注册设备 → `file_operations` 七大回调 → 环形缓冲区 + `mutex` + 等待队列
→ `ioctl` ABI → `sysfs` 属性 → 模块参数 → `kthread` 心跳 → 用户态功能测试与多进程并发压测。

> **⚠️ 能力边界（请先读完这段再看代码）**
>
> * 本项目的设备是**纯内存虚拟设备**，**没有任何真实硬件**，不涉及寄存器操作、
>   中断（IRQ）、DMA、设备树（Device Tree）、平台驱动（platform driver）、
>   probe/remove 匹配、电源管理或任何板级调试。
> * 本项目**没有**在真实开发板上验证过，也不包含 device tree 源文件。
> * 内核态的正确性由 GitHub Actions 在 `ubuntu-latest` 上**真实 `insmod` 后运行测试**来验证；
>   本地 WSL 只能完成编译验证（见下文的“本地验证到了什么程度”）。
> * 环形缓冲区的核心算法额外有一套**用户态**单元测试 + 参考模型随机模糊测试，
>   但这是用户态测试，**不能**替代内核态测试，两者不能互相冒充。

[![CI](https://github.com/meminehobe24435-cmyk/linux-driver-lab/actions/workflows/ci.yml/badge.svg)](https://github.com/meminehobe24435-cmyk/linux-driver-lab/actions/workflows/ci.yml)

---

## 目录

- [它做了什么](#它做了什么)
- [环境要求](#环境要求)
- [编译与运行](#编译与运行)
- [本地验证到了什么程度](#本地验证到了什么程度)
- [ioctl ABI 表](#ioctl-abi-表)
- [模块参数表](#模块参数表)
- [sysfs 属性](#sysfs-属性)
- [llseek 的语义](#llseek-的语义)
- [读写与丢弃语义](#读写与丢弃语义)
- [测试怎么跑](#测试怎么跑)
- [目录结构](#目录结构)
- [设计要点](#设计要点)

---

## 它做了什么

`/dev/labdev` 是一个"字符设备版管道"：

```
   用户 write()  ──copy_from_user──▶  kernel 环形缓冲区  ──copy_to_user──▶  用户 read()
                                           │
                          mutex 保护 + wait_queue 唤醒
                            ioctl / sysfs / kthread 观测
```

| 能力 | 实现位置 |
| --- | --- |
| 字符设备注册 | `misc_register()`（不是自己 `register_chrdev`，原因见 `docs/面试问答.md`） |
| `file_operations` | `open` / `release` / `read` / `write` / `llseek` / `unlocked_ioctl` / `poll`，共 7 个 |
| 并发控制 | `struct mutex` 保护环形缓冲区与全部统计字段；`wait_queue_head_t` 实现阻塞读 |
| 非阻塞 | `O_NONBLOCK` 下空读立即返回 `-EAGAIN`，绝不睡眠 |
| epoll 支持 | `poll()` 正确上报 `EPOLLIN\|EPOLLRDNORM`，并且永远上报 `EPOLLOUT` |
| ioctl | 4 条命令，`_IOR` / `_IOWR` / `_IO` 编码，结构体大小固定并带编译期断言 |
| sysfs | `buffer_size`(RO) / `debug`(RW) / `stats`(RO)，共 3 个 |
| 模块参数 | `buf_size` / `verbose` / `heartbeat_ms`，共 3 个 |
| 内核线程 | `kthread_run()` 周期性心跳；卸载时 `kthread_stop()` 安全停止 |
| 内存安全 | 所有用户指针走 `copy_from_user`/`copy_to_user`；错误路径逐级回滚 |

---

## 环境要求

| 项 | 要求 |
| --- | --- |
| Linux 内核 | **>= 4.15**（`struct miscdevice.groups` 从 4.15 开始有；`sysfs_emit` 从 5.10 开始，更早的内核由代码里的版本判断自动退回 `scnprintf`） |
| 内核头文件 | 必须与**正在运行的内核**版本一致：`linux-headers-$(uname -r)` |
| 编译工具 | `gcc`、`make`、`build-essential`（kbuild） |
| 权限 | `insmod`/`rmmod` 需要 root；设备默认 `mode=0600`，所以测试也要 root |
| 用户态测试 | 只要 C 库；`tools/stress.py` 需要 Python 3.6+ 且**只用标准库** |

在 Ubuntu / Debian 上：

```bash
sudo apt-get update
sudo apt-get install -y build-essential linux-headers-$(uname -r) python3
```

---

## 编译与运行

```bash
# 1. 用户态单元测试（不需要内核头文件，先跑这个最快）
make test-host

# 2. 编译内核模块，产物是 labdev.ko
make

# 3. 编译用户态功能测试
make -C tools

# 4. 加载 / 卸载（需要 root）
sudo insmod ./labdev.ko
lsmod | grep labdev
ls -l /dev/labdev

# 5. 真实内核态测试
sudo ./tools/dtest /dev/labdev
sudo python3 tools/stress.py --dev /dev/labdev --procs 4 --threads 2 --seconds 3

# 6. 卸载并确认没有残留
sudo rmmod labdev
test ! -e /dev/labdev && echo "设备节点已消失"
dmesg | tail -20
```

也可以带参数加载：

```bash
sudo insmod ./labdev.ko buf_size=16384 heartbeat_ms=250 verbose=1
```

或者直接用顶层 Makefile 的便捷目标（内部会 `sudo` 并帮你检查设备节点）：

```bash
make load     # insmod + 检查 /dev/labdev 出现
make unload   # rmmod + 检查 /dev/labdev 消失
make check    # test-host + 编译 dtest
```

---

## 本地验证到了什么程度

诚实说明本仓库最后一次改动时**本地环境（Windows + WSL2）实际做了什么**：

| 项目 | 本地结果 |
| --- | --- |
| `make test-host`（用户态环形缓冲区测试） | ✅ 全部通过 |
| `make`（用 `linux-headers-5.15.0-194-generic` 编译） | ✅ **0 warning / 0 error**，产出 `labdev.ko`（约 350 KB） |
| `insmod` | ❌ **失败，且这是预期结果**。本机运行的是 WSL2 定制内核 `6.18.33.2-microsoft-standard-WSL2`，而 Ubuntu 22.04 仓库里能装到的头文件是 `5.15.0-194-generic`，`vermagic` 不匹配。实际报错：<br>`module labdev: .gnu.linkonce.this_module section size must match the kernel's built struct module size at run time`<br>`insmod: ERROR: could not insert module ./labdev.ko: Invalid module format` |
| 内核态功能测试 / 并发压测 | 本地**未做**（依赖 `insmod` 成功），全部交给 CI 在版本匹配的内核上跑 |

所以：**本地只证明了"能零警告编译出模块"，内核态行为由 CI 证明。**

---

## ioctl ABI 表

命令定义在 `lab_ioctl.h`，内核和用户态**共用同一个头文件**，结构体大小有
`_Static_assert` 编译期断言（`lab_config` 必须 16 字节，`lab_stats` 必须 64 字节）。

| 命令 | 方向 | 序号 | 参数结构体 | 大小 | 说明 |
| --- | --- | --- | --- | --- | --- |
| `LAB_IOC_GET_CONFIG` | `_IOR` | 1 | `struct lab_config` | 16 B | 读取当前配置 |
| `LAB_IOC_SET_CONFIG` | `_IOWR` | 2 | `struct lab_config` | 16 B | 设置配置，并**回填实际生效的值** |
| `LAB_IOC_RESET` | `_IO` | 3 | 无 | 0 B | 清空缓冲区并清零全部统计计数器 |
| `LAB_IOC_GET_STATS` | `_IOR` | 4 | `struct lab_stats` | 64 B | 读取统计计数器 |

`magic = 'L'`；`_IOC_NR` 超过 4 视为未知命令。

### `struct lab_config` (16 B)

| 字段 | 类型 | 语义 |
| --- | --- | --- |
| `buffer_size` | `__u32` | 环形缓冲区字节数。传 **0 表示保持不变**。合法范围 `1024..1048576`，越界返回 `-EINVAL`。非 0 时会**重新分配**缓冲区，旧缓冲区里未读走的数据会被搬过去，装不下的部分计入 `bytes_dropped` |
| `read_chunk` | `__u32` | 单次 `read()` 最多返回的字节数。**0 表示不限制** |
| `flags` | `__u32` | 位掩码，见下表。含未知位返回 `-EINVAL` |
| `reserved` | `__u32` | **必须为 0**，非 0 返回 `-EINVAL`（为将来扩展留位置） |

### `flags` 位

| 宏 | 值 | 语义 |
| --- | --- | --- |
| `LAB_FLAG_NO_DROP` | `1<<0` | 严格模式：缓冲区空间不足时 `write()` 直接返回 `-ENOSPC`，**不覆盖、不丢弃任何数据**。默认为 0（即满时覆盖最老数据） |

### `struct lab_stats` (64 B)

| 字段 | 类型 | 语义 |
| --- | --- | --- |
| `bytes_written` | `__u64` | 被接受并写进缓冲区的字节数 |
| `bytes_read` | `__u64` | 被用户读走的字节数 |
| `bytes_dropped` | `__u64` | 因缓冲区满而被覆盖掉的**未读**字节数（缩容装不下的也计入这里） |
| `wait_count` | `__u64` | 阻塞读进入睡眠的累计次数 |
| `poll_count` | `__u64` | `poll()` / `epoll_wait` 触发回调的累计次数 |
| `write_calls` | `__u64` | 成功进入写入路径的 `write()` 次数 |
| `read_calls` | `__u64` | 成功进入读取路径的 `read()` 次数 |
| `heartbeat` | `__u64` | 内核线程心跳计数，每 `heartbeat_ms` 加 1 |

### ioctl 错误码约定

| 错误码 | 触发条件 |
| --- | --- |
| `-ENOTTY` | magic 不对 / 序号越界 / **参数大小与内核不一致**（用户态头文件版本不匹配） |
| `-EFAULT` | 用户指针不可访问（`access_ok` 检查或 `copy_*_user` 失败） |
| `-EINVAL` | `flags` 含未知位、`reserved != 0`、`buffer_size` 越界、sysfs 写非法值 |
| `-ENOMEM` | 扩容时 `kvmalloc()` 失败 |

---

## 模块参数表

| 参数 | 类型 | 权限 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `buf_size` | `uint` | `0444` | `4096` | 环形缓冲区初始容量。范围 `1024..1048576`，越界时 `insmod` **失败**（`-EINVAL`）。加载后只能通过 `LAB_IOC_SET_CONFIG` 改 |
| `verbose` | `bool` | `0444` | `true` | 是否打印模块加载/卸载日志 |
| `heartbeat_ms` | `uint` | `0644` | `1000` | 内核线程心跳周期（毫秒），范围 `0..60000`。**0 表示不启动内核线程**。权限是 0644 且内核线程每轮直接读这个全局变量，所以**运行期改它能立即生效** |

对应 sysfs 路径：`/sys/module/labdev/parameters/<名字>`

```bash
cat  /sys/module/labdev/parameters/buf_size
echo 200 | sudo tee /sys/module/labdev/parameters/heartbeat_ms   # 立即生效
```

---

## sysfs 属性

路径：`/sys/class/misc/labdev/<属性名>`

| 属性 | 模式 | 说明 |
| --- | --- | --- |
| `buffer_size` | `0444` (只读) | 当前缓冲区容量（字节）。`SET_CONFIG` 扩容后会跟着变 |
| `debug` | `0644` (读写) | 只接受 `0` 或 `1`；写其他值返回 `-EINVAL`。为 1 时打印每次 open/read/write/ioctl 的日志 |
| `stats` | `0444` (只读) | 8 行 `key=value` 的统计快照，方便脚本解析 |

```bash
cat /sys/class/misc/labdev/buffer_size
echo 1 | sudo tee /sys/class/misc/labdev/debug
cat /sys/class/misc/labdev/stats
# bytes_written=0
# bytes_read=0
# bytes_dropped=0
# wait_count=0
# poll_count=0
# write_calls=0
# read_calls=0
# heartbeat=42
```

---

## llseek 的语义

**`llseek` 的偏移是"缓冲区相对偏移"，不是文件偏移。** 这与普通文件完全不同，
所以在这里显式说明。

缓冲区里的数据在被新数据覆盖之前一直保留（见下节），因此读游标可以**回退重读**：

| whence | 基准点 | 说明 |
| --- | --- | --- |
| `SEEK_SET` | 缓冲区里**最老**的那个字节 | `lseek(fd, 0, SEEK_SET)` = 回到最早可读位置 |
| `SEEK_CUR` | 当前读游标 | 允许负偏移，用于回退 |
| `SEEK_END` | 缓冲区末尾（已缓冲数据之后） | 此时 `read()` 会返回 0 或阻塞 |

* 返回值是**相对最老字节的偏移量**（≥ 0），超出范围或 `whence` 非法返回 `-EINVAL`。
* 一旦数据被新写入覆盖（计入 `bytes_dropped`），回退就会以 `-EINVAL` 失败 ——
  "回退能力什么时候消失"只有一个答案：**数据被覆盖的时候**。

```c
write(fd, buf, 100);          /* 写入 100 字节 */
read(fd, tmp, 40);            /* 读走 40 字节 */
lseek(fd, 0, SEEK_SET);       /* 回到 0 */
read(fd, tmp, 100);           /* 仍然能读到完整 100 字节 */
```

> 注：`open()` 里刻意**没有**调用 `nonseekable_open()`/`stream_open()`，
> 因为它们会清掉 `FMODE_LSEEK`，VFS 就再也不会把 `lseek()` 转发到我们的
> `unlocked_ioctl`/`llseek` 了。

---

## 读写与丢弃语义

* `read(fd, buf, count)`
  * 缓冲区有数据：返回 `min(count, 可读字节数, read_chunk)`（`read_chunk=0` 时该项不限制）。
  * 缓冲区空且**没有** `O_NONBLOCK`：**阻塞睡眠**，被 `write()`（或 `ioctl RESET`/扩容）唤醒；
    `wait_count` 加 1。被信号打断返回 `-ERESTARTSYS`。
  * 缓冲区空且**有** `O_NONBLOCK`：立即返回 `-EAGAIN`。
  * `count == 0`：返回 0，不改变任何状态。
  * `copy_to_user` 失败：**回滚读游标**再返回 `-EFAULT`，保证数据不会凭空消失。
* `write(fd, buf, count)`
  * **永不阻塞**，所以 `O_NONBLOCK` 对写没有影响。
  * 单次最多接受 `buffer_size` 字节，超出部分**短写**返回（返回值 < count）。
  * 空间不足时先**无损回收**"已读过但还没被复用"的空间；仍然不足则覆盖最老的未读字节，
    并把被覆盖的字节数累加到 `bytes_dropped`。
  * 开了 `LAB_FLAG_NO_DROP` 时空间不足直接返回 `-ENOSPC`，一个字节都不丢。
* **缓冲区是设备级状态，不是 fd 级状态**：多个 fd 打开同一个设备共享同一个缓冲区
  （这也是为什么 CI 能跑多进程并发压测）；`close()` 再 `open()` 数据仍在。

---

## 测试怎么跑

三层测试，职责不同，**不能互相冒充**：

### 1. 用户态单元测试（`tests/`）—— 验证缓冲区算法

```bash
make test-host          # 等价于 cd tests && make && ./ringbuf_test
```

* 编译的是**内核模块链接的同一份 `ringbuf.c`**（只是内存来自 `malloc()` 而不是 `kvmalloc()`）。
* 内容：14 个确定性边界用例 + 3 组随机模糊测试（共 25 万次随机操作），
  模糊测试用一个"动态数组 + 读游标"的参考模型做**逐字节比对**，任何状态分歧都会失败。
* ⚠️ **这是用户态测试**。它不能证明锁、等待队列、`copy_*_user` 这些内核态行为是对的。

### 2. 内核态功能测试（`tools/dtest.c`）—— 验证真实驱动行为

```bash
sudo ./tools/dtest /dev/labdev
```

覆盖：`open`/`release`、写读往返、`O_NONBLOCK` 空读 `-EAGAIN`、
`fork()` 子进程阻塞读被写入唤醒、`poll` 状态、`epoll` 水平触发 + `EPOLLOUT` 常亮、
`llseek` 回退与越界拒绝、全部 4 条 ioctl（含 `_IOWR` 回填、扩容保数据、缩容计丢弃）、
缓冲区写满覆盖、`LAB_FLAG_NO_DROP` 的 `-ENOSPC`、
非法 ioctl（错误 magic / 序号越界 / **参数大小不匹配**）、非法参数（未知 flags、`reserved!=0`、`buffer_size` 越界）、
非法用户指针（`PROT_NONE` → `-EFAULT` 且读游标回滚）、`count==0`、1 MiB 超大写入短写、
多 fd 共享缓冲区、跨 `close`/`open` 数据保留。每条断言都给 PASS/FAIL，退出码非 0 即失败。

### 3. 多进程并发压测（`tools/stress.py`）—— 验证并发正确性

```bash
sudo python3 tools/stress.py --dev /dev/labdev --procs 4 --threads 2 --seconds 3
```

三个阶段的判据：

| 阶段 | 做法 | 失败判据 |
| --- | --- | --- |
| 1 严格完整性 | 开 `LAB_FLAG_NO_DROP`，1 个写进程 + 1 个读进程传 8 MiB 确定性字节流 | 读到的字节流与写出的**逐字节不完全相同**、字节数不符、`bytes_dropped != 0` |
| 2 并发读写 | 4 进程 × 2 线程（一半写一半读），写 40 字节记录（magic + seq + writer_id + crc32 + payload），读者解析并校验 | 出现 crc 通过但 **payload 与 `(writer_id, seq)` 推导值不符** 的记录；或同一 writer 的 `seq` 非严格递增；或读者一条都读不到；或设备统计与读者累计不一致 |
| 3 open/close 混压 | 4 进程 × 2 线程反复 `open→ioctl→write→poll→read→close` | 进程崩溃/超时、一次迭代都没完成 |

* 缓冲区满覆盖造成的**读者错位**是设计行为，由 resync 处理并**单独计数**，不算失败；
  真正算失败的是"crc 自洽但内容不符"这种硬损坏。
* 输出包含吞吐量（MiB/s、条写/s、条校验/s）。

### 4. CI 里怎么跑

`.github/workflows/ci.yml` 在 `ubuntu-latest` 上按顺序做：
环境信息 → 装 `linux-headers-$(uname -r)` → `make test-host` → `make`（并 grep 编译日志确认无 warning）
→ `make -C tools` → `sudo insmod` + 检查 `/dev/labdev`、`/sys/class/misc/labdev/*`、模块参数
→ sysfs 读写与非法值拒绝 → `sudo ./tools/dtest` → `sudo python3 tools/stress.py`
→ `sudo rmmod` 并检查模块/设备节点/sysfs **三处都无残留** → 非法模块参数必须加载失败
→ 自定义参数重新加载并复跑 dtest → `dmesg | tail -50`。

**所有步骤都不带 `|| true`**：`insmod` 失败、测试失败、`rmmod` 有残留都会让 CI 变红。

---

## 目录结构

```
linux-driver-lab/
├── labdev_main.c            # 内核模块主体：fops / ioctl / sysfs / kthread / 模块参数
├── ringbuf.c                # 环形缓冲区核心实现（内核态与用户态共用同一份）
├── ringbuf.h                # 环形缓冲区接口（不依赖任何内核头文件）
├── lab_ioctl.h              # ioctl ABI：内核与用户态共用的唯一契约 + 编译期大小断言
├── Makefile                 # kbuild 外部模块 Makefile + test-host/tools/check/load/unload 便捷目标
├── LICENSE                  # MIT
├── README.md                # 本文件
├── .gitignore
├── .github/workflows/
│   └── ci.yml               # ubuntu-latest 上真实 insmod 的完整 CI
├── docs/
│   └── 面试问答.md           # misc vs register_chrdev、mutex vs spinlock、poll/epoll、kthread 退出 等
├── tests/
│   ├── Makefile
│   └── ringbuf_test.c       # 用户态单元测试 + 参考模型模糊测试
└── tools/
    ├── Makefile
    ├── dtest.c              # 内核态功能测试（需 insmod）
    └── stress.py            # 多进程/多线程并发压测 + 完整性校验
```

---

## 设计要点

几个容易被问到、也容易写错的地方：

1. **阻塞读的等待循环**：`read()` 在缓冲区空时先 `mutex_unlock()` 再
   `wait_event_interruptible()`，醒来后**重新加锁并回到循环开头重新检查条件**。
   `wait_event_interruptible()` 的语义是"先挂进等待队列、再复查条件"，所以不存在
   丢唤醒；而反复解锁/加锁是为了不把写者挡在临界区外。
2. **`wait_event` 里读共享状态不加锁**：那只是一个提示性判断（`used != consumed`），
   真正的判断在重新加锁之后。这类"良性数据竞争"在内核里是允许的，但必须能说清楚为什么。
3. **`mutex` 而不是 `spinlock`**：本驱动持锁期间要做 `copy_to_user`/`kvmalloc`
   这类**可能睡眠**的操作，自旋锁下这是非法的。临界区里没有硬实时要求，用 mutex 天然正确。
4. **`copy_to_user` 失败要回滚**：不能用普通 `memcpy`（会绕过 `access_ok` 并在
   用户态给出内核地址），也不能"先消费再拷贝"，否则用户态一个坏指针就能把数据吃掉。
   本项目在拷贝失败时用 `ringbuf_seek(..., -n, SEEK_CUR)` 把读游标退回去。
5. **`kthread_stop()` 必须在拿 `dev->lock` 之前调用**：否则线程卡在 `mutex_lock()` 上等锁，
   而 `kthread_stop()` 又在等线程退出 —— 死锁。卸载顺序是
   `kthread_stop()` → `misc_deregister()` → `kvfree()`。
6. **`open()` 里 `try_module_get(THIS_MODULE)`**：让 `rmmod` 在还有 fd 打开时直接
   报 `EBUSY`，而不是把正在被调用的代码卸掉。
7. **`misc_register()` 的 `groups` 字段**：把 sysfs 属性声明式地交给 misc 框架管理，
   注册失败时框架自己回滚，比手工 `sysfs_create_group` + 出错时 `sysfs_remove_group`
   少一整类"忘记注销"的 bug。
8. **`kvmalloc()` 而不是 `kmalloc()`**：缓冲区最大 1 MiB，直接 `kmalloc` 需要高阶
   连续页，内存碎片时容易失败；`kvmalloc` 先试 `kmalloc`，失败退回 `vmalloc`。
9. **ioctl 按完整 `cmd` 匹配**：`_IOR('L',4,struct lab_stats)` 与
   `_IOR('L',4,struct lab_config)` 是不同的整数，所以"用户态头文件与内核不一致"
   会被 `default` 分支以 `-ENOTTY` 拒绝，而不是用错误的大小去写用户内存。

更多问答见 [`docs/面试问答.md`](docs/面试问答.md)。

---

## License

MIT，见 [LICENSE](LICENSE)。
内核模块声明为 `Dual MIT/GPL`，因此可以合法使用 GPL-only 内核符号，也不会污染内核。
