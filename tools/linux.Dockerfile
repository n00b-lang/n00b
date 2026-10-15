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

RUN apt-get update \
 && apt-get install -y --no-install-recommends llvm-22-dev \
 && rm -rf /var/lib/apt/lists/*

ENV PATH=/usr/lib/llvm-22/bin:$PATH
ENV LLVM_CONFIG=/usr/lib/llvm-22/bin/llvm-config
ENV CC=/usr/lib/llvm-22/bin/clang
ENV CXX=/usr/lib/llvm-22/bin/clang++
