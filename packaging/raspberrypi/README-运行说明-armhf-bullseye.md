# MPD SACD / Raspberry Pi armhf · Bullseye（32 位）

适用：32 位 ARM Linux / ARMv7-A 硬浮点，glibc 2.31 或更新，例如 Raspbian / Raspberry Pi OS Bullseye
32 位、Debian 11 armhf，也适用于更新的 Bookworm。ARMv6 的 Pi 1 / Zero(1) 不适用。

解压后可以直接运行，也可以用统一安装器安装。包内含非 glibc 依赖库：

```sh
uname -m                       # 应为 armv7l（内核为 64 位时用户空间仍可是 armhf）
getconf GNU_LIBC_VERSION        # 应为 glibc 2.31 或更高
sha256sum -c SHA256SUMS
./install.sh --check
./mpd --version
```

安装到当前用户 `~/.local`，把 `~/.local/bin` 放进 PATH：

```sh
./install.sh
export PATH="$HOME/.local/bin:$PATH"
mpd --version
```

安装到所有用户可用的 `/usr/local`，由你手工授权执行：

```sh
sudo ./install.sh --prefix /usr/local
```

若已有其他版本占用目标 `bin/mpd`，安装器会停止；审阅后添加 `--force` 可先备份再替换。
后续升级使用相同命令，保留已验证的旧版本。卸载并恢复安装前保存的命令：

```sh
./install.sh --uninstall
# 对应系统级安装：sudo ./install.sh --uninstall --prefix /usr/local
```

安装器不下载、不安装系统依赖、不改用户配置、不启停服务。
正在运行的 MPD 继续使用原进程；系统服务若指向 `/usr/bin/mpd`，需由你另外调整服务。

首次以当前用户启动：

```sh
mkdir -p ~/.local/state/mpd-sacd/playlists ~/.config/mpd-sacd
cp mpd.conf.example ~/.config/mpd-sacd/mpd.conf
aplay -l
# 编辑 ~/.config/mpd-sacd/mpd.conf：音乐目录、端口、USB DAC 的 hw:CARD=...,DEV=0。
./mpd --no-daemon --stderr ~/.config/mpd-sacd/mpd.conf
```

支持 SACD ISO、DFF/DSD、FLAC、APE（FFmpeg）与 CUE。
USB 原生 DSD 需要 DAC 与内核支持，并使用 `hw:` 设备。
ALSA 配置由系统 `libasound2-data` 提供；若缺失，可通过系统包管理器安装
`alsa-utils libasound2-data`。

示例中的双向 PCM 静音桥默认关闭。Shanling UA2 Plus 的原生 DSD/PCM 切换问题，
可按示例启用 `dsd_to_pcm_bridge` 与 `pcm_to_dsd_bridge`；每次适用切换会播放
500 ms 静音。其他 DAC 先按默认配置验证，再决定是否启用。

`VALIDATION.json` 是此包的验证范围；QEMU 测试不代表树莓派实际播放通过。
`BUILD.json` 和 `BUILD-PACKAGES.tsv` 记录源码、架构与依赖版本，许可见 `licenses/`，
MPD 对应源码和构建脚本见 `source/mpd-source.tar.gz`。

## 关于本包的构建方式

Bullseye 自带 GCC 10，编不了 MPD 0.25 需要的 C++23，因此本包用较新的 clang 16
对着 Debian Bullseye armhf 的 sysroot 交叉编译，并把 libstdc++ 静态链入，
使可执行文件不依赖系统的 GCC 10 运行时。GCC 12 的 libstdc++ 引用了两个晚于
glibc 2.31 的符号（`__libc_single_threaded`、`arc4random`），由 `glibc231-compat.c`
提供；前者取保守值 0，使 libstdc++ 始终走原子路径，后者基于 `getrandom(2)`。

包内 `lib/` 是构建时链接的同一批 Debian Bullseye armhf 库，运行时优先使用它们，
因此不依赖你系统上装了哪些库。ICU 会引入系统或包内的 `libstdc++.so.6`（GCC 10），
与静态链入的 GCC 12 libstdc++ 在同一进程中共存；MPD 只使用 ICU 的 C 接口，
没有 C++ 对象跨越边界。

`BUILD-PACKAGES.tsv` 记录这批库的 Debian 二进制包与源码包精确版本。
