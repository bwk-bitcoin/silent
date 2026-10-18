#!/usr/bin/env bash
#
# Device GUI end to end: the real silent app (the device_e2e probe, built with
# -DSILENT_BUILD_DEVICE_E2E=ON) on a private Xvfb display, against a regtest chain
# (the e2e_chain example), a Ledger on Speculos (key B, nanosp, app pinned in
# versions.env) and the Coldcard EDGE simulator (key C), key A hot in silent. It
# creates a 2-of-3 MuSig2 shared wallet, registers it on both devices, funds it and
# spends it twice: through the Ledger + Coldcard leaf (with a lost MuSig2 session,
# the simulator restarted between the rounds) and through the Ledger + hot leaf.
# Everything is torn down on exit.
#
# Usage:
#   tests/e2e/devices/run.sh
#
# Prerequisites: docker (the Speculos image, and the app builder image when no ELF is
# given), Xvfb, git, gcc and make, autoconf, automake and libtool (the Coldcard
# simulator build), SDL2, python3 with venv, curl, cmake, Qt6 dev, a Rust toolchain
# and the rest of silent's build dependencies, lib/qontrol (`just qontrol`).
#
# Environment:
#   SILENT_E2E_CACHE      : cache of the Ledger ELF, the Coldcard firmware, the Python
#                           venv and the runs (default
#                           ${XDG_CACHE_HOME:-$HOME/.cache}/silent/e2e-devices).
#   SPECULOS_ELF          : a prebuilt Ledger Bitcoin Test ELF of LEDGER_APP_VERSION
#                           for LEDGER_MODEL; copied into the cache, skips the build.
#   BWK_HWI_DIR           : bwk's hwi crate, whose Speculos cache
#                           (target/speculos-cache, hwi/tests/speculos/run.sh) is
#                           looked into before building the ELF.
#   COLDCARD_FIRMWARE_DIR : a Coldcard firmware checkout at COLDCARD_FIRMWARE_COMMIT,
#                           used in place (its simulator built when it is not).
#   E2E_SKIP_BUILD=1      : run the silent binaries already built.
#   E2E_DISPLAY           : the first X display number tried (default 90).
#
# Outputs: a run directory under $SILENT_E2E_CACHE/runs (printed first) with the
# probe, chain, Speculos, clicker and simulator logs, the probe's HOME and shots/,
# one whole window screenshot per step. The script ends with the probe's
# `failures: N` and exits with its status.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
HERE="$ROOT/tests/e2e/devices"
cd "$ROOT"

# shellcheck disable=SC1091
. "$HERE/versions.env"

CACHE="${SILENT_E2E_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/silent/e2e-devices}"
mkdir -p "$CACHE/runs"
CACHE="$(cd "$CACHE" && pwd)"
RUN_DIR="$(mktemp -d -p "$CACHE/runs" run.XXXXXX)"
echo "==> Run directory $RUN_DIR"

CONTAINER=silent-e2e-speculos
COLDCARD_SOCKET=/tmp/ckcc-simulator.sock
SPECULOS_API=http://127.0.0.1:5000
# GCC 14 turns these warnings of the pinned firmware into errors.
COLDCARD_CFLAGS="-Wno-error=dangling-pointer -Wno-error=enum-int-mismatch -Wno-unterminated-string-initialization -Wno-error=array-bounds -Wno-error=unused-but-set-variable"

fail() {
    echo "$*" >&2
    exit 1
}

# ===== Preflight =====

for tool in docker Xvfb git gcc make python3 curl cmake cargo timeout; do
    command -v "$tool" >/dev/null || fail "$tool not found on PATH"
done
for port in 9999 5000; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
        fail "port $port is in use: Speculos needs it (another Speculos running?)"
    fi
done
if [[ -e "$COLDCARD_SOCKET" ]]; then
    if python3 -I -c 'import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.connect(sys.argv[1])' "$COLDCARD_SOCKET" 2>/dev/null; then
        fail "a Coldcard simulator answers on $COLDCARD_SOCKET: stop it first"
    fi
    echo "==> Removing the stale $COLDCARD_SOCKET"
    rm -f "$COLDCARD_SOCKET"
