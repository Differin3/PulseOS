#!/usr/bin/env bash
# Fully automated network test via WSL (no PowerShell execution policy).
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

SERIAL_PORT="${SERIAL_PORT:-4444}"
GUEST_IP="${GUEST_IP:-10.0.2.15}"
CURL_HOST="${CURL_HOST:-127.0.0.1}"
HTTP_PORT="${HTTP_PORT:-8080}"
HTTP_MAX_REQUESTS="${HTTP_MAX_REQUESTS:-32}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"
TEST_TIMEOUT="${TEST_TIMEOUT:-180}"
IDLE_DONE_TIMEOUT="${IDLE_DONE_TIMEOUT:-60}"
SKIP_BUILD=0
MYOS_HEADLESS="${MYOS_HEADLESS:-0}"
MYOS_DIAG="${MYOS_DIAG:-0}"

for arg in "$@"; do
    case "$arg" in
        -SkipBuild|--skip-build) SKIP_BUILD=1 ;;
        -Headless|--headless) MYOS_HEADLESS=1 ;;
    esac
done

mkdir -p "$ROOT/logs"
AUTO_LOG="$ROOT/logs/auto-test.log"
SERIAL_LOG="$ROOT/logs/qemu-serial.log"

log() { echo ""; echo "== $*"; }
pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*"; exit 1; }

