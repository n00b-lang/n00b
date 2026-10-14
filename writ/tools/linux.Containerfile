FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg lsb-release software-properties-common \
      meson ninja-build python3 pkg-config git binutils \
      libicu-dev libutf8proc-dev zlib1g-dev \
 && curl -fsSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh \
 && bash /tmp/llvm.sh 22 \
 && apt-get install -y --no-install-recommends libclang-22-dev libclang-rt-22-dev llvm-22-tools \
 && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo
ENV PATH=/opt/cargo/bin:$PATH

RUN ln -s /usr/lib/llvm-22/bin/clang /usr/local/bin/cc \
 && curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain 1.95.0 \
 && rustup toolchain install nightly --profile minimal --component miri,rust-src

ENV LLVM_PREFIX=/usr/lib/llvm-22
ENV CC=/usr/lib/llvm-22/bin/clang