fi
DISPLAY_NUM="${E2E_DISPLAY:-90}"
for _ in $(seq 20); do
    [[ -e /tmp/.X11-unix/X$DISPLAY_NUM || -e /tmp/.X$DISPLAY_NUM-lock ]] || break
    DISPLAY_NUM=$((DISPLAY_NUM + 1))
done
[[ -e /tmp/.X11-unix/X$DISPLAY_NUM || -e /tmp/.X$DISPLAY_NUM-lock ]] &&
    fail "no free X display from ${E2E_DISPLAY:-90}"
[[ "$SPECULOS_IMAGE" == *@sha256:* ]] || fail "SPECULOS_IMAGE=$SPECULOS_IMAGE is not pinned by digest"
[[ "$LEDGER_APP_BUILDER_IMAGE" == *@sha256:* ]] ||
    fail "LEDGER_APP_BUILDER_IMAGE=$LEDGER_APP_BUILDER_IMAGE is not pinned by digest"

# ===== Ledger ELF (cached) =====

# The key of bwk-hwi's hwi/tests/speculos/run.sh: app version + builder image.
LEDGER_KEY="${LEDGER_APP_VERSION}_$(echo "$LEDGER_APP_BUILDER_IMAGE" | tr -c '[:alnum:]' '_')"
ELF="$CACHE/ledger/$LEDGER_MODEL/$LEDGER_KEY/app.elf"
BWK_ELF="${BWK_HWI_DIR:+$BWK_HWI_DIR/target/speculos-cache/$LEDGER_MODEL/$LEDGER_KEY/app.elf}"
mkdir -p "$(dirname "$ELF")"
if [[ -n "${SPECULOS_ELF:-}" ]]; then
    [[ -f "$SPECULOS_ELF" ]] || fail "SPECULOS_ELF=$SPECULOS_ELF is not a file"
    echo "==> Using prebuilt $SPECULOS_ELF"
    cp "$SPECULOS_ELF" "$ELF"
elif [[ -f "$ELF" ]]; then
    echo "==> Using cached $ELF"
elif [[ -n "$BWK_ELF" && -f "$BWK_ELF" ]]; then
    echo "==> Using bwk's cached $BWK_ELF"
    cp "$BWK_ELF" "$ELF"
else
    echo "==> Building Ledger Bitcoin app $LEDGER_APP_VERSION for $LEDGER_MODEL"
    case "$LEDGER_MODEL" in
        nanox) SDK_VAR=NANOX_SDK ;;
        nanosp) SDK_VAR=NANOSP_SDK ;;
        *) fail "unknown LEDGER_MODEL $LEDGER_MODEL (expected nanox, nanosp)" ;;
    esac
    BUILD_DIR="$(mktemp -d -p "$CACHE" ledger-build.XXXXXX)"
    git clone --branch "$LEDGER_APP_VERSION" --depth 1 \
        https://github.com/LedgerHQ/app-bitcoin-new.git "$BUILD_DIR/app-bitcoin-new"
    docker run --rm -e CPATH=/app/src -e SDK_VAR="$SDK_VAR" -e JOBS="${CARGO_BUILD_JOBS:-6}" \
        -u "$(id -u):$(id -g)" -v "$BUILD_DIR/app-bitcoin-new:/app" -w /app \
        "$LEDGER_APP_BUILDER_IMAGE" \
        bash -c 'make -j"$JOBS" BOLOS_SDK="${!SDK_VAR}" COIN=bitcoin_testnet'
    cp "$BUILD_DIR/app-bitcoin-new/bin/app.elf" "$ELF"
    rm -rf "$BUILD_DIR"
    echo "==> Cached $ELF"
fi

# ===== Coldcard simulator (cached) =====

if [[ -n "${COLDCARD_FIRMWARE_DIR:-}" ]]; then
    FIRMWARE="$(cd "$COLDCARD_FIRMWARE_DIR" && pwd)"
    HEAD="$(git -C "$FIRMWARE" rev-parse HEAD)"
    [[ "$HEAD" == "$COLDCARD_FIRMWARE_COMMIT" ]] ||
        fail "COLDCARD_FIRMWARE_DIR is at $HEAD, expected $COLDCARD_FIRMWARE_COMMIT"
    echo "==> Using the Coldcard firmware checkout $FIRMWARE"
