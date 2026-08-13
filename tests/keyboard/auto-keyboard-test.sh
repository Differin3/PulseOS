#!/usr/bin/env bash
# Guest keyboard decode autotest (inject scancodes → serial markers)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

SERIAL_LOG="$ROOT/logs/qemu-serial.log"
SERIAL_PORT="${SERIAL_PORT:-4445}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
KBD_TIMEOUT="${KBD_TIMEOUT:-60}"
SKIP_BUILD=0

mkdir -p "$ROOT/logs"

pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*"; exit 1; }

for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=1 ;;
    esac
done

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    echo "== build"
    make -f Makefile.wsl
else
    echo "== skip build"
fi

[[ -f "$ROOT/myos.iso" ]] || fail "myos.iso missing"

if [[ ! -f "$ROOT/myos.img" ]]; then
    dd if=/dev/zero of="$ROOT/myos.img" bs=1M count=64 status=none
fi

pkill -f "qemu-system-i386.*serial.*tcp::$SERIAL_PORT" 2>/dev/null || true
sleep 0.5
: > "$SERIAL_LOG"

qemu-system-i386 \
    -cdrom "$ROOT/myos.iso" \
    -m 128 \
    -serial "tcp:127.0.0.1:$SERIAL_PORT,server,nowait" \
    -drive "file=$ROOT/myos.img,if=none,format=raw,id=disk0" \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -display none \
    -no-reboot \
    >"$ROOT/logs/qemu-kbd-stdout.log" 2>&1 &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

python3 - "$SERIAL_PORT" "$BOOT_TIMEOUT" "$KBD_TIMEOUT" "$SERIAL_LOG" <<'PY'
import os, socket, sys, threading, time

PORT = int(sys.argv[1])
BOOT_TIMEOUT = float(sys.argv[2])
KBD_TIMEOUT = float(sys.argv[3])
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

def wait_substrings(needles, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = get_text()
        for n in needles:
            if n in text:
                return True
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

t = threading.Thread(target=reader, args=(sock,), daemon=True)
t.start()

if not wait_substrings(["Keyboard ready", "/ >", "Filesystem initialized"], BOOT_TIMEOUT):
    print("[FAIL] Boot timeout")
    print(get_text()[-2000:])
    sys.exit(1)

sock.sendall(b"autotest keyboard\n")

if not wait_substrings(["keyboard_ok", "[AUTOTEST] keyboard ok"], KBD_TIMEOUT):
    print("[FAIL] keyboard autotest did not pass")
    print(get_text()[-3000:])
    sys.exit(1)

print("[PASS] guest keyboard autotest")
stop.set()
sock.close()
sys.exit(0)
PY

RC=$?
if [[ "$RC" -ne 0 ]]; then
    fail "keyboard orchestrator failed ($RC)"
fi

for m in kbd_ascii_ok kbd_shift_ok kbd_edit_ok kbd_special_ok kbd_ctrl_ok kbd_caps_ok kbd_burst_ok keyboard_ok; do
    if grep -q "$m" "$SERIAL_LOG" 2>/dev/null; then
        pass "$m"
    else
        fail "missing $m"
    fi
done

echo "[PASS] keyboard autotest complete"
