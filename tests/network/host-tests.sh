#!/usr/bin/env bash
# HTTP tests from Linux/WSL host -> QEMU guest (user networking).
set -eu

GUEST_IP="${1:-10.0.2.15}"
HTTP_PORT="${2:-8080}"
BASE="http://${GUEST_IP}:${HTTP_PORT}"
VERBOSE="${MYOS_VERBOSE:-0}"
CURL_FMT='connect:%{time_connect}s total:%{time_total}s size:%{size_download}B code:%{http_code}'

PASS=0
FAIL=0
SKIP=0
START_TS=$(date +%s)

pass() { echo "[PASS] $*"; PASS=$((PASS + 1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL + 1)); }
skip() { echo "[SKIP] $*"; SKIP=$((SKIP + 1)); }

curl_stat() {
  local url="$1"
  curl -s -o /dev/null -w "${CURL_FMT}" --max-time 8 "${url}" 2>/dev/null || echo "code:000"
}

curl_diag() {
  local url="$1"
  echo "[--] curl -v ${url} (diagnostic)"
  curl -v --max-time 8 -o /dev/null "${url}" 2>&1 | tail -30 || true
}

command -v curl >/dev/null || { echo "[FAIL] curl required"; exit 1; }

echo "============================================"
echo " KnitOS network host tests"
echo " Target: ${BASE}"
echo " Time:   $(date '+%Y-%m-%d %H:%M:%S')"
echo " Verbose: ${VERBOSE}"
echo "============================================"
echo

if [[ "${GUEST_IP}" == "127.0.0.1" ]]; then
  echo "[INFO] 127.0.0.1 = QEMU hostfwd (host -> guest :${HTTP_PORT})"
elif [[ "${GUEST_IP}" == "10.0.2.15" ]]; then
  echo "[INFO] 10.0.2.15 = guest IP on QEMU user network (may need routing from WSL)"
fi
echo

echo "[--] Reachability ping ${GUEST_IP} ..."
t0=$(date +%s%N 2>/dev/null || echo 0)
if ping -c 1 -W 1 "${GUEST_IP}" >/dev/null 2>&1; then
  pass "Ping ${GUEST_IP}"
else
  skip "Guest not pingable (ICMP often blocked in QEMU user net)"
fi

echo "[--] TCP probe ${GUEST_IP}:${HTTP_PORT} ..."
if (echo >/dev/tcp/"${GUEST_IP}"/"${HTTP_PORT}") 2>/dev/null; then
  pass "TCP port ${HTTP_PORT} open"
else
  fail "TCP port ${HTTP_PORT} closed or unreachable"
  if [[ "$VERBOSE" == "1" ]]; then
    curl_diag "${BASE}/"
  fi
fi

echo "[--] GET / (expect 200 + KnitOS + text/html) ..."
code=$(curl -s -o /tmp/myos_index.html -w "%{http_code}" --max-time 8 "${BASE}/" || true)
stat=$(curl_stat "${BASE}/")
size=$(wc -c < /tmp/myos_index.html 2>/dev/null | tr -d ' ' || echo 0)
ctype=$(curl -sI --max-time 5 "${BASE}/" | grep -i '^Content-Type:' | tr -d '\r' || true)
if [[ "$code" == "200" ]] && grep -qi KnitOS /tmp/myos_index.html; then
  pass "GET / -> ${code} (${size} bytes) [${stat}]"
  if echo "$ctype" | grep -qi 'text/html'; then
    pass "Content-Type text/html"
  else
    fail "Content-Type missing or wrong: ${ctype:-none}"
  fi
else
  fail "GET / -> ${code} size=${size} [${stat}]"
  if [[ "$VERBOSE" == "1" ]]; then
    curl_diag "${BASE}/"
    head -5 /tmp/myos_index.html 2>/dev/null || true
  fi
fi

echo "[--] GET /test.txt (expect hello-from-guest) ..."
code=$(curl -s -o /tmp/myos_test.txt -w "%{http_code}" --max-time 8 "${BASE}/test.txt" || true)
stat=$(curl_stat "${BASE}/test.txt")
if [[ "$code" == "200" ]] && grep -q hello-from-guest /tmp/myos_test.txt; then
  pass "GET /test.txt -> ${code} [${stat}]"
elif [[ "$code" == "404" ]]; then
  skip "/test.txt missing (autotest should create /www/test.txt)"
else
  fail "GET /test.txt -> ${code} [${stat}]"
fi

echo "[--] HEAD / (expect 200, no body) ..."
headers=$(curl -sI --max-time 8 "${BASE}/" || true)
if echo "$headers" | grep -qE "HTTP/1\.[01] 200"; then
  pass "HEAD / status 200"
  if echo "$headers" | grep -qi 'Content-Length:'; then
    pass "HEAD / Content-Length present"
  else
    fail "HEAD / missing Content-Length"
  fi
else
  fail "HEAD / bad status"
  if [[ "$VERBOSE" == "1" ]]; then
    echo "$headers" | head -15
  fi
fi

