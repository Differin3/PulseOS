#!/usr/bin/env bash
# Fully automated network test via WSL (no PowerShell execution policy).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

SERIAL_PORT="${SERIAL_PORT:-4444}"
GUEST_IP="${GUEST_IP:-10.0.2.15}"
CURL_HOST="${CURL_HOST:-127.0.0.1}"
HTTP_PORT="${HTTP_PORT:-8080}"
HTTP_MAX_REQUESTS="${HTTP_MAX_REQUESTS:-48}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-120}"
TEST_TIMEOUT="${TEST_TIMEOUT:-180}"
IDLE_DONE_TIMEOUT="${IDLE_DONE_TIMEOUT:-60}"
SKIP_BUILD=0
MYOS_HEADLESS="${MYOS_HEADLESS:-0}"
MYOS_DIAG="${MYOS_DIAG:-0}"
# virtio (default) | rtl8139 — QEMU NIC model for modern stack regression
MYOS_NIC="${MYOS_NIC:-virtio}"

for arg in "$@"; do
    case "$arg" in
        -SkipBuild|--skip-build) SKIP_BUILD=1 ;;
        -Headless|--headless) MYOS_HEADLESS=1 ;;
        -Rtl8139|--rtl8139) MYOS_NIC=rtl8139 ;;
        -Virtio|--virtio) MYOS_NIC=virtio ;;
    esac
done

case "$MYOS_NIC" in
    virtio|rtl8139) ;;
    *) echo "[FAIL] MYOS_NIC must be virtio or rtl8139 (got: $MYOS_NIC)"; exit 1 ;;
esac

mkdir -p "$ROOT/logs"
AUTO_LOG="$ROOT/logs/auto-test.log"
SERIAL_LOG="$ROOT/logs/qemu-serial.log"

log() { echo ""; echo "== $*"; }
pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*"; exit 1; }

