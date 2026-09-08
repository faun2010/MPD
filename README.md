# Music Player Daemon

本分支支持 SACD ISO 音轨和 DFF 原生 DSD。树莓派 ARM64 发行包保留 FLAC 与
FFmpeg 解码能力（包括 APE），面向 64 位 Raspberry Pi OS / Debian 12 及更新系统，
要求 glibc 2.36 或更新版本；不能安装到 32 位树莓派系统。

解压发行包后运行通用安装脚本：

```sh
./install.sh --check
./install.sh
```

默认安装到 `~/.local`，命令为 `~/.local/bin/mpd`。安装前会校验文件、CPU 架构及
动态库兼容性；保留已有配置，不下载依赖、不自动提权或启动服务。
已有同名命令时默认拒绝覆盖；显式 `--force` 会备份后替换，卸载时恢复备份。
安装到系统命令目录需手工执行：

```sh
sudo ./install.sh --prefix /usr/local --force
```

卸载使用 `./install.sh --uninstall`（系统安装须使用相同 `--prefix` 和权限）。
用户修改过的安装文件不会被静默删除。发行包的 `mpd.conf.example` 是配置模板，
需要自行填写音乐目录、ALSA 设备并创建状态目录；设备专属的 DSD 切换选项默认不启用。
发行版 `mpd.service` 通常使用 `/usr/bin/mpd`，安装到其他前缀不会自动修改其启动路径。

从源码构建本机版本后，也可以用同一脚本安装指定二进制：

```sh
./mybuild.sh
./install.sh --binary build/sacd-debug/mpd
```

源码安装模式使用本机已有动态库，不生成跨系统便携包。
ARM64 交叉编译、便携包与源码包的生成方法见
[树莓派构建说明](packaging/raspberrypi/README.md)。

本地声卡验证使用独立 demo：

```sh
./mybuild.sh
./run-local.sh
```

另开终端控制独立实例（默认仅监听本机 `6601`）：

```sh
mpc -h 127.0.0.1 -p 6601 update --wait
mpc -h 127.0.0.1 -p 6601 clear
mpc -h 127.0.0.1 -p 6601 add /
mpc -h 127.0.0.1 -p 6601 play
mpc -h 127.0.0.1 -p 6601 stop
```

默认音乐目录为 `~/Music`，ALSA 输出为 `default`，关闭 DoP。
需要测试 USB DAC 时显式指定音乐目录和设备；例如已验证的 Shanling UA2 Plus：

```sh
MUSIC_DIR=/path/to/music AUDIO_DEVICE='hw:CARD=Plus,DEV=0' DSD_SWITCH_BRIDGE=yes ./run-local.sh
```

`DSD_SWITCH_BRIDGE=yes` 启用双向过渡，默认关闭。
其中 `dsd_to_pcm_bridge "yes"` 从原生 DSD 切到不同采样率的 PCM 时，
先按旧 DSD 的实际 ALSA 字率和声道数播放 500 ms 的 16 位 PCM 静音，排空并关闭，
再打开目标 PCM。这样把模式切换与时钟切换分开，不改变队列或目标音乐的采样率。
`pcm_to_dsd_bridge "yes"` 从 PCM 切到不同 ALSA 字率的原生 DSD 时，
先查询目标 DSD 的实际格式、字率和声道数，不应用 DSD 硬件参数；
按该字率播放 500 ms 的 16 位 PCM 静音，排空并关闭，再打开目标 DSD。
两项选项通用默认关闭；设备必须精确支持中间 PCM 格式，首次打开和 DoP 不走过渡路径。
反向过渡无需事先播放 DSD，不应用于解码为 PCM 的回退路径。
本机 UA2 Plus 的原生 DSD64 与双声道 16 位 PCM 44.1 kHz，已在 ncmpcpp 中
直接选曲双向试听通过，采集的目标音乐前缀与独立解码一致。
去掉额外关闭等待后，多次双向切换也正常；最终仅保留 500 ms PCM 静音桥。
证据覆盖用户听感、ALSA 数据及运行状态，不能确定 DAC 内部具体异常状态。

demo 同时开启 `stop_dsd_silence "yes"`，停止或取消 DSD 时输出格式正确的静音；
已回补 drain 后清除 DSD 残留块的修复。旧 `thesycon_dsd_workaround` 保持关闭。
可通过 `MUSIC_DIR`、`AUDIO_DEVICE`、`MPD_PORT`、`BUILD_DIR` 和 `DSD_SWITCH_BRIDGE` 调整。
配置、曲库和状态保存在所选构建目录的 `runtime/`（默认 `build/sacd-debug/runtime/`）；
本地运行脚本不安装或修改系统服务。
ISO 入库后展开为 `专辑.iso/2C_AUDIO__TRACK001.iso` 等音轨，单轨 DFF 直接入库。

对上述 UA2 Plus 启用切换修复时，在实际 ALSA `audio_output` 块中配置
`dop "no"`、`dsd_to_pcm_bridge "yes"`、`pcm_to_dsd_bridge "yes"`、
`stop_dsd_silence "yes"`，并保持 `thesycon_dsd_workaround` 关闭。
旧测试配置中的 `dsd_switch_delay` 已移除，不再需要填写。
仅替换二进制不会改变已运行的进程；下一次启动该二进制时生效。
本机发行版 `mpd.service` 的 `ExecStart` 仍是 `/usr/bin/mpd`，上述命令不会更改它。

复验命令（播放测试会替换上述独立实例的队列，并通过声卡播放）：

```sh
meson test -C build/sacd-debug --print-errorlogs
python3 test/verify_sacd_decode.py /path/to/music
python3 test/verify_sacd_playback.py
```

完整解码校验将未压缩 DFF 的输出与源 DSD 数据块比较长度和 SHA256；
播放校验记录 ALSA 原生 DSD 格式、推进的硬件指针、跳转和暂停恢复，
并保存 ISO/DFF 的 PCM 波形。报告位于 `build/sacd-debug/verification/`。
这些证据覆盖解码和 Linux/USB 声卡传输；不代表人工听音或模拟端测量。

http://www.musicpd.org

A daemon for playing music of various formats.  Music is played through the 
server's audio device.  The daemon stores info about all available music, 
and this info can be easily searched and retrieved.  Player control, info
retrieval, and playlist management can all be managed remotely.

For basic installation instructions
[read the manual](https://www.musicpd.org/doc/user/install.html).

# Users

- [Manual](https://mpd.readthedocs.io/en/stable/user.html), or see `/usr/share/doc/mpd/html` in your installation
- [Forum](https://github.com/MusicPlayerDaemon/MPD/discussions)
- [IRC](ircs://irc.libera.chat:6697/#mpd)
- [Bug tracker](https://github.com/MusicPlayerDaemon/MPD/issues/)

# Developers

- [Protocol specification](https://mpd.readthedocs.io/en/latest/protocol.html)
- [Developer manual](https://mpd.readthedocs.io/en/latest/developer.html)

# Legal

MPD is released under the
[GNU General Public License version 2](https://www.gnu.org/licenses/gpl-2.0.txt),
which is distributed in the COPYING file.
