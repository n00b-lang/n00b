#!/usr/bin/env bash
set -uo pipefail

SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_MAJOR=22

say() {
    printf 'configure: %s\n' "$*" >&2
}

die() {
    printf 'configure: error: %s\n' "$1" >&2
    shift
    local line
    for line in "$@"; do
        printf '  %s\n' "$line" >&2
    done
    exit 1
}

[ "$#" -eq 1 ] && [ -n "$1" ] || {
    printf 'configure: error: expected one argument, the output file\n' >&2
    printf 'usage: tools/configure.sh OUTPUT.mk\n' >&2
    exit 2
}
OUT="$1"

case "${BUILD_TYPE:-debugoptimized}" in
    debug|debugoptimized|release|sanitize) ;;
    *) die "BUILD_TYPE is '$BUILD_TYPE'; use debug, debugoptimized, release or sanitize" ;;
esac
case "${N00B_RUST:-0}" in
    0|1) ;;
    *) die "N00B_RUST is '$N00B_RUST'; use 0 or 1" ;;
esac

llvm_major() {
    "$1" --version 2>/dev/null | sed -n 's/^\([0-9][0-9]*\)\..*/\1/p'
}

usable_llvm() {
    [ -n "$1" ] && [ -x "$1" ] && [ "$(llvm_major "$1")" = "$LLVM_MAJOR" ]
}

candidates() {
    [ -n "${LLVM_CONFIG:-}" ] && printf '%s\n' "$LLVM_CONFIG"
    command -v "llvm-config-$LLVM_MAJOR" 2>/dev/null
    command -v llvm-config 2>/dev/null
    if command -v brew >/dev/null 2>&1; then
        local formula
        for formula in "llvm@$LLVM_MAJOR" llvm; do
            local prefix
            prefix="$(brew --prefix "$formula" 2>/dev/null)" &&
                printf '%s\n' "$prefix/bin/llvm-config"
        done
    fi
    printf '%s\n' "/usr/lib/llvm-$LLVM_MAJOR/bin/llvm-config" \
                  "/usr/local/opt/llvm/bin/llvm-config" \
                  "/opt/homebrew/opt/llvm/bin/llvm-config"
    [ -n "${N00B_PREFIX:-}" ] &&
        printf '%s\n' "$N00B_PREFIX"/share/n00b/llvm/*/bin/llvm-config
    printf '%s\n' "$HOME"/.local/share/n00b/llvm/*/bin/llvm-config
}

LLVM_RELEASE=22.1.4

find_llvm() {
    local candidate
    while IFS= read -r candidate; do
        if usable_llvm "$candidate"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done < <(candidates)
    return 1
}

release_asset() {
    case "$(uname -s)-$(uname -m)" in
        Darwin-arm64) printf 'LLVM-%s-macOS-ARM64.tar.xz\n' "$LLVM_RELEASE" ;;
        Linux-x86_64) printf 'LLVM-%s-Linux-X64.tar.xz\n' "$LLVM_RELEASE" ;;
        Linux-aarch64|Linux-arm64) printf 'LLVM-%s-Linux-ARM64.tar.xz\n' "$LLVM_RELEASE" ;;
        *) return 1 ;;
    esac
}

download_llvm() {
    local asset dest answer
    asset="$(release_asset)" || return 1
    dest="${N00B_PREFIX:-$HOME/.local}/share/n00b/llvm/$LLVM_RELEASE"
    if [ "${N00B_DOWNLOAD_LLVM:-}" != 1 ]; then
        [ -t 0 ] || return 1
        say "n00b can download the official LLVM $LLVM_RELEASE release (about 1.5-2 GB)"
        say "into $dest."
        read -r -p "configure: Download it now? [y/N] " answer || return 1
        case "$answer" in
            y|Y|yes|YES) ;;
            *) return 1 ;;
        esac
    fi
    local url="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_RELEASE/$asset"
    local staging="$dest.download"
    rm -rf "$staging" && mkdir -p "$staging" || return 1
    say "downloading $url"
    curl -fL --progress-bar "$url" -o "$staging/$asset" || return 1
    say "unpacking into $dest"
    tar -xJf "$staging/$asset" -C "$staging" --strip-components=1 &&
        rm -f "$staging/$asset" && rm -rf "$dest" && mv "$staging" "$dest" || return 1
    printf '%s\n' "$dest/bin/llvm-config"
}