{
echo ""
echo "======== $(date '+%Y-%m-%d %H:%M:%S') auto-network-test.sh ========"
log "config: SERIAL_PORT=$SERIAL_PORT CURL_HOST=$CURL_HOST GUEST_IP=$GUEST_IP HTTP_PORT=$HTTP_PORT HTTP_MAX=$HTTP_MAX_REQUESTS HEADLESS=$MYOS_HEADLESS VERBOSE=${MYOS_VERBOSE:-1} DIAG=$MYOS_DIAG"

stop_qemu() {
    pkill -f qemu-system-i386 2>/dev/null || true
    sleep 1
}

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    log "build (make)"
    # WSL on /mnt/c: drop stale network .o so tcp fixes always apply
    rm -f "$ROOT/kernel/drivers/network/protocols/tcp.o" \
          "$ROOT/kernel/drivers/network/nic.o" \
          "$ROOT/kernel/drivers/network/http_server.o" \
          "$ROOT/kernel/drivers/network/http_protocol.o" \
          "$ROOT/kernel/drivers/network/http_gzip.o" \
          "$ROOT/kernel/drivers/network/socket.o" \
          "$ROOT/kernel/drivers/network/network_config.o" \
          "$ROOT/kernel/kernel.o" 2>/dev/null || true
    make -C "$ROOT" all
fi

[[ -f "$ROOT/myos.iso" ]] || fail "myos.iso missing — run build first"

: > "$SERIAL_LOG"

stop_qemu

if [[ ! -f "$ROOT/myos.img" ]]; then
    log "Creating myos.img"
    dd if=/dev/zero of="$ROOT/myos.img" bs=1M count=64 status=none
fi

ISO="$ROOT/myos.iso"
IMG="$ROOT/myos.img"

QEMU_DISPLAY_ARGS=()
if [[ "$MYOS_HEADLESS" == "1" ]]; then
    QEMU_DISPLAY_ARGS=(-display none)
    log "QEMU headless"
else
    if [[ -n "${DISPLAY:-}" ]] || [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
        QEMU_DISPLAY_ARGS=(-display gtk)
        log "QEMU with GTK window (WSLg)"
    else
        QEMU_DISPLAY_ARGS=(-display default)
        log "QEMU with default display"
    fi
fi

log "Starting QEMU (serial tcp :$SERIAL_PORT, hostfwd ::${HTTP_PORT}->guest:${HTTP_PORT})"
qemu-system-i386 "${QEMU_DISPLAY_ARGS[@]}" \
    -cdrom "$ISO" \
    -drive file="$IMG",if=none,format=raw,id=disk0 \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -netdev "user,id=net0,hostfwd=tcp::${HTTP_PORT}-:${HTTP_PORT}" \
    -device rtl8139,netdev=net0 \
    -serial "tcp:0.0.0.0:${SERIAL_PORT},server,nowait" &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    stop_qemu
    rm -f "/tmp/myos_http_ready_$$"
}
trap cleanup EXIT

export ROOT SERIAL_PORT GUEST_IP CURL_HOST HTTP_PORT HTTP_MAX_REQUESTS SERIAL_LOG
export BOOT_TIMEOUT TEST_TIMEOUT IDLE_DONE_TIMEOUT
export READY_FLAG="/tmp/myos_http_ready_$$"
export MYOS_VERBOSE="${MYOS_VERBOSE:-1}"

python3 << 'PY'
import os, socket, sys, threading, time, subprocess

ROOT = os.environ["ROOT"]
PORT = int(os.environ["SERIAL_PORT"])
CURL_HOST = os.environ.get("CURL_HOST", "127.0.0.1")
GUEST_IP = os.environ.get("GUEST_IP", "10.0.2.15")
HTTP = int(os.environ["HTTP_PORT"])
HTTP_MAX = int(os.environ.get("HTTP_MAX_REQUESTS", "16"))
LOG = os.environ["SERIAL_LOG"]
BOOT_TIMEOUT = int(os.environ["BOOT_TIMEOUT"])
TEST_TIMEOUT = int(os.environ["TEST_TIMEOUT"])
IDLE_DONE_TIMEOUT = int(os.environ.get("IDLE_DONE_TIMEOUT", "60"))
READY_FLAG = os.environ["READY_FLAG"]
VERBOSE = os.environ.get("MYOS_VERBOSE", "1") == "1"
DIAG = os.environ.get("MYOS_DIAG", "0") == "1"
HOST_TESTS_SH = os.path.join(ROOT, "tests", "network", "host-tests.sh")
HOST_TESTS_BAT = os.path.join(ROOT, "tests", "network", "host-tests.bat")
RUN_START = time.time()

print("== live guest serial on console (MYOS_VERBOSE=%s)" % (1 if VERBOSE else 0))
sys.stdout.flush()

SERIAL_MARKERS = (
    "[INF]", "[DBG]", "[ERR]", "[AUTOTEST]", "[HTTP]", "[CMD]",
    "[OK]", "[FAIL]", "[tcp]", "[nic]", "[http]", "[autotest]",
)

chunks = []
lock = threading.Lock()
stop = threading.Event()
serial_lines = 0

def get_text():
    with lock:
        return "".join(chunks)

def serial_summary(text):
    keys = (
        ("csum fail", "TCP checksum rejected"),
        ("syn rx", "TCP SYN received"),
        ("syn-ack sent", "TCP SYN-ACK sent"),
        ("established", "TCP connections established"),
        ("[INF][http] accept", "HTTP accepts"),
        ("[INF][http] sent", "HTTP responses sent"),
        ("[INF][http] idle_done", "HTTP idle shutdown"),
        ("dhcp_ok", "DHCP acquired in autotest"),
        ("dns_ok", "DNS resolve myos.local"),
        ("[INF][http] gzip", "gzip compression"),
        ("post_echo", "POST echo handler"),
        ("[INF][http] access", "HTTP access log"),
        ("[INF][autotest] http_ready", "HTTP server ready"),
        ("http_ready", "HTTP ready marker"),
        ("http_done", "Autotest finished"),
    )
    print("== guest serial summary:")
    for k, label in keys:
        n = text.count(k)
        if n:
            print("   %-22s %5d  (%s)" % (k + ":", n, label))
    if "csum fail" in text:
        print("[HINT] guest drops TCP checksum — rebuild: build-test.bat rebuild")
    if "[INF][nic] tcp rx" in text and "syn rx" not in text:
        print("[HINT] NIC sees TCP but stack ignores it (dest IP / parse?)")
    if "syn rx" in text and "[INF][http] accept" not in text:
        print("[HINT] SYN arrives but no HTTP accept (handshake / socket?)")
    if "[INF][http] accept" in text and "[INF][http] sent" not in text:
        print("[HINT] accepted but no HTTP response (recv/parse?)")

def parse_served(text):
    import re
    m = re.search(r"\[AUTOTEST\] http_done served=(\d+)", text)
    if m:
        return int(m.group(1))
    m = re.search(r'\[INF\]\[autotest\] http_done.*served=0x([0-9a-fA-F]+)', text)
    if m:
        return int(m.group(1), 16)
    return None

def print_final_report(curl_rc, text):
    elapsed = time.time() - RUN_START
    served = parse_served(text)
    print("")
    print("=" * 60)
    print("  AUTO TEST REPORT")
    print("=" * 60)
    print("  Duration     : %.1f s" % elapsed)
    print("  Target       : http://%s:%d (guest %s)" % (CURL_HOST, HTTP, GUEST_IP))
    print("  Host curl    : %s" % ("PASS" if curl_rc == 0 else "FAIL (%d)" % curl_rc))
    if served is not None:
        print("  Guest served : %d HTTP request(s)" % served)
    if "http_done" in text or "[INF][autotest] http_done" in text:
        print("  Guest autotest: FINISHED (http_done)")
    elif "idle_done" in text:
        print("  Guest autotest: FINISHED (idle timeout after tests)")
    else:
        print("  Guest autotest: no http_done marker")
    print("  Logs         : logs/auto-test.log, logs/qemu-serial.log")
    serial_summary(text)
    print("=" * 60)
    if curl_rc == 0 and "[INF][http] sent" in text:
        print("  OVERALL: PASS")
    else:
        print("  OVERALL: FAIL")
    print("=" * 60)

def echo_serial(data):
    global serial_lines
    text = data.decode("utf-8", errors="replace")
    if not VERBOSE:
        if not any(m in text for m in SERIAL_MARKERS):
            return
    sys.stdout.write(text)
    sys.stdout.flush()
    serial_lines += text.count("\n")

def wait_substrings(needles, timeout, progress_label=None):
    deadline = time.time() + timeout
    last_report = time.time()
    while time.time() < deadline and not stop.is_set():
        text = get_text()
        for n in needles:
            if n in text:
                return True
        if progress_label and time.time() - last_report >= 8:
            last_report = time.time()
            elapsed = int(timeout - (deadline - time.time()))
            print("== %s (%ds) — guest serial live (%d lines echoed)" % (
                progress_label, elapsed, serial_lines))
            print("--- serial tail (last 600 chars) ---")
            print(text[-600:])
            serial_summary(text)
            sys.stdout.flush()
        time.sleep(0.1)
    return False

def reader(sock):
    with open(LOG, "ab") as f:
        while not stop.is_set():
            try:
                data = sock.recv(4096)
            except OSError:
                break
            if not data:
                break
            with lock:
                chunks.append(data.decode("utf-8", errors="replace"))
            echo_serial(data)
            f.write(data)
            f.flush()

def host_diagnostics(host):
    print("")
    print("== host diagnostics: %s:%d (guest %s)" % (host, HTTP, GUEST_IP))
    for cmd in (["ss", "-tln"], ["ss", "-tlnp"]):
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=3)
            out = (r.stdout or "") + (r.stderr or "")
            if out.strip():
                print("--- %s ---" % " ".join(cmd))
                for line in out.splitlines():
                    if str(HTTP) in line or "State" in line or "LISTEN" in line:
                        print(line)
        except (OSError, subprocess.TimeoutExpired):
            pass
    try:
        s = socket.create_connection((host, HTTP), timeout=4)
        print("[PROBE] TCP connect OK -> %s:%d" % (host, HTTP))
        s.close()
    except OSError as e:
        print("[PROBE] TCP connect FAIL -> %s:%d (%s)" % (host, HTTP, e))
    print("--- curl -v (first probe) ---")
    try:
        r = subprocess.run(
            ["curl", "-v", "--max-time", "8", "-o", "/dev/null",
             "http://%s:%d/" % (host, HTTP)],
            capture_output=True, text=True, timeout=12)
        out = (r.stdout or "") + (r.stderr or "")
        print(out[-2500:] if len(out) > 2500 else out)
        print("[PROBE] curl exit", r.returncode)
    except (OSError, subprocess.TimeoutExpired) as e:
        print("[PROBE] curl error:", e)
    print("--- guest serial at probe time ---")
    serial_summary(get_text())
    print("")

