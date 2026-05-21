#!/usr/bin/env bash
set -euo pipefail

JOBS="${N00B_JOBS:-2}"

copy_logs_and_exit() {
    local status=$1

    cp -a build_linux/meson-logs /logs/ 2>/dev/null || true
    exit "${status}"
}

run_n00b_build() {
    set +e
    bash build.sh build_linux
    local status=$?
    set -e
    copy_logs_and_exit "${status}"
}

echo "--- Building ncc (native Linux) ---"
cp -a /src/subprojects/ncc /build_ncc
cd /build_ncc
rm -rf build
CC=clang meson setup --buildtype=release build .
meson compile -C build -j "${JOBS}"
cp build/ncc /usr/local/bin/ncc

# Build n00b using the freshly-built ncc. Force the build to use
# /usr/local/bin/ncc; the cached subprojects/ncc/build_release/ncc is the host
# binary when a developer ran `bash build.sh` before invoking this wrapper.
cp -a /src /build
cd /build
rm -rf /build/subprojects/ncc/build_release
export NCC_PATH=/usr/local/bin/ncc

if [ "${N00B_TEST:-0}" = "1" ] && command -v dbus-run-session >/dev/null; then
    cat >/tmp/n00b-dbus-build.sh <<'N00B_DBUS_BUILD'
#!/usr/bin/env bash
set -euo pipefail

# Unlock the keyring with an empty password in headless CI mode.
printf "\n" | gnome-keyring-daemon \
    --start --foreground \
    --components=secrets >/dev/null 2>&1 &

for _ in 1 2 3 4 5; do
    if dbus-send --session --print-reply \
        --dest=org.freedesktop.secrets \
        /org/freedesktop/secrets \
        org.freedesktop.DBus.Peer.Ping \
        >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done

set +e
bash build.sh build_linux
status=$?
set -e
cp -a build_linux/meson-logs /logs/ 2>/dev/null || true
exit "${status}"
N00B_DBUS_BUILD

    chmod +x /tmp/n00b-dbus-build.sh
    exec dbus-run-session -- /tmp/n00b-dbus-build.sh
fi

run_n00b_build