else
    FIRMWARE="$CACHE/coldcard/$COLDCARD_FIRMWARE_COMMIT"
    if [[ ! -d "$FIRMWARE" ]]; then
        echo "==> Cloning the Coldcard firmware at $COLDCARD_FIRMWARE_COMMIT"
        CLONE_DIR="$(mktemp -d -p "$CACHE" coldcard-clone.XXXXXX)"
        git clone --recursive "$COLDCARD_FIRMWARE_REPO" "$CLONE_DIR/firmware"
        git -C "$CLONE_DIR/firmware" checkout "$COLDCARD_FIRMWARE_COMMIT"
        git -C "$CLONE_DIR/firmware" submodule update --init --recursive
        mkdir -p "$(dirname "$FIRMWARE")"
        mv "$CLONE_DIR/firmware" "$FIRMWARE"
        rm -rf "$CLONE_DIR"
    else
        echo "==> Using the cached Coldcard firmware $FIRMWARE"
    fi
fi
if [[ ! -x "$FIRMWARE/unix/coldcard-mpy" ]]; then
    echo "==> Building the Coldcard simulator"
    (
        cd "$FIRMWARE/unix"
        # the links of `make setup`, without its vanilla micropython (`make tools`)
        ln -sfn ../external/micropython/ports/unix l-port
        ln -sfn ../external/micropython l-mpy
        ln -sfn ../external/micropython/ports/unix/coldcard-mpy coldcard-mpy
        # the frozen modules are compiled with it, and `make` does not build it
        make -C ../external/micropython/mpy-cross CFLAGS_EXTRA="$COLDCARD_CFLAGS"
        make ngu-setup
        make CFLAGS_EXTRA="$COLDCARD_CFLAGS"
    ) 2>&1 | tee "$RUN_DIR/coldcard-build.log"
    grep -q "LINK coldcard-mpy" "$RUN_DIR/coldcard-build.log" ||
        fail "the Coldcard simulator build did not link coldcard-mpy"
else
    echo "==> Using the built Coldcard simulator $FIRMWARE/unix/coldcard-mpy"
fi

VENV="$CACHE/venv"
REQUIREMENTS="$HERE/drivers/requirements.txt"
STAMP="$VENV/requirements.sha256"
if [[ ! -f "$STAMP" ]] || ! sha256sum --status -c "$STAMP" 2>/dev/null; then
    echo "==> Creating the Python venv $VENV"
    rm -rf "$VENV"
    python3 -m venv "$VENV"
    PIP_CACHE_DIR="$CACHE/pip" "$VENV/bin/pip" install --quiet --requirement "$REQUIREMENTS"
    sha256sum "$REQUIREMENTS" >"$STAMP"
else
    echo "==> Using the Python venv $VENV"
fi

# ===== silent =====

CHAIN_BIN="$ROOT/silent/target/debug/examples/e2e_chain"
PROBE_BIN="$ROOT/build-e2e/device_e2e"
if [[ "${E2E_SKIP_BUILD:-}" != 1 ]]; then
    echo "==> Building silent: the e2e_chain example and the device_e2e probe"
    cargo build --manifest-path silent/Cargo.toml --example e2e_chain
    # device_e2e depends on RunBeforeBuild, which runs build.sh (the Rust binding).
    cmake -S . -B build-e2e -DSILENT_BUILD_DEVICE_E2E=ON
    cmake --build build-e2e --target device_e2e -j"${CARGO_BUILD_JOBS:-6}"
else
    echo "==> E2E_SKIP_BUILD=1: using the built $PROBE_BIN"
fi
[[ -x "$CHAIN_BIN" ]] || fail "$CHAIN_BIN is not built"
[[ -x "$PROBE_BIN" ]] || fail "$PROBE_BIN is not built"

# ===== Run =====