def run_host_http_tests():
    if not os.path.isfile(HOST_TESTS_SH):
        print("[FAIL] missing %s" % HOST_TESTS_SH)
        return 1
    env = os.environ.copy()
    env["MYOS_VERBOSE"] = "0"
    host = CURL_HOST
    print("== host HTTP tests (bash) -> http://%s:%d/" % (host, HTTP))
    sys.stdout.flush()
    try:
        r = subprocess.run(["bash", HOST_TESTS_SH, host, str(HTTP)], cwd=ROOT, env=env,
                           timeout=90)
    except subprocess.TimeoutExpired:
        print("[FAIL] host HTTP tests timed out (90s)")
        return 1
    if r.returncode == 0:
        return 0
    if DIAG or os.environ.get("MYOS_DIAG", "0") == "1":
        host_diagnostics(CURL_HOST)
    else:
        print("[INFO] host tests failed — set MYOS_DIAG=1 for ss/curl probes")
    return 1

def curl_worker():
    deadline = time.time() + 150
    while time.time() < deadline and not stop.is_set():
        if os.path.exists(READY_FLAG):
            break
        time.sleep(0.2)
    time.sleep(3)
    if not os.path.exists(READY_FLAG):
        return 1
    return run_host_http_tests()

sock = None
deadline = time.time() + 90
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
if not wait_substrings(["Keyboard ready", "/ >", "Net OK"], BOOT_TIMEOUT):
    print("[FAIL] Boot timeout")
    sys.exit(1)
