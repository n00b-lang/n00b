#!/usr/bin/env bash
# Prepare a macOS SDK tarball for Docker cross-compilation with osxcross.
#
# Usage:
#   bash docker/prepare-sdk.sh /path/to/MacOSX15.2.sdk.tar.xz
#   bash docker/prepare-sdk.sh /path/to/MacOSX15.2.sdk/     # extracted dir
#
# After running this, rebuild the Docker image:
#   docker build -t n00b-linux \
#     --build-arg MACOS_SDK=<filename>.tar.xz docker/
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="${SCRIPT_DIR}/sdk"

usage() {
    echo "Usage: $0 <path-to-sdk>"
    echo ""
    echo "  <path-to-sdk> can be:"
    echo "    - A .tar.xz or .tar.gz SDK tarball (e.g. MacOSX15.2.sdk.tar.xz)"
    echo "    - An extracted SDK directory (e.g. /path/to/MacOSX15.2.sdk/)"
    echo ""
    echo "The SDK can be obtained from Xcode.app:"
    echo "  /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/"
    exit 1
}

if [[ $# -ne 1 ]]; then
    usage
fi

INPUT="$1"

# Validate the input exists
if [[ ! -e "$INPUT" ]]; then
    echo "ERROR: '$INPUT' does not exist."
    exit 1
fi

mkdir -p "$SDK_DIR"

if [[ -d "$INPUT" ]]; then
    # Input is an extracted SDK directory — validate and tar it up
    if [[ ! -f "$INPUT/SDKSettings.json" ]] && [[ ! -f "$INPUT/SDKSettings.plist" ]]; then
        echo "ERROR: '$INPUT' does not look like a macOS SDK."
        echo "Expected to find SDKSettings.json or SDKSettings.plist inside it."
        exit 1
    fi

    SDK_NAME="$(basename "$INPUT")"
    # Ensure it has .sdk suffix
    if [[ "$SDK_NAME" != *.sdk ]]; then
        SDK_NAME="${SDK_NAME}.sdk"
    fi
    TARBALL="${SDK_NAME}.tar.xz"

    echo "Packaging '$INPUT' as ${TARBALL}..."
    # osxcross expects the tarball to contain a single MacOSX*.sdk/ directory
    tar -C "$(dirname "$INPUT")" -cJf "${SDK_DIR}/${TARBALL}" "$(basename "$INPUT")"
    echo "Created ${SDK_DIR}/${TARBALL}"

elif [[ -f "$INPUT" ]]; then
    # Input is a tarball — validate it
    case "$INPUT" in
        *.tar.xz|*.tar.gz|*.tar.bz2|*.tgz)
            ;;
        *)
            echo "ERROR: Unrecognized archive format: '$INPUT'"
            echo "Expected .tar.xz, .tar.gz, .tar.bz2, or .tgz"
            exit 1
            ;;
    esac

    TARBALL="$(basename "$INPUT")"
    cp "$INPUT" "${SDK_DIR}/${TARBALL}"
    echo "Copied ${TARBALL} to ${SDK_DIR}/"
else
    echo "ERROR: '$INPUT' is neither a file nor a directory."
    exit 1
fi

echo ""
echo "Next steps:"
echo "  1. Rebuild the Docker image with the SDK:"
echo "     docker build -t n00b-linux --build-arg MACOS_SDK=${TARBALL} docker/"
echo ""
echo "  2. Cross-compile for macOS:"
echo "     bash docker/cross-build.sh macos-arm64"
echo "     # or: bash build.sh  (auto-delegates on macOS when image has osxcross)"