if [ -n "${LLVM_CONFIG:-}" ] && ! usable_llvm "$LLVM_CONFIG"; then
    if [ ! -x "$LLVM_CONFIG" ]; then
        die "LLVM_CONFIG is '$LLVM_CONFIG', which is not an executable" \
            "point it at the llvm-config of an LLVM $LLVM_MAJOR install, or unset it"
    fi
    die "LLVM_CONFIG is '$LLVM_CONFIG', which reports LLVM $(llvm_major "$LLVM_CONFIG"), not $LLVM_MAJOR" \
        "point it at the llvm-config of an LLVM $LLVM_MAJOR install, or unset it"
fi

LLVM="$(find_llvm)" || LLVM="$(download_llvm)" || LLVM=""

if [ -z "$LLVM" ]; then
    case "$(uname -s)" in
        Darwin) HINT="install it with: brew install llvm@$LLVM_MAJOR" ;;
        Linux)
            if command -v apt-get >/dev/null 2>&1; then
                HINT="install it with: curl -fsSL https://apt.llvm.org/llvm.sh | sudo bash -s $LLVM_MAJOR; then sudo apt-get install llvm-$LLVM_MAJOR-dev libclang-$LLVM_MAJOR-dev"
            elif command -v dnf >/dev/null 2>&1; then
                HINT="install it with: sudo dnf install llvm-devel clang-devel"
            else
                HINT="install LLVM $LLVM_MAJOR with your package manager"
            fi ;;
        *) HINT="install LLVM $LLVM_MAJOR" ;;
    esac
    die "LLVM $LLVM_MAJOR was not found" "$HINT" \
        "or point LLVM_CONFIG at an llvm-config for LLVM $LLVM_MAJOR"
fi

REAL_PREFIX="$("$LLVM" --prefix)"
BINDIR="$(cd "$(dirname "$LLVM")" && pwd)"
PREFIX_DIR="$(dirname "$BINDIR")"
LIBDIR="$PREFIX_DIR/lib"
INCLUDEDIR="$PREFIX_DIR/include"

stable() {
    sed "s|$REAL_PREFIX|$PREFIX_DIR|g"
}
CC_CHOICE="$BINDIR/clang"
[ -x "$CC_CHOICE" ] ||
    die "LLVM at $PREFIX_DIR has no clang" "install the full LLVM toolchain"

RUST="${N00B_RUST:-0}"
CARGO=""
if [ "$RUST" = 1 ]; then
    CARGO="$(command -v cargo 2>/dev/null)" ||
        die "N00B_RUST=1 needs cargo on PATH" "install Rust, or configure with N00B_RUST=0"
    RESHARP="${RESHARP_ROOT:-$HOME/resharp-writ}"
    WANT="$(tr -d '[:space:]' < "$SOURCE/writ/deps/RESHARP_REVISION")"
    [ -d "$RESHARP" ] ||
        die "N00B_RUST=1 needs the resharp workspace, but $RESHARP does not exist" \
            "set RESHARP_ROOT to its location"
    if command -v jj >/dev/null 2>&1 && [ -d "$RESHARP/.jj" ]; then
        HAVE="$(jj -R "$RESHARP" log --no-graph -r '@-' -T 'commit_id' 2>/dev/null)"
        [ "$HAVE" = "$WANT" ] ||
            die "resharp at $RESHARP is on $HAVE; the pinned revision is $WANT"
    elif [ "$(cat "$RESHARP/.writ-pin" 2>/dev/null)" != "$WANT" ]; then
        die "resharp at $RESHARP is not marked with the pinned revision $WANT"
    fi
fi

SDK=""
if [ "$(uname -s)" = Darwin ]; then
    SDK="${SDKROOT:-$(xcrun --show-sdk-path 2>/dev/null)}"
fi

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HOST_WINDOWS=1 ;;
    *) HOST_WINDOWS="" ;;
esac

LLVM_COMPONENTS="core bitwriter bitreader linker passes x86 aarch64"

static_archive() {
    local name="$1" dir
    for dir in "$LIBDIR" \
               "$( (brew --prefix "$name" 2>/dev/null || true) )/lib" \
               "/opt/homebrew/opt/$name/lib" "/usr/local/opt/$name/lib" \
               /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu \
               /usr/lib64 /usr/lib /usr/local/lib; do
        [ -f "$dir/lib$name.a" ] && { printf '%s\n' "$dir/lib$name.a"; return 0; }
    done
    return 1
}