echo "[--] OPTIONS / (expect 204 + Allow) ..."
code=$(curl -s -o /dev/null -w "%{http_code}" -X OPTIONS --max-time 8 "${BASE}/" || true)
allow=$(curl -sI -X OPTIONS --max-time 8 "${BASE}/" | grep -i '^Allow:' | tr -d '\r' || true)
if [[ "$code" == "204" ]]; then
  pass "OPTIONS / -> 204"
  if [[ -n "$allow" ]]; then
    pass "OPTIONS Allow header (${allow})"
  else
    fail "OPTIONS missing Allow header"
  fi
else
  fail "OPTIONS / -> ${code}"
fi

echo "[--] GET /no-such-file (expect 404) ..."
code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 8 "${BASE}/no-such-file" || true)
if [[ "$code" == "404" ]]; then
  pass "GET /no-such-file -> 404"
else
  fail "GET missing -> ${code}"
fi

echo "[--] Keep-Alive (two GETs on one connection) ..."
if curl -sf --http1.1 --max-time 10 "${BASE}/" "${BASE}/test.txt" -o /tmp/myos_ka.html >/dev/null 2>&1; then
  pass "Keep-Alive multi-GET"
else
  skip "Keep-Alive multi-GET (server may close after first response)"
fi

echo "[--] POST /api/echo (expect body echo) ..."
post_body="myos-autotest-post"
post_resp=$(curl -s -X POST -H "Expect:" --data-binary "${post_body}" --max-time 8 "${BASE}/api/echo" || true)
if [[ "$post_resp" == "${post_body}" ]]; then
  pass "POST /api/echo echoes body"
else
  fail "POST /api/echo -> '${post_resp}'"
fi

echo "[--] PUT /put-test.txt (expect 201) ..."
put_code=$(curl -s -o /dev/null -w "%{http_code}" -X PUT -H "Expect:" --data-binary "put-by-host" --max-time 8 "${BASE}/put-test.txt" || true)
if [[ "$put_code" == "201" ]]; then
  pass "PUT /put-test.txt -> 201"
else
  fail "PUT /put-test.txt -> ${put_code}"
fi

echo "[--] GET /put-test.txt (verify PUT) ..."
get_put=$(curl -s --max-time 8 "${BASE}/put-test.txt" || true)
if [[ "$get_put" == "put-by-host" ]]; then
  pass "GET /put-test.txt content after PUT"
else
  fail "GET /put-test.txt -> '${get_put}'"
fi

echo "[--] gzip Accept-Encoding ..."
if curl -sf -H "Accept-Encoding: gzip" --compressed --max-time 10 "${BASE}/" -o /tmp/myos_gz.html 2>/dev/null; then
  if grep -qi KnitOS /tmp/myos_gz.html; then
    pass "gzip response decompresses to KnitOS HTML"
  else
    fail "gzip body missing KnitOS"
  fi
else
  hdr=$(curl -sI -H "Accept-Encoding: gzip" --max-time 8 "${BASE}/" || true)
  if echo "$hdr" | grep -qi 'Content-Encoding: gzip'; then
    pass "Content-Encoding: gzip header present"
  else
    fail "gzip not supported or transfer failed"
  fi
fi

echo "[--] GET /api/ports (port table / owner) ..."
ports_tbl=$(curl -s --max-time 8 "${BASE}/api/ports" || true)
if echo "$ports_tbl" | grep -q "httpd"; then
  pass "/api/ports lists httpd"
else
  fail "/api/ports missing httpd — got: $(echo "$ports_tbl" | head -c 200)"
fi
if echo "$ports_tbl" | grep -q ":${HTTP_PORT}"; then
  pass "/api/ports shows listen port ${HTTP_PORT}"
else
  fail "/api/ports missing :${HTTP_PORT}"
fi
if echo "$ports_tbl" | grep -q "LISTEN"; then
  pass "/api/ports has LISTEN state"
else
  fail "/api/ports missing LISTEN"
fi
if echo "$ports_tbl" | grep -q "dhcpd"; then
  pass "/api/ports lists dhcpd"
else
  fail "/api/ports missing dhcpd"
fi

echo "[--] GET /api/access-log (request logging) ..."
access_log=$(curl -s --max-time 8 "${BASE}/api/access-log" || true)
if echo "$access_log" | grep -q "POST /api/echo"; then
  pass "access log contains POST /api/echo"
else
  fail "access log missing POST entry"
fi
if echo "$access_log" | grep -q "PUT /put-test.txt"; then
  pass "access log contains PUT"
else
  fail "access log missing PUT entry"
fi

echo "[--] Server + Connection headers ..."
hdr=$(curl -sI --max-time 8 "${BASE}/" || true)
if echo "$hdr" | grep -qi "Server: KnitOS-HTTP/1.1"; then
  pass "Server: KnitOS-HTTP/1.1"
else
  fail "Server header missing"
fi
if echo "$hdr" | grep -qi 'Connection:'; then
  pass "Connection header present"
else
  skip "Connection header not advertised"
fi

ELAPSED=$(( $(date +%s) - START_TS ))
echo
echo "============================================"
echo " Results: PASS=${PASS}  FAIL=${FAIL}  SKIP=${SKIP}  (${ELAPSED}s)"
echo " Target:  ${BASE}"
if [[ "$FAIL" -eq 0 ]]; then
  echo " Status:  ALL REQUIRED TESTS PASSED"
else
  echo " Status:  FAILED (${FAIL} case(s))"
fi
echo "============================================"
[[ "$FAIL" -eq 0 ]]
