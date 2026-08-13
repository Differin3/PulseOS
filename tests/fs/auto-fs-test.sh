#!/usr/bin/env bash
# Standalone guest filesystem autotest (CMD -> WSL -> QEMU -> serial)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

AUTO_LOG="$ROOT/logs/auto-fs-test.log"
SERIAL_LOG="$ROOT/logs/qemu-serial.log"
SERIAL_PORT="${SERIAL_PORT:-4444}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
FS_TIMEOUT="${FS_TIMEOUT:-180}"
MYOS_HEADLESS="${MYOS_HEADLESS:-1}"
MYOS_VERBOSE="${MYOS_VERBOSE:-1}"
MYOS_NIC="${MYOS_NIC:-virtio}"
SKIP_BUILD=0

mkdir -p "$ROOT/logs"
: > "$AUTO_LOG"

pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*"; exit 1; }

log() { echo "$*" | tee -a "$AUTO_LOG"; }

for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=1 ;;
        --rebuild) make clean || true; SKIP_BUILD=0 ;;
    esac
done

{
echo ""
echo "======== $(date '+%Y-%m-%d %H:%M:%S') auto-fs-test.sh ========"
echo "== config: SERIAL_PORT=$SERIAL_PORT NIC=$MYOS_NIC HEADLESS=$MYOS_HEADLESS"

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    echo "== build (make)"
    make
else
    echo "== skip build"
fi

if [[ ! -f "$ROOT/myos.img" ]]; then
    echo "Creating myos.img"
    dd if=/dev/zero of="$ROOT/myos.img" bs=1M count=64 status=none
fi

pkill -f "qemu-system-i386.*serial.*tcp::$SERIAL_PORT" 2>/dev/null || true
sleep 0.5
: > "$SERIAL_LOG"

QEMU_DISPLAY=(-display none)
if [[ "$MYOS_HEADLESS" != "1" ]]; then
    QEMU_DISPLAY=(-display gtk)
fi

NIC_ARGS=()
if [[ "$MYOS_NIC" == "rtl8139" ]]; then
    NIC_ARGS=(-netdev user,id=net0,hostfwd=tcp::8080-:8080 -device rtl8139,netdev=net0)
else
    NIC_ARGS=(-netdev user,id=net0,hostfwd=tcp::8080-:8080 -device virtio-net-pci,netdev=net0)
fi

echo "== Starting QEMU for FS autotest"
qemu-system-i386 \
    -cdrom "$ROOT/myos.iso" \
    -m 128 \
    -serial "tcp:127.0.0.1:$SERIAL_PORT,server,nowait" \
    -drive "file=$ROOT/myos.img,if=none,format=raw,id=disk0" \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    "${QEMU_DISPLAY[@]}" \
    "${NIC_ARGS[@]}" \
    -no-reboot \
    >"$ROOT/logs/qemu-fs-stdout.log" 2>&1 &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

python3 - "$SERIAL_PORT" "$BOOT_TIMEOUT" "$FS_TIMEOUT" "$SERIAL_LOG" <<'PY'
import os, socket, sys, threading, time

PORT = int(sys.argv[1])
BOOT_TIMEOUT = float(sys.argv[2])
FS_TIMEOUT = float(sys.argv[3])
SERIAL_LOG = sys.argv[4]

buf = []
lock = threading.Lock()
stop = threading.Event()

def get_text():
    with lock:
        return "".join(buf)

def reader(sock):
    with open(SERIAL_LOG, "a", encoding="utf-8", errors="replace") as logf:
        while not stop.is_set():
            try:
                data = sock.recv(4096)
            except OSError:
                break
            if not data:
                break
            s = data.decode("utf-8", errors="replace")
            with lock:
                buf.append(s)
            logf.write(s)
            logf.flush()
            if os.environ.get("MYOS_VERBOSE", "1") == "1":
                sys.stdout.write(s)
                sys.stdout.flush()

def wait_substrings(needles, timeout, progress_label="wait"):
    deadline = time.time() + timeout
    last = 0.0
    while time.time() < deadline:
        text = get_text()
        for n in needles:
            if n in text:
                return True
        now = time.time()
        if now - last >= 5:
            print("== %s (%.0fs left)" % (progress_label, deadline - now))
            last = now
        time.sleep(0.1)
    return False

sock = None
deadline = time.time() + 30
while time.time() < deadline:
    try:
        sock = socket.create_connection(("127.0.0.1", PORT), timeout=2)
        break
    except OSError:
        time.sleep(0.3)
if sock is None:
    print("[FAIL] Serial connect timeout")
    sys.exit(1)

print("== Connecting serial 127.0.0.1:%d" % PORT)
t = threading.Thread(target=reader, args=(sock,), daemon=True)
t.start()

print("== Waiting for guest shell")
if not wait_substrings(["Keyboard ready", "/ >", "Net OK", "Filesystem initialized"], BOOT_TIMEOUT):
    print("[FAIL] Boot timeout")
    print(get_text()[-2000:])
    sys.exit(1)
print("[PASS] Guest ready")

print("== Sending: autotest fs")
sock.sendall(b"autotest fs\n")

if not wait_substrings(["[AUTOTEST] fs ok", "[INF][autotest] fs_ok"], FS_TIMEOUT,
                       progress_label="waiting fs_ok"):
    text = get_text()
    print("[FAIL] FS autotest did not pass")
    print("--- serial tail ---")
    print(text[-3000:])
    if "fs FAIL" in text or "fs_failed" in text:
        print("[HINT] guest reported a specific FS step failure")
    sys.exit(1)

print("[PASS] guest filesystem autotest")
stop.set()
sock.close()
sys.exit(0)
PY

RC=$?
if [[ "$RC" -ne 0 ]]; then
    fail "FS orchestrator failed ($RC)"
fi

if grep -qE 'fs_ok|\[AUTOTEST\] fs ok' "$SERIAL_LOG" 2>/dev/null; then
    pass "FS autotest marker in serial log"
else
    fail "no fs_ok in serial log"
fi
if grep -q 'fs_failed\|fs FAIL' "$SERIAL_LOG" 2>/dev/null; then
    fail "FS autotest reported failure"
fi
if grep -q 'symlink_ok' "$SERIAL_LOG" 2>/dev/null; then
    pass "symlink_ok"
else
    fail "no symlink_ok in serial log"
fi
if grep -q 'journal_ok' "$SERIAL_LOG" 2>/dev/null; then
    pass "journal_ok"
else
    fail "no journal_ok in serial log"
fi
if grep -q 'fd_file_ok' "$SERIAL_LOG" 2>/dev/null; then
    pass "fd_file_ok"
else
    fail "no fd_file_ok in serial log"
fi
if grep -q '\[INF\]\[fs\] init ok' "$SERIAL_LOG" 2>/dev/null; then
    pass "FS init ok at boot"
else
    echo "[WARN] no [INF][fs] init ok (older image?)"
fi
if grep -q 'command timeout' "$SERIAL_LOG" 2>/dev/null; then
    echo "[WARN] AHCI command timeout seen during run"
else
    pass "no AHCI command timeout"
fi

echo ""
echo "============================================"
echo " FS AUTO TEST PASSED"
echo " Logs: $AUTO_LOG"
echo "       $SERIAL_LOG"
echo "============================================"
exit 0

} 2>&1 | tee -a "$AUTO_LOG"