static_system_libs() {
    local lib archive
    for lib in $("$LLVM" --link-static --system-libs 2>/dev/null); do
        case "$lib" in
            -lzstd)
                archive="$(static_archive zstd)" || return 1
                printf '%s ' "$archive" ;;
            *z3*|-lxml2|-lm|-lpthread|-ldl) ;;
            *) printf '%s ' "$lib" ;;
        esac
    done
}

STATIC_PROBLEM=""
if [ ! -f "$LIBDIR/libLLVMCore.a" ]; then
    STATIC_PROBLEM="LLVM at $PREFIX_DIR has no static libraries"
elif ! SYSTEM_STATIC="$(static_system_libs)"; then
    case "$(uname -s)" in
        Darwin) ZSTD_HINT="brew install zstd" ;;
        *) ZSTD_HINT="install libzstd-dev, or zstd-static/libzstd-devel" ;;
    esac
    STATIC_PROBLEM="no static zstd library was found ($ZSTD_HINT)"
fi

if [ -z "$STATIC_PROBLEM" ]; then
    if [ "$(uname -s)" = Darwin ]; then
        CXX_RUNTIME="-lc++ -Wl,-dead_strip"
    elif [ -n "$HOST_WINDOWS" ]; then
        CXX_RUNTIME="-lc++ -Wl,--gc-sections"
    else
        CXX_RUNTIME="-lstdc++ -Wl,--gc-sections"
    fi
    LLVM_LINK_LIBS="$("$LLVM" --link-static --ldflags | stable) $("$LLVM" --link-static --libs $LLVM_COMPONENTS) $SYSTEM_STATIC $CXX_RUNTIME"
    LLVM_TEST_LINK_LIBS="$("$LLVM" --link-static --ldflags | stable) $("$LLVM" --link-static --libs $LLVM_COMPONENTS all-targets) $SYSTEM_STATIC $CXX_RUNTIME"
    LLVM_LINKAGE=static
else
    LLVM_LINK_LIBS="$("$LLVM" --link-shared --ldflags | stable) -Wl,-rpath,$LIBDIR $("$LLVM" --link-shared --libs $LLVM_COMPONENTS) $("$LLVM" --link-shared --system-libs 2>/dev/null)"
    LLVM_TEST_LINK_LIBS="$LLVM_LINK_LIBS"
    LLVM_LINKAGE=shared
    say "$STATIC_PROBLEM; linking LLVM as a shared library"
fi

CROSS_TARGETS=""
if [ -z "$HOST_WINDOWS" ] && printf '#include <windows.h>\n' |
    "$CC_CHOICE" --target=x86_64-w64-windows-gnu -fsyntax-only -x c - \
        >/dev/null 2>&1; then
    CROSS_TARGETS="x86_64-windows"
fi

mkdir -p "$(dirname "$OUT")" 2>/dev/null && : > "$OUT.tmp" 2>/dev/null ||
    die "cannot write the configuration to $OUT"
cat > "$OUT.tmp" <<EOF
LLVM_CONFIG := $LLVM
LLVM_BINDIR := $BINDIR
LLVM_LIBDIR := $LIBDIR
LLVM_INCLUDEDIR := $INCLUDEDIR
LLVM_PREFIX := $PREFIX_DIR
LLVM_CFLAGS := $("$LLVM" --cflags | stable | sed 's/-fno-exceptions//; s/-funwind-tables//')
LLVM_LIBS := $LLVM_LINK_LIBS
LLVM_TEST_LIBS := $LLVM_TEST_LINK_LIBS
LLVM_LINKAGE := $LLVM_LINKAGE
CC := $CC_CHOICE
LLVM_LINK := $BINDIR/llvm-link
LLVM_AR := $BINDIR/llvm-ar
CROSS_TARGETS := $CROSS_TARGETS
BUILD_TYPE := ${BUILD_TYPE:-debugoptimized}
N00B_RUST := $RUST
CARGO := $CARGO
SDKROOT := $SDK
SOURCE_ROOT := $SOURCE
EOF
mv "$OUT.tmp" "$OUT"
say "using LLVM $("$LLVM" --version) from $PREFIX_DIR, linked $LLVM_LINKAGE"
if [ -n "$CROSS_TARGETS" ]; then
    say "cross runtimes: $CROSS_TARGETS"
fi
