# Music Player Daemon — SACD build

This is [Music Player Daemon](http://www.musicpd.org) (upstream 0.25
development line) plus the Super Audio CD / DVD-Audio work from
[Maxim Anisiutkin's MPD SACD fork](https://sourceforge.net/p/sacddecoder/mpd/),
kept as a small, mergeable delta so upstream can be merged regularly.

What this build adds on top of upstream MPD:

- **sacdiso** decoder: SACD ISO images (`.iso`, `.dat`) are scanned as
  containers; stereo and multichannel tracks, plain DSD and DST, disc
  metadata and artwork.
- **dvdaiso** decoder: DVD-Audio ISO images (PCM and MLP).
- **dff** decoder: native DSDIFF from the SACD tree, exact DSD tail, DST
  cache flushed on seek, safe against concurrent rescans.
- **ALSA** options `dsd_to_pcm_bridge` and `pcm_to_dsd_bridge`: a short
  PCM silence at the right word clock when switching between native DSD and
  PCM, for DACs that cannot change mode and clock at once (verified on a
  Shanling UA2 Plus).
- A unified installer (`install.sh`), verification scripts and a Raspberry
  Pi ARM64 release pipeline.

The version reports as `0.25.1-sacd`; the MPD protocol version is unchanged.
See [NEWS](NEWS) for the change list and
[doc/plugins.rst](doc/plugins.rst) for the plugin and output settings.

## Building

The SACD decoders need *libixml* (part of *libupnp*) and are enabled
automatically when it is found; use `-Dsacdiso=enabled -Ddvdaiso=enabled`
to require them or `disabled` to leave them out. Native DSD output needs
`-Ddsd=true` (the default).

```sh
meson setup build/release --buildtype=release -Dsacdiso=enabled -Ddvdaiso=enabled
ninja -C build/release
```

`./mybuild.sh` configures a debug-optimized tree with tests in
`build/sacd-debug`. For the general build instructions see the
[upstream manual](https://www.musicpd.org/doc/user/install.html).

## Running a local test instance

```sh
./mybuild.sh
MUSIC_DIR=/path/to/music AUDIO_DEVICE='hw:CARD=Plus,DEV=0' DSD_SWITCH_BRIDGE=yes ./run-local.sh
```

This starts an isolated MPD on `127.0.0.1:6601` with its database, state
and playlists under `build/sacd-debug/runtime/`; it does not touch the
system MPD or its configuration. `MUSIC_DIR`, `AUDIO_DEVICE`, `MPD_PORT`,
`OUTPUT_NAME`, `BUILD_DIR` and `DSD_SWITCH_BRIDGE` (enables both ALSA
bridges) are the knobs. SACD images appear in the library as
`album.iso/2C_AUDIO__TRACK001.iso`; DFF files are ordinary songs.

To use the bridges in your own `mpd.conf`:

```
audio_output {
    type "alsa"
    name "DAC"
    device "hw:CARD=Plus,DEV=0"
    dop "no"
    dsd_to_pcm_bridge "yes"
    pcm_to_dsd_bridge "yes"
    stop_dsd_silence "yes"
}
```

## Verification

```sh
meson test -C build/sacd-debug --print-errorlogs
python3 test/verify_sacd_decode.py /path/to/music        # bit-exact decode, no sound card
python3 test/verify_sacd_playback.py                     # native DSD playback on the ALSA device
python3 test/verify_format_switch.py                     # FLAC/CUE <-> DFF/ISO switching
```

The decode check compares every uncompressed DFF with its source DSD
payload by length and SHA256 and fully decodes each ISO track. The playback
checks drive the local instance (they replace its queue) and record the
ALSA hardware state, seeks, pause/resume and format transitions. Reports
are written to `build/sacd-debug/verification/`.

## Installing and releases

```sh
./install.sh --check --binary build/release/mpd
./install.sh --binary build/release/mpd            # into ~/.local
sudo ./install.sh --prefix /usr/local --binary build/release/mpd
```

The installer verifies the binary, keeps previous versions, never touches
configuration or services, and `./install.sh --uninstall` restores what it
replaced. Release archives (`mpd-sacd-linux-x86_64`, `mpd-sacd-arm64-bookworm`)
are produced by the `release` GitHub workflow for tags matching `v*-sacd*`;
the Raspberry Pi / Debian 12 aarch64 bundle and its Docker cross build are
described in [packaging/raspberrypi/README.md](packaging/raspberrypi/README.md).

## 中文简述

本仓库是上游 MPD 0.25 开发线加上 SACD ISO、DVD-Audio ISO 与 DFF 原生 DSD
解码，以及针对 DSD/PCM 切换的 ALSA 静音桥。`./mybuild.sh` 构建，
`./run-local.sh` 启动独立测试实例（默认端口 6601），`test/` 下的三个脚本分别做
逐字节解码校验、原生 DSD 实机播放校验和格式切换校验。安装用
`./install.sh`，树莓派 ARM64 发行包见 `packaging/raspberrypi/`。

## Upstream

- [Manual](https://mpd.readthedocs.io/en/stable/user.html)
- [Protocol specification](https://mpd.readthedocs.io/en/latest/protocol.html)
- [Developer manual](https://mpd.readthedocs.io/en/latest/developer.html)
- [Upstream repository](https://github.com/MusicPlayerDaemon/MPD) and
  [bug tracker](https://github.com/MusicPlayerDaemon/MPD/issues/); please
  report problems with the SACD decoders or the ALSA bridges here, not
  upstream.

## Legal

MPD is released under the
[GNU General Public License version 2](https://www.gnu.org/licenses/gpl-2.0.txt),
which is distributed in the COPYING file. The SACD and DVD-Audio libraries
in `src/lib/sacdiso` and `src/lib/dvdaiso` keep their original licences and
copyright notices.
