FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg lsb-release software-properties-common \
      make python3 git binutils \
      zlib1g-dev libzstd-dev \
 && curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
 && bash /tmp/llvm.sh 22 \
 && apt-get install -y --no-install-recommends libclang-22-dev libclang-rt-22-dev llvm-22-tools \
 && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

RUN apt-get update \
 && apt-get install -y --no-install-recommends llvm-22-dev lld-22 \
      gcc-mingw-w64-x86-64 \
 && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo

RUN curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.95.0

ENV PATH=/opt/cargo/bin:/usr/lib/llvm-22/bin:$PATH
ENV LLVM_CONFIG=/usr/lib/llvm-22/bin/llvm-config
ENV CC=/usr/lib/llvm-22/bin/clang
ENV CXX=/usr/lib/llvm-22/bin/clang++
ENV CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER=/usr/lib/llvm-22/bin/clang
ENV CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=/usr/lib/llvm-22/bin/clang
