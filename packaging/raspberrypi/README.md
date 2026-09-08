# Raspberry Pi ARM64 构建与发行

目标是通用 ARMv8-A / AArch64、Debian 12 或 Raspberry Pi OS Bookworm 64 位，
最低 glibc 2.36。不会生成 ARMv7/armhf 或 Bullseye 安装包。

在仓库根目录运行。交叉工具链和目标依赖只装入 Docker 镜像：

```sh
docker build -t mpd-sacd-rpi-cross:bookworm -f packaging/raspberrypi/Dockerfile .
docker run --rm --network none --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$PWD,dst=/src" \
    mpd-sacd-rpi-cross:bookworm sh packaging/raspberrypi/build.sh
docker run --rm --network none --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$PWD,dst=/src" \
    mpd-sacd-rpi-cross:bookworm python3 packaging/raspberrypi/verify.py
```

`build/rpi-arm64/mpd` 是 ARM64 ELF。编译使用 `-march=armv8-a`，
单元测试和解码使用 QEMU `cortex-a53`；不把构建宿主的 CPU 特性带入目标。
Clang 16 尚未实现 C++23 P2448 的 constexpr 定义放宽，故关闭
`invalid-constexpr` 定义诊断；常量求值仍由编译器检查。
fmt 9 使用 header-only 模式避免与 Debian GCC 的模板 ABI 差异。

默认验证生成无版权音乐内容的 DFF 字节模式与 FLAC 整数样本，逐字节对照解码，
并检查 FFmpeg 的 APE decoder/demuxer。若有本地 SACD ISO，可额外传入
`--iso-uri /music/disc.iso/2C_AUDIO__TRACK001.iso --iso-reference /fixtures/reference.raw --iso-seek 759`，
并将对应只读目录挂入容器。参考数据须由独立解码生成，不把用户音乐放进发行包。
未提供 ISO 样本时，验证报告会明确记录未测试。

源码和安装器提交完成后打包：

```sh
docker run --rm --network none --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$PWD,dst=/src" \
    mpd-sacd-rpi-cross:bookworm python3 packaging/raspberrypi/package.py
cd build/releases
sha256sum -c SHA256SUMS-arm64
mkdir relocated
tar -xzf mpd-sacd-arm64-bookworm.tar.gz -C relocated
(cd relocated/mpd-sacd-arm64-bookworm && sha256sum -c SHA256SUMS)
```

再次打包同一输出目录时需显式加 `--replace`。打包器要求相关源码无未提交改动，
核对编译时 Git 版本与发行提交之间的可执行源码未变，确认验证报告对应待打包 ELF。
它递归收集非 glibc 共享库、核对 ELF 架构和最高 GLIBC 符号版本，并要求每个
非 glibc 库从包内 `lib/` 解析。可在容器里对解压后的目录运行
`verify.py --bundle-dir build/releases/relocated/mpd-sacd-arm64-bookworm` 再检验搬移结果。

发行产物位于 `build/releases/`：

- `mpd-sacd-arm64-bookworm.tar.gz`：含统一安装器、运行脚本、ELF、库、配置示例、许可、源码和验证报告。
- `mpd-sacd-arm64-source.tar.gz`：相同提交的构建/测试源码与安装和交叉构建脚本，排除 IDE 状态与音乐样本。
- `SHA256SUMS-arm64`：以上两个归档的相对路径校验。

包内 `BUILD.json` 标识发行源码提交、编译源码提交、ARMv8-A/glibc 基线和 ELF 哈希；
`BUILD-PACKAGES.tsv` 记录 Debian 二进制包与源码包的精确版本。
依赖库的 Debian 源码获取信息位于 `source/DEPENDENCIES.md`。

QEMU user 会引入后台线程，完整 MPD 启动可能触发启动线程检查。
这里的单元测试、解码与动态链接验证不代表完整 daemon 或树莓派/DAC 实际播放通过。
实板应运行安装器 `--check`，再用自己的 DAC 验证格式、DSD 和双向切换。