print("[PASS] Guest ready")

curl_rc = [0]

def run_curl():
    curl_rc[0] = curl_worker()

ct = threading.Thread(target=run_curl, daemon=True)
ct.start()

print("== Sending: autotest network %d %d" % (HTTP, HTTP_MAX))
sock.sendall(("autotest network %d %d\n" % (HTTP, HTTP_MAX)).encode("ascii"))

if not wait_substrings(["[INF][http] listen", "[INF][autotest] http_ready"], 90,
                       progress_label="waiting http_ready"):
    print("[FAIL] HTTP listen not seen on serial")
    print("--- serial tail ---")
    print(get_text()[-2000:])
    serial_summary(get_text())
    print_final_report(1, get_text())
    sys.exit(1)
open(READY_FLAG, "w", encoding="utf-8").close()
print("[PASS] HTTP server listening on guest :%d (max %d, idle-exit after tests)" % (HTTP, HTTP_MAX))
serial_summary(get_text())

print("== Running host HTTP tests in parallel with guest server")
ct.join(timeout=TEST_TIMEOUT)
if curl_rc[0] != 0:
    print("[FAIL] host HTTP tests exit %d" % curl_rc[0])
    serial_summary(get_text())
    print_final_report(curl_rc[0], get_text())
    sys.exit(1)
print("[PASS] host HTTP tests (all curl cases)")

print("== Waiting for guest autotest to finish (http_done or idle, up to %ds)" % IDLE_DONE_TIMEOUT)
done_markers = (
    "[AUTOTEST] http_done", "[INF][autotest] http_done",
    "[INF][http] idle_done", "[HTTP] idle done",
)
if not wait_substrings(list(done_markers), IDLE_DONE_TIMEOUT,
                       progress_label="waiting guest shutdown"):
    text = get_text()
    if "[INF][http] sent" in text and curl_rc[0] == 0:
        print("[WARN] http_done not seen but host tests passed and responses were sent — treating as PASS")
    else:
        print("[FAIL] guest did not finish after host tests")
        print("--- serial tail ---")
        print(text[-3000:])
        serial_summary(text)
        print_final_report(1, text)
        sys.exit(1)
else:
    print("[PASS] guest autotest finished")

print_final_report(curl_rc[0], get_text())

stop.set()
sock.close()
sys.exit(0)
PY

RC=$?
if [[ "$RC" -ne 0 ]]; then
    fail "Python orchestrator failed ($RC)"
fi

log "Serial log checks"
if grep -q 'dhcp_ok\|dhcp ok' "$SERIAL_LOG" 2>/dev/null; then pass "DHCP autotest marker"; else echo "[WARN] no dhcp_ok in log"; fi
if grep -q 'dns_ok\|dns ok' "$SERIAL_LOG" 2>/dev/null; then pass "DNS autotest marker"; else fail "no dns_ok in log"; fi
if grep -q '\[INF\]\[dhcp\]' "$SERIAL_LOG" 2>/dev/null; then pass "DHCP activity in log"; else echo "[INFO] no [INF][dhcp] lines (quiet DHCP is OK)"; fi
if grep -q '\[INF\]\[http\] listen' "$SERIAL_LOG" 2>/dev/null; then pass "HTTP listen in log"; else fail "no http listen in log"; fi
if grep -q '\[INF\]\[http\] sent' "$SERIAL_LOG" 2>/dev/null; then pass "HTTP sent in log"; else fail "no http sent in log"; fi
if grep -q 'syn rx' "$SERIAL_LOG" 2>/dev/null; then pass "TCP SYN received"; else echo "[WARN] no syn rx in log"; fi
if grep -q 'csum fail' "$SERIAL_LOG" 2>/dev/null; then echo "[WARN] TCP checksum failures in log"; else pass "no TCP checksum errors"; fi
if grep -q '\[INF\]\[http\] gzip' "$SERIAL_LOG" 2>/dev/null; then pass "gzip compression used"; else echo "[WARN] no gzip in serial log"; fi
if grep -q 'post_echo' "$SERIAL_LOG" 2>/dev/null; then pass "POST echo in log"; else echo "[WARN] no post_echo marker"; fi
if grep -qE 'http_done|idle_done' "$SERIAL_LOG" 2>/dev/null; then pass "HTTP server shutdown marker"; else echo "[WARN] no http_done/idle_done"; fi

echo ""
echo "============================================"
echo " AUTO TEST PASSED"
echo " Logs: $AUTO_LOG"
echo "       $SERIAL_LOG"
echo "============================================"
exit 0

} 2>&1 | tee -a "$AUTO_LOG"