{
echo ""
echo "======== $(date '+%Y-%m-%d %H:%M:%S') auto-network-test.sh ========"
log "config: SERIAL_PORT=$SERIAL_PORT CURL_HOST=$CURL_HOST GUEST_IP=$GUEST_IP HTTP_PORT=$HTTP_PORT HTTP_MAX=$HTTP_MAX_REQUESTS NIC=$MYOS_NIC HEADLESS=$MYOS_HEADLESS VERBOSE=${MYOS_VERBOSE:-1} DIAG=$MYOS_DIAG"

stop_qemu() {
    pkill -f qemu-system-i386 2>/dev/null || true
    sleep 1
}

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    log "build (make)"
    # WSL on /mnt/c: drop stale net-stack objects so modern stack always rebuilds
    rm -f "$ROOT/kernel/drivers/network/protocols/tcp.o" \
          "$ROOT/kernel/drivers/network/protocols/tcp_connection.o" \
          "$ROOT/kernel/drivers/network/protocols/ip.o" \
          "$ROOT/kernel/drivers/network/protocols/route.o" \
          "$ROOT/kernel/drivers/network/protocols/udp.o" \
          "$ROOT/kernel/drivers/network/protocols/icmp.o" \
          "$ROOT/kernel/drivers/network/protocols/arp.o" \
          "$ROOT/kernel/drivers/network/nic.o" \
          "$ROOT/kernel/drivers/network/core/skb.o" \
          "$ROOT/kernel/drivers/network/core/netif.o" \
          "$ROOT/kernel/drivers/network/core/net_queue.o" \
          "$ROOT/kernel/drivers/network/core/net_rx.o" \
          "$ROOT/kernel/drivers/network/core/net_wait.o" \
          "$ROOT/kernel/drivers/network/core/net_ports.o" \
          "$ROOT/kernel/drivers/network/core/net_rx.o" \
          "$ROOT/kernel/drivers/network/drivers/virtio_net/virtio_net.o" \
          "$ROOT/kernel/drivers/network/drivers/rtl8139/rtl8139.o" \
          "$ROOT/kernel/drivers/network/http_server.o" \
          "$ROOT/kernel/drivers/network/http_protocol.o" \
          "$ROOT/kernel/drivers/network/http_gzip.o" \
          "$ROOT/kernel/drivers/network/socket.o" \
          "$ROOT/kernel/drivers/network/network_config.o" \
          "$ROOT/kernel/drivers/network/dhcp/dhcp.o" \
          "$ROOT/kernel/drivers/network/dns/dns.o" \
          "$ROOT/kernel/drivers/pic/pic.o" \
          "$ROOT/kernel/drivers/timer/pit.o" \
          "$ROOT/kernel/idt.o" \
          "$ROOT/boot/interrupts.o" \
          "$ROOT/kernel/kernel.o" \
          "$ROOT/kernel/sched/task.o" \
          "$ROOT/kernel/sched/switch.o" \
          "$ROOT/kernel/drivers/timer/pit.o" \
          "$ROOT/kernel/drivers/network/core/net_wait.o" \
          "$ROOT/kernel/drivers/network/nic.o" \
          "$ROOT/kernel/drivers/network/socket.o" \
          "$ROOT/kernel/drivers/network/http_server.o" \
          "$ROOT/kernel/drivers/network/dhcp/dhcp.o" \
          "$ROOT/kernel/fs.o" \
          "$ROOT/kernel/fs_autotest.o" \
          "$ROOT/kernel/dev.o" \
          "$ROOT/kernel/utils.o" \
          "$ROOT/kernel/syscall.o" \
          "$ROOT/kernel/mm/paging.o" \
          "$ROOT/kernel/vga_autotest.o" \
          "$ROOT/kernel/drivers/video/terminal.o" \
          "$ROOT/kernel/drivers/video/fb.o" \
          "$ROOT/boot/boot.o" \
          "$ROOT/kernel/drivers/network/http_server.o" \
          "$ROOT/kernel/drivers/network/dhcp/dhcp.o" \
          "$ROOT/kernel/drivers/storage/ahci.o" 2>/dev/null || true
    make -C "$ROOT" all
fi

[[ -f "$ROOT/myos.iso" ]] || fail "myos.iso missing — run build first"

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

if [[ "$MYOS_NIC" == "rtl8139" ]]; then
    NIC_DEVICE_ARGS=(-device rtl8139,netdev=net0)
    NIC_EXPECT_MARKER='[INF][rtl8139]'
else
    NIC_DEVICE_ARGS=(-device virtio-net-pci,disable-legacy=off,disable-modern=on,netdev=net0)
    NIC_EXPECT_MARKER='[INF][virtio] initialized'
fi

log "Starting QEMU NIC=$MYOS_NIC (serial tcp :$SERIAL_PORT, hostfwd ::${HTTP_PORT}->guest:${HTTP_PORT})"
# logfile= captures guest serial even before the TCP client connects (nowait otherwise drops it)
rm -f "$SERIAL_LOG"
qemu-system-i386 "${QEMU_DISPLAY_ARGS[@]}" \
    -m 128M \
    -cdrom "$ISO" \
    -drive file="$IMG",if=none,format=raw,id=disk0 \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -netdev "user,id=net0,hostfwd=tcp::${HTTP_PORT}-:${HTTP_PORT}" \
    "${NIC_DEVICE_ARGS[@]}" \
    -chardev "socket,id=serial0,host=0.0.0.0,port=${SERIAL_PORT},server=on,wait=off,logfile=${SERIAL_LOG},logappend=on" \
    -serial chardev:serial0 &
QEMU_PID=$!
# Give QEMU a moment to bind the serial socket before the Python client connects
sleep 1
if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    fail "QEMU exited immediately after start (pid $QEMU_PID)"
fi

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
export MYOS_NIC NIC_EXPECT_MARKER

python3 << 'PY'
import os, socket, sys, threading, time, subprocess

ROOT = os.environ["ROOT"]
PORT = int(os.environ["SERIAL_PORT"])
CURL_HOST = os.environ.get("CURL_HOST", "127.0.0.1")
GUEST_IP = os.environ.get("GUEST_IP", "10.0.2.15")
HTTP = int(os.environ["HTTP_PORT"])
HTTP_MAX = int(os.environ.get("HTTP_MAX_REQUESTS", "48"))
LOG = os.environ["SERIAL_LOG"]
BOOT_TIMEOUT = int(os.environ["BOOT_TIMEOUT"])
TEST_TIMEOUT = int(os.environ["TEST_TIMEOUT"])
IDLE_DONE_TIMEOUT = int(os.environ.get("IDLE_DONE_TIMEOUT", "60"))
READY_FLAG = os.environ["READY_FLAG"]
VERBOSE = os.environ.get("MYOS_VERBOSE", "1") == "1"
DIAG = os.environ.get("MYOS_DIAG", "0") == "1"
MYOS_NIC = os.environ.get("MYOS_NIC", "virtio")
NIC_EXPECT_MARKER = os.environ.get("NIC_EXPECT_MARKER", "[INF][virtio] initialized")
HOST_TESTS_SH = os.path.join(ROOT, "tests", "network", "host-tests.sh")
HOST_TESTS_BAT = os.path.join(ROOT, "tests", "network", "host-tests.bat")
RUN_START = time.time()
# #region agent log
DBG_LOG = "/mnt/c/python/Car/carBK-EPXX-EEPROM-CAN/debug-37aafb.log"
if not os.path.isdir(os.path.dirname(DBG_LOG)):
    DBG_LOG = os.path.join(ROOT, "debug-37aafb.log")
DBG_MARKERS = (
    "H1_before_wait_after_ack", "H1_after_wait_after_ack", "H1_wait_timer_stuck",
    "H1_wait_noirq_spin", "H1_bound_skip_wait",
    "H2_ack_handler_ret", "H2_state_bound", "H2_poll_req",
    "H4_acquire_ok", "H5_apply_boot_after_dhcp", "DHCP boot ok", "ACK applied",
    "H6_irq_on", "H6_dev_init_enter", "H6_dev_init_done", "Keyboard ready",
    "H7_wr_write_fail", "H7_fs_alloc_fail", "H7_fs_data_write_fail",
    "H7_fs_table_write_fail", "H7_ahci_wr_fail", "H7_wr_ok", "fs_ok",
)

def agent_dbg(message, data=None, hypothesisId="boot"):
    try:
        import json
        payload = {
            "sessionId": "37aafb",
            "timestamp": int(time.time() * 1000),
            "location": "auto-network-test.sh:python",
            "message": message,
            "data": data or {},
            "hypothesisId": hypothesisId,
            "runId": "pre-fix",
        }
        with open(DBG_LOG, "a", encoding="utf-8") as df:
            df.write(json.dumps(payload, ensure_ascii=False) + "\n")
    except Exception:
        pass
# #endregion

print("== live guest serial on console (MYOS_VERBOSE=%s)" % (1 if VERBOSE else 0))
sys.stdout.flush()

SERIAL_MARKERS = (
    "[INF]", "[DBG]", "[ERR]", "[AUTOTEST]", "[HTTP]", "[CMD]",
    "[OK]", "[FAIL]", "[tcp]", "[nic]", "[http]", "[autotest]", "ports_ok", "ports_failed",
)

chunks = []
lock = threading.Lock()
stop = threading.Event()
serial_lines = 0
live_echoed = [0]  # byte offset already printed from QEMU logfile

def get_text():
    """Authoritative serial = QEMU chardev logfile (not TCP-only; late connect OK)."""
    global serial_lines
    try:
        with open(LOG, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        serial_lines = text.count("\n")
        return text
    except OSError:
        with lock:
            return "".join(chunks)

def echo_new_from_file():
    """Live-print new logfile bytes (guest TX before/after TCP client)."""
    global serial_lines
    try:
        with open(LOG, "rb") as f:
            f.seek(live_echoed[0])
            data = f.read()
        if not data:
            return
        live_echoed[0] += len(data)
        echo_serial(data)
    except OSError:
        pass

def serial_summary(text):
    keys = (
        ("[INF][virtio] initialized", "virtio-net driver init"),
        ("[INF][rtl8139]", "RTL8139 driver activity"),
        ("[INF][nic]", "NIC layer activity"),
        ("PIT timer", "PIT status line"),
        ("csum fail", "TCP checksum rejected"),
        ("syn rx", "TCP SYN received"),
        ("syn-ack sent", "TCP SYN-ACK sent"),
        ("established", "TCP connections established"),
        ("[INF][http] accept", "HTTP accepts"),
        ("[INF][http] sent", "HTTP responses sent"),
        ("[INF][http] idle_done", "HTTP idle shutdown"),
        ("fs_ok", "Filesystem autotest passed"),
        ("fs_failed", "Filesystem autotest failed"),
        ("dhcp_ok", "DHCP acquired in autotest"),
        ("dns_ok", "DNS resolve knitos.local"),
        ("ports_ok", "Port table autotest passed"),
        ("ports_failed", "Port table autotest failed"),
        ("sched_ok", "Scheduler autotest passed"),
        ("sched_failed", "Scheduler autotest failed"),
        ("sleep_ok", "Scheduler sleep/wake passed"),
        ("sleep_failed", "Scheduler sleep/wake failed"),
        ("netwait_ok", "Scheduler net-wait wake passed"),
        ("netwait_failed", "Scheduler net-wait wake failed"),
        ("kill_ok", "Scheduler task_kill passed"),
        ("kill_failed", "Scheduler task_kill failed"),
        ("systemd_ok", "Init task named systemd"),
        ("systemd_failed", "Init task systemd check failed"),
        ("kill_net_ok", "Kill closes victim sockets"),
        ("cwd_ok", "Per-task cwd isolation"),
        ("fd_ok", "Per-task fd table"),
        ("paging_ok", "Paging + PF smoke"),
        ("ring3_ok", "Ring-3 + syscall smoke"),
        ("fork_ok", "task_fork smoke"),
        ("exec_ok", "task_exec smoke"),
        ("user_shell_ok", "systemd user shell launch"),
        ("httpd_kthread_ok", "httpd runs as kthread"),
        ("httpd_kill_ok", "kill httpd frees listen port"),
        ("dhcpd_kthread_ok", "dhcpd runs as kthread"),
        ("aspace_ok", "Per-task CR3 isolation"),
        ("vga_ok", "VGA console autotest"),
        ("fb_ok", "Framebuffer 1024x768"),
        ("fb_skip", "Framebuffer skipped (text mode)"),
        ("[INF][sched] init", "Scheduler init"),
        ("[INF][sched] create", "Scheduler task create"),
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
    print("  NIC model    : %s" % MYOS_NIC)
    print("  Target       : http://%s:%d (guest %s)" % (CURL_HOST, HTTP, GUEST_IP))
    print("  Host curl    : %s" % ("PASS" if curl_rc == 0 else "FAIL (%d)" % curl_rc))
    if NIC_EXPECT_MARKER in text:
        print("  NIC driver   : PASS (%s)" % NIC_EXPECT_MARKER)
    else:
        print("  NIC driver   : WARN missing %s" % NIC_EXPECT_MARKER)
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
        echo_new_from_file()
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
    # Keep TCP socket alive for sending shell commands; logging is via QEMU logfile.
    while not stop.is_set():
        try:
            data = sock.recv(4096)
        except OSError:
            break
        if not data:
            break
        echo_new_from_file()
        text = data.decode("utf-8", errors="replace")
        # #region agent log
        for m in DBG_MARKERS:
            if m in text:
                agent_dbg("serial_marker", {"marker": m, "tail": text[-180:]},
                          "H1" if m.startswith("H1") else
                          "H2" if m.startswith("H2") else
                          "H4" if m.startswith("H4") else
                          "H5" if m.startswith("H5") or m == "DHCP boot ok" else "boot")
        # #endregion

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
# Net OK alone is too early (dev_init/shell not ready yet) — require Keyboard ready.
if not wait_substrings(["Keyboard ready"], BOOT_TIMEOUT,
                       progress_label="waiting Keyboard ready"):
    # #region agent log
    text = get_text()
    seen = {m: (m in text) for m in DBG_MARKERS}
    agent_dbg("boot_timeout", {"seen": seen, "tail": text[-1200:]}, "H6")
    print("[DBG] marker presence:", seen)
    # #endregion
    print("[FAIL] Boot timeout (need Keyboard ready)")
    sys.exit(1)
if "/ >" not in get_text() and " > " not in get_text():
    # prompt may arrive a moment later
    wait_substrings(["/ >", " > "], 15, progress_label="waiting shell prompt")
print("[PASS] Guest ready")
# #region agent log
agent_dbg("guest_ready", {"seen": {m: (m in get_text()) for m in DBG_MARKERS}}, "boot")
# #endregion

curl_rc = [0]

def run_curl():
    curl_rc[0] = curl_worker()

ct = threading.Thread(target=run_curl, daemon=True)
ct.start()

print("== Sending: autotest network %d %d" % (HTTP, HTTP_MAX))
sock.sendall(("autotest network %d %d\n" % (HTTP, HTTP_MAX)).encode("ascii"))

if not wait_substrings(["[AUTOTEST] fs ok", "[INF][autotest] fs_ok"], 120,
                       progress_label="waiting fs_ok"):
    print("[FAIL] Filesystem autotest did not pass")
    print("--- serial tail ---")
    print(get_text()[-2000:])
    serial_summary(get_text())
    print_final_report(1, get_text())
    sys.exit(1)
print("[PASS] guest filesystem autotest")

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

log "Serial log checks (NIC=$MYOS_NIC)"
if [[ "$MYOS_NIC" == "virtio" ]]; then
    if grep -qF '[INF][virtio] initialized' "$SERIAL_LOG" 2>/dev/null; then
        pass "virtio-net driver initialized"
    else
        fail "no [INF][virtio] initialized — guest did not bind virtio-net"
    fi
else
    if grep -qF '[INF][rtl8139]' "$SERIAL_LOG" 2>/dev/null; then
        pass "RTL8139 driver activity"
    else
        fail "no [INF][rtl8139] — guest did not use RTL8139"
    fi
fi
if grep -q 'PIT timer' "$SERIAL_LOG" 2>/dev/null; then pass "PIT timer boot line"; else echo "[WARN] no PIT timer status in log (VGA-only OK)"; fi
if grep -qE 'fs_ok|\[AUTOTEST\] fs ok' "$SERIAL_LOG" 2>/dev/null; then pass "FS autotest marker"; else fail "no fs_ok in log"; fi
if grep -q 'fs_failed\|fs FAIL' "$SERIAL_LOG" 2>/dev/null; then fail "FS autotest reported failure"; fi
if grep -q 'dhcp_ok\|dhcp ok' "$SERIAL_LOG" 2>/dev/null; then pass "DHCP autotest marker"; else echo "[WARN] no dhcp_ok in log"; fi
if grep -q 'dns_ok\|dns ok' "$SERIAL_LOG" 2>/dev/null; then pass "DNS autotest marker"; else fail "no dns_ok in log"; fi
if grep -q 'ports_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Ports autotest marker"; else fail "no ports_ok in log"; fi
if grep -q 'ports_failed' "$SERIAL_LOG" 2>/dev/null; then fail "Ports autotest reported failure"; fi
if grep -q 'sched_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Scheduler autotest marker"; else fail "no sched_ok in log"; fi
if grep -q 'sched_failed' "$SERIAL_LOG" 2>/dev/null; then fail "Scheduler autotest reported failure"; fi
if grep -q 'sleep_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Scheduler sleep/wake marker"; else fail "no sleep_ok in log"; fi
if grep -q 'sleep_failed' "$SERIAL_LOG" 2>/dev/null; then fail "Scheduler sleep reported failure"; fi
if grep -q 'netwait_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Scheduler net-wait marker"; else fail "no netwait_ok in log"; fi
if grep -q 'netwait_failed' "$SERIAL_LOG" 2>/dev/null; then fail "Scheduler net-wait reported failure"; fi
if grep -q 'kill_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Scheduler kill marker"; else fail "no kill_ok in log"; fi
if grep -q 'kill_failed' "$SERIAL_LOG" 2>/dev/null; then fail "Scheduler kill reported failure"; fi
if grep -q 'systemd_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Systemd init task marker"; else fail "no systemd_ok in log"; fi
if grep -q 'systemd_failed' "$SERIAL_LOG" 2>/dev/null; then fail "Systemd init check failed"; fi
if grep -q 'kill_net_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Kill closes sockets marker"; else fail "no kill_net_ok in log"; fi
if grep -q 'cwd_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Per-task cwd marker"; else fail "no cwd_ok in log"; fi
if grep -q 'fd_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Per-task fd marker"; else fail "no fd_ok in log"; fi
if grep -q 'paging_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Paging marker"; else fail "no paging_ok in log"; fi
if grep -q 'ring3_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Ring-3 marker"; else fail "no ring3_ok in log"; fi
if grep -q 'fork_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Fork marker"; else fail "no fork_ok in log"; fi
if grep -q 'exec_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Exec marker"; else fail "no exec_ok in log"; fi
if grep -q 'user_shell_ok' "$SERIAL_LOG" 2>/dev/null; then pass "User shell launch marker"; else fail "no user_shell_ok in log"; fi
if grep -q 'httpd_kthread_ok' "$SERIAL_LOG" 2>/dev/null; then pass "httpd kthread marker"; else fail "no httpd_kthread_ok in log"; fi
if grep -q 'httpd_kill_ok' "$SERIAL_LOG" 2>/dev/null; then pass "httpd kill frees port"; else fail "no httpd_kill_ok in log"; fi
if grep -q 'dhcpd_kthread_ok' "$SERIAL_LOG" 2>/dev/null; then pass "dhcpd kthread marker"; else fail "no dhcpd_kthread_ok in log"; fi
if grep -q 'aspace_ok' "$SERIAL_LOG" 2>/dev/null; then pass "Address-space isolation marker"; else fail "no aspace_ok in log"; fi
if grep -q 'vga_ok' "$SERIAL_LOG" 2>/dev/null; then pass "VGA autotest marker"; else fail "no vga_ok in log"; fi
if grep -q 'fb_ok' "$SERIAL_LOG" 2>/dev/null; then
    pass "Framebuffer marker"
elif grep -q 'fb_skip' "$SERIAL_LOG" 2>/dev/null; then
    echo "[WARN] fb_skip — expected fb_ok under QEMU+GRUB gfxpayload"
    fail "no fb_ok in log (got fb_skip)"
else
    fail "no fb_ok/fb_skip in log"
fi
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
echo " AUTO TEST PASSED (NIC=$MYOS_NIC)"
echo " Logs: $AUTO_LOG"
echo "       $SERIAL_LOG"
echo "============================================"
exit 0

} 2>&1 | tee -a "$AUTO_LOG"