XVFB_PID=""
CLICKER_PID=""
# shellcheck disable=SC2317 # run by the EXIT trap
cleanup() {
    docker logs "$CONTAINER" >"$RUN_DIR/speculos.log" 2>&1 || true
    docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    if [[ -n "$CLICKER_PID" ]]; then
        kill -TERM "$CLICKER_PID" 2>/dev/null || true
    fi
    # the simulator the probe left running, if it did not stop it
    if [[ -f "$RUN_DIR/coldcard-sim.pid" ]]; then
        SIM_PID="$(cat "$RUN_DIR/coldcard-sim.pid")"
        kill -TERM -- -"$SIM_PID" 2>/dev/null || true
        for _ in $(seq 50); do
            kill -0 -- -"$SIM_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill -KILL -- -"$SIM_PID" 2>/dev/null || true
        rm -rf "$COLDCARD_SOCKET" /tmp/cc-simulators/"$SIM_PID"
    fi
    if [[ -n "$XVFB_PID" ]]; then
        kill -TERM "$XVFB_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "==> Xvfb :$DISPLAY_NUM"
Xvfb ":$DISPLAY_NUM" -screen 0 2600x1200x24 -nolisten tcp >"$RUN_DIR/xvfb.log" 2>&1 &
XVFB_PID=$!
for _ in $(seq 50); do
    [[ -S /tmp/.X11-unix/X$DISPLAY_NUM ]] && break
    sleep 0.1
done
[[ -S /tmp/.X11-unix/X$DISPLAY_NUM ]] || fail "Xvfb :$DISPLAY_NUM did not start"

# Speculos as bwk-hwi's hwi/tests/devices/run.sh starts it, under another name.
echo "==> Speculos $SPECULOS_IMAGE"
docker image inspect "$SPECULOS_IMAGE" >/dev/null 2>&1 || docker pull "$SPECULOS_IMAGE"
docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
docker run -d --name "$CONTAINER" \
    -p 127.0.0.1:9999:9999 -p 127.0.0.1:5000:5000 \
    -v "$(dirname "$ELF")":/app:ro \
    "$SPECULOS_IMAGE" \
    --model "$LEDGER_MODEL" --display headless --api-port 5000 --apdu-port 9999 \
    --seed "$SEED_B" "/app/$(basename "$ELF")" >/dev/null
for _ in $(seq 60); do
    curl -sf "$SPECULOS_API/events" >/dev/null && break
    sleep 1
done
curl -sf "$SPECULOS_API/events" >/dev/null ||
    fail "Speculos did not answer on $SPECULOS_API within 60 s"
echo "==> ELF $(sha256sum "$ELF")"

python3 -I "$HERE/drivers/ledger_clicker.py" --api "$SPECULOS_API" \
    --log "$RUN_DIR/ledger-screens.log" &
CLICKER_PID=$!

echo "==> Probe (log $RUN_DIR/probe.log)"
mkdir -p "$RUN_DIR/home"
set +e
timeout 2400 env -u WAYLAND_DISPLAY \
    HOME="$RUN_DIR/home" DISPLAY=":$DISPLAY_NUM" QT_QPA_PLATFORM=xcb \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    BWK_COLDCARD_SIMULATOR="$COLDCARD_SOCKET" RUST_LOG=bwk_hwi=debug \
    E2E_RUN_DIR="$RUN_DIR" E2E_CHAIN_BIN="$CHAIN_BIN" \
    E2E_COLDCARD_UNIX_DIR="$FIRMWARE/unix" E2E_PYTHON="$VENV/bin/python" \
    E2E_DRIVERS_DIR="$HERE/drivers" E2E_LEDGER_SCREENS="$RUN_DIR/ledger-screens.log" \
    E2E_SEED_A="$SEED_A" E2E_SEED_C="$SEED_C" \
    "$PROBE_BIN" >"$RUN_DIR/probe.log" 2>&1
STATUS=$?
set -e

grep -E '^(OK  |FAIL|-- |txid |shots: |failures: )' "$RUN_DIR/probe.log" || true
echo "==> Shots in $RUN_DIR/shots"
grep '^txid ' "$RUN_DIR/probe.log" || echo "==> no txid"
grep -q '^failures: ' "$RUN_DIR/probe.log" || echo "failures: probe ended with status $STATUS"
exit "$STATUS"
