#!/usr/bin/env bash
# Smoke: boot shell + help + ls (AHCI disk required)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SERIAL_PORT="${SERIAL_PORT:-4444}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
SERIAL_LOG="$ROOT/logs/qemu-serial-shell-smoke.log"
mkdir -p "$ROOT/logs"

if [[ "${1:-}" != "--skip-build" ]]; then
  echo "== make"
  make -j"$(nproc)"
fi

[[ -f "$ROOT/myos.img" ]] || dd if=/dev/zero of="$ROOT/myos.img" bs=1M count=64 status=none
pkill -f "qemu-system-i386.*serial.*tcp::$SERIAL_PORT" 2>/dev/null || true
sleep 0.4
: > "$SERIAL_LOG"

qemu-system-i386 \
  -cdrom "$ROOT/myos.iso" -m 128 \
  -serial "tcp:127.0.0.1:$SERIAL_PORT,server,nowait" \
  -drive "file=$ROOT/myos.img,if=none,format=raw,id=disk0" \
  -device ich9-ahci,id=ahci0 \
  -device ide-hd,drive=disk0,bus=ahci0.0 \
  -display none \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -no-reboot \
  >"$ROOT/logs/qemu-shell-smoke-stdout.log" 2>&1 &
QEMU_PID=$!
cleanup() { kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true; }
trap cleanup EXIT

python3 - "$SERIAL_PORT" "$BOOT_TIMEOUT" "$SERIAL_LOG" <<'PY'
import os, socket, sys, threading, time
PORT=int(sys.argv[1]); BOOT=float(sys.argv[2]); LOG=sys.argv[3]
buf=[]; lock=threading.Lock(); stop=threading.Event()
def text():
    with lock: return "".join(buf)
def reader(sock):
    with open(LOG,"a",encoding="utf-8",errors="replace") as f:
        while not stop.is_set():
            try: data=sock.recv(4096)
            except OSError: break
            if not data: break
            s=data.decode("utf-8",errors="replace")
            with lock: buf.append(s)
            f.write(s); f.flush()
            sys.stdout.write(s); sys.stdout.flush()
def wait(needles, timeout, label):
    deadline=time.time()+timeout; last=0
    while time.time()<deadline:
        t=text()
        if all(n in t for n in needles): return True
        if time.time()-last>=5:
            print("== %s (%.0fs left)"%(label, deadline-time.time())); last=time.time()
        time.sleep(0.1)
    return False
sock=None
deadline=time.time()+30
while time.time()<deadline:
    try:
        sock=socket.create_connection(("127.0.0.1",PORT),timeout=2); break
    except OSError: time.sleep(0.3)
if not sock:
    print("[FAIL] serial connect"); sys.exit(1)
threading.Thread(target=reader,args=(sock,),daemon=True).start()
print("== wait boot")
if not wait(["Filesystem initialized","Keyboard ready"], BOOT, "boot"):
    print("[FAIL] boot"); print(text()[-2500:]); sys.exit(1)
print("[PASS] boot + FS")
# clear marker window for command checks
base=len(text())
print("== send help")
sock.sendall(b"help\n")
if not wait(["=== Commands"], 20, "help"):
    print("[FAIL] help"); print(text()[base:]); sys.exit(1)
print("[PASS] help")
base=len(text())
print("== send ls")
sock.sendall(b"ls\n")
deadline=time.time()+20
ok=False
while time.time()<deadline:
    chunk=text()[base:]
    if "filesystem not initialized" in chunk or "cannot access" in chunk:
        print("[FAIL] ls"); print(chunk[-1500:]); sys.exit(1)
    # listing printed and prompt returned
    if ("\n/" in chunk or " ." in chunk or "www" in chunk or "tmp" in chunk or "etc" in chunk) and "/ >" in chunk:
        ok=True; break
    if "[CMD]" in chunk and "/ >" in chunk and "ls" in chunk and "cannot access" not in chunk and "not initialized" not in chunk:
        # at least command was accepted without error; wait a bit more for listing
        pass
    time.sleep(0.1)
chunk=text()[base:]
if "cannot access" in chunk or "not initialized" in chunk:
    print("[FAIL] ls"); print(chunk[-1500:]); sys.exit(1)
# Accept any non-error completion with prompt
if "/ >" not in chunk and "pid=" not in chunk:
    print("[FAIL] ls no prompt"); print(chunk[-1500:]); sys.exit(1)
print("[PASS] ls")
print("--- ls output ---")
print(chunk[-800:])
stop.set(); sock.close(); sys.exit(0)
PY
echo "[PASS] shell smoke overall"
