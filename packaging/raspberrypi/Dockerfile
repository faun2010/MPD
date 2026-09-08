FROM node:24-bookworm
RUN dpkg --add-architecture arm64 \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      crossbuild-essential-arm64 python3 python3-pip pkg-config ninja-build \
      qemu-user file binutils-aarch64-linux-gnu \
      libasound2-dev:arm64 libfmt-dev:arm64 libflac-dev:arm64 \
      libavformat-dev:arm64 libavcodec-dev:arm64 libavutil-dev:arm64 \
      libupnp-dev:arm64 libsoxr-dev:arm64 libpcre2-dev:arm64 \
      zlib1g-dev:arm64 libicu-dev:arm64 libsqlite3-dev:arm64 libgtest-dev:arm64 \
 && python3 -m pip install --break-system-packages --no-cache-dir meson==1.3.2 \
 && rm -rf /var/lib/apt/lists/*
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends clang-16 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
