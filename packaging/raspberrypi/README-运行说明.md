# MPD SACD / Raspberry Pi ARM64

适用：64 位 Linux / ARMv8-A，glibc 2.36 或更新，例如 Raspberry Pi OS Bookworm
64 位、Debian 12。32 位系统和 Bullseye 的 glibc 2.31 不适用。

解压后可以直接运行，也可以用统一安装器安装。包内含非 glibc 依赖库：

```sh
uname -m                       # 应为 aarch64
getconf GNU_LIBC_VERSION        # 应为 glibc 2.36 或更高
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
