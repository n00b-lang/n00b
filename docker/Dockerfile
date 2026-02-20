FROM ubuntu:24.04 AS toolchain

ARG TARGETPLATFORM
ARG TARGETARCH
ARG DEBIAN_FRONTEND=noninteractive
ARG MESON_VERSION=1.6.1
ARG LLVM_VERSION=22
ARG LLVM_DIST=noble

ENV CC=clang
ENV TARGETPLATFORM=${TARGETPLATFORM}
ENV TARGETARCH=${TARGETARCH}

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        bash \
        ca-certificates \
        file \
        git \
        gpg \
        gpg-agent \
        wget \
        ninja-build \
        pkg-config \
        python3 \
        python3-pip \
    && wget -qO /usr/share/keyrings/llvm-snapshot.gpg.key https://apt.llvm.org/llvm-snapshot.gpg.key \
    && gpg --dearmor --yes --output /usr/share/keyrings/llvm-archive-keyring.gpg /usr/share/keyrings/llvm-snapshot.gpg.key \
    && echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/${LLVM_DIST}/ llvm-toolchain-${LLVM_DIST}-${LLVM_VERSION} main" > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && apt-get install --yes --no-install-recommends \
        clang-${LLVM_VERSION} \
        lld-${LLVM_VERSION} \
    && python3 -m pip install --no-cache-dir --break-system-packages "meson==${MESON_VERSION}" \
    && ln -sf /usr/bin/clang-${LLVM_VERSION} /usr/local/bin/clang \
    && ln -sf /usr/bin/clang++-${LLVM_VERSION} /usr/local/bin/clang++ \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workdir

FROM toolchain AS verify

ARG TARGETPLATFORM
ARG TARGETARCH

ENV TARGETPLATFORM=${TARGETPLATFORM}
ENV TARGETARCH=${TARGETARCH}

COPY --chmod=0755 docker/run-build.sh /usr/local/bin/run-build.sh
COPY . /workdir

RUN /usr/local/bin/run-build.sh

FROM toolchain AS verify-windows-cross

ARG TARGETPLATFORM
ARG TARGETARCH

ENV TARGETPLATFORM=${TARGETPLATFORM}
ENV TARGETARCH=${TARGETARCH}

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        binutils-mingw-w64-x86-64 \
        gcc-mingw-w64-x86-64 \
        g++-mingw-w64-x86-64 \
        wine64 \
    && rm -rf /var/lib/apt/lists/*

COPY --chmod=0755 docker/run-build-windows.sh /usr/local/bin/run-build-windows.sh
COPY . /workdir

RUN /usr/local/bin/run-build-windows.sh
