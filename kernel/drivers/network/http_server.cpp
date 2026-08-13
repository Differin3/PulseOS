#include "http_server.h"
#include "http_protocol.h"
#include "http_gzip.h"
#include "socket.h"
#include "core/net_ports.h"
#include "protocols/ip.h"
#include "protocols/tcp.h"
#include "protocols/tcp_connection.h"
#include "nic.h"
#include "../../fs.h"
#include "serial_log.h"
#include "drivers/video/terminal.h"
#include "drivers/timer/pit.h"
#include "sched/task.h"
#include <stddef.h>
#include <stdint.h>

static void http_term_u32(uint32_t v) {
    char buf[12];
    int i = 0;
    if (v == 0) {
        terminal_putchar('0');
        return;
    }
    while (v > 0 && i < 11) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        terminal_putchar(buf[--i]);
    }
}

#define HTTP_REQ_MAX   2048
#define HTTP_PATH_MAX  192
#define HTTP_FILE_MAX  32768
#define HTTP_DOC_ROOT  "/www"
#define HTTP_CONN_MAX  16
#define HTTP_PENDING_MAX 512
#define HTTP_ACCESS_LOG "/var/log/http-access.log"
#define HTTP_ACCESS_MAX 8192

static size_t http_strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int http_send_all(int fd, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 1024) chunk = 1024;
        int n = socket_send(fd, p + off, chunk);
        if (n <= 0) return -1;
        off += (size_t)n;
        socket_service_network();
    }
    return 0;
}

static int http_send_response(int fd, int status, const char* content_type,
                              const void* body, size_t body_len,
                              bool is_http11, bool keep_alive, bool send_body,
                              const char* content_encoding) {
    char hdr[448];
    int hlen = http_format_response_header(hdr, sizeof(hdr), status, content_type,
                                           send_body ? body_len : 0, is_http11, keep_alive,
                                           content_encoding);
    if (hlen < 0) return -1;
    if (http_send_all(fd, hdr, (size_t)hlen) != 0) return -1;
    if (send_body && body_len > 0 && body && http_send_all(fd, body, body_len) != 0) return -1;
    return 0;
}

static bool http_path_safe(const char* path) {
    if (!path || path[0] != '/') return false;
    for (size_t i = 0; path[i]; i++) {
        if (path[i] == '.' && path[i + 1] == '.' && (path[i + 2] == '/' || path[i + 2] == 0)) {
            return false;
        }
    }
    return true;
}

static void http_build_fs_path(const char* url_path, char* out, size_t cap) {
    const char* req = url_path;
    if (!req[0]) req = "/";

    size_t o = 0;
    const char* root = HTTP_DOC_ROOT;
    while (*root && o + 1 < cap) out[o++] = *root++;
    if (req[0] == '/' && req[1] == 0) {
        const char* idx = "/index.html";
        while (*idx && o + 1 < cap) out[o++] = *idx++;
        out[o] = 0;
        return;
    }
    const char* p = req;
    while (*p && o + 1 < cap) out[o++] = *p++;
    if (o > 0 && out[o - 1] == '/') {
        const char* idx = "index.html";
        while (*idx && o + 1 < cap) out[o++] = *idx++;
    }
    out[o] = 0;
}

static bool http_path_eq(const char* path, const char* lit) {
    if (!path || !lit) return false;
    while (*lit) {
        if (*path != *lit) return false;
        path++;
        lit++;
    }
    return *path == 0;
}

static const char* http_method_name(int method) {
    switch (method) {
        case HTTP_METHOD_GET: return "GET";
        case HTTP_METHOD_HEAD: return "HEAD";
        case HTTP_METHOD_OPTIONS: return "OPTIONS";
        case HTTP_METHOD_POST: return "POST";
        case HTTP_METHOD_PUT: return "PUT";
        default: return "?";
    }
}

static void http_log_access(int method, const char* path, int status, uint32_t client_ip) {
    fs_create_dir("/var");
    fs_create_dir("/var/log");

    char line[160];
    int lp = 0;
    const char* mn = http_method_name(method);
    while (*mn && lp + 1 < (int)sizeof(line)) line[lp++] = *mn++;
    if (lp + 1 < (int)sizeof(line)) line[lp++] = ' ';
    for (const char* p = path; *p && lp + 1 < (int)sizeof(line); p++) line[lp++] = *p;
    if (lp + 1 < (int)sizeof(line)) line[lp++] = ' ';
    char stmp[8];
    int st = 0;
    int sv = status;
    if (sv == 0) { stmp[st++] = '0'; }
    else {
        char t[8];
        int ti = 0;
        while (sv > 0 && ti < 7) { t[ti++] = (char)('0' + (sv % 10)); sv /= 10; }
        while (ti > 0 && st < 7) stmp[st++] = t[--ti];
    }
    stmp[st] = 0;
    for (int i = 0; stmp[i] && lp + 1 < (int)sizeof(line); i++) line[lp++] = stmp[i];
    if (lp + 1 < (int)sizeof(line)) line[lp++] = ' ';
    char ipbuf[16];
    ip_format_address(client_ip, ipbuf, sizeof(ipbuf));
    for (int i = 0; ipbuf[i] && lp + 1 < (int)sizeof(line); i++) line[lp++] = ipbuf[i];
    if (lp + 2 < (int)sizeof(line)) { line[lp++] = '\n'; line[lp] = 0; }

    static char logbuf[HTTP_ACCESS_MAX];
    size_t old_len = 0;
    uint32_t fsize = 0;
    if (fs_open(HTTP_ACCESS_LOG, &fsize) == 0 && fsize > 0 && fsize < HTTP_ACCESS_MAX - 200) {
        if (fs_read(HTTP_ACCESS_LOG, logbuf, fsize) > 0) old_len = fsize;
    }
    size_t add = 0;
    while (line[add] && old_len + add + 1 < HTTP_ACCESS_MAX) {
        logbuf[old_len + add] = line[add];
        add++;
    }
    logbuf[old_len + add] = 0;
    if (old_len + add > HTTP_ACCESS_MAX - 512) {
        size_t keep = HTTP_ACCESS_MAX / 2;
        for (size_t i = 0; i < keep && logbuf[old_len + add - keep + i]; i++) {
            logbuf[i] = logbuf[old_len + add - keep + i];
        }
        logbuf[keep] = 0;
        old_len = keep;
        for (size_t i = 0; line[i] && old_len + i + 1 < HTTP_ACCESS_MAX; i++) {
            logbuf[old_len + i] = line[i];
        }
        old_len += add;
        logbuf[old_len] = 0;
    }
    fs_write(HTTP_ACCESS_LOG, logbuf, old_len + add);
    log_fmt3(LOG_INFO, "http", "access", "status", (uint32_t)status, "method", (uint32_t)method, "ip", client_ip);
}

static int http_find_hdr_end(const char* buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (int)(i + 4);
        }
    }
    return -1;
}

static void http_save_pending(int fd, char* pending, size_t* pending_len, size_t pending_cap) {
    if (!pending || !pending_len) return;
    size_t pl = *pending_len;
    while (pl < pending_cap && socket_recv_available(fd) > 0) {
        int r = socket_recv(fd, pending + pl, pending_cap - pl, 50);
        if (r <= 0) break;
        pl += (size_t)r;
    }
    *pending_len = pl;
}

static int http_recv_request(int fd, char* buf, size_t cap, char* body, size_t body_cap,
                             size_t* body_len_out, char* pending, size_t* pending_len,
                             size_t pending_cap, bool short_idle) {
    if (body_len_out) *body_len_out = 0;
    if (pending_len) *pending_len = 0;

    size_t total = 0;
    if (pending && pending_len && *pending_len > 0) {
        size_t take = *pending_len;
        if (take >= cap) take = cap - 1;
        for (size_t i = 0; i < take; i++) buf[i] = pending[i];
        total = take;
        size_t rem = *pending_len - take;
        for (size_t i = 0; i < rem && i < pending_cap; i++) pending[i] = pending[take + i];
        *pending_len = rem;
    }

    for (int drain = 0; drain < 16 && total + 1 < cap; drain++) {
        if (http_find_hdr_end(buf, total) >= 0) break;
        size_t avail = socket_recv_available(fd);
        if (avail == 0) break;
        size_t chunk = cap - total - 1;
        if (chunk > avail) chunk = avail;
        int n = socket_recv(fd, buf + total, chunk, 50);
        if (n <= 0) break;
        total += (size_t)n;
    }

    int max_rounds = short_idle ? 12 : 60;
    int recv_ms = short_idle ? 40 : 80;
    for (int round = 0; round < max_rounds && total + 1 < cap; round++) {
        if (http_find_hdr_end(buf, total) >= 0) break;
        if (short_idle && round > 1 && socket_recv_available(fd) == 0) {
            int n = socket_recv(fd, buf + total, cap - total - 1, 40);
            if (n <= 0) return -1;
            total += (size_t)n;
            continue;
        }
        int n = socket_recv(fd, buf + total, cap - total - 1, recv_ms);
        if (n < 0) return -1;
        if (n == 0) {
            socket_service_network();
            continue;
        }
        total += (size_t)n;
        buf[total] = 0;
    }

    int hdr_end = http_find_hdr_end(buf, total);
    if (hdr_end < 0) return total > 0 ? (int)total : -1;

    struct http_request_info pinfo;
    if (http_parse_request(buf, (size_t)hdr_end, &pinfo) != 0) {
        return (int)hdr_end;
    }

    if (pinfo.expect_continue) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        http_send_all(fd, cont, sizeof(cont) - 1);
    }

    size_t blen = 0;
    size_t inbuf = (total > (size_t)hdr_end) ? (total - (size_t)hdr_end) : 0;
    size_t body_in_buf = 0;

    if (body && body_cap > 0 && pinfo.content_length > 0) {
        body_in_buf = inbuf;
        if (body_in_buf > (size_t)pinfo.content_length) body_in_buf = (size_t)pinfo.content_length;
        if (body_in_buf > body_cap) body_in_buf = body_cap;
        for (size_t i = 0; i < body_in_buf; i++) {
            body[i] = buf[(size_t)hdr_end + i];
        }
        blen = body_in_buf;
        if (blen < (size_t)pinfo.content_length) {
            size_t need = (size_t)pinfo.content_length - blen;
            if (need + blen > body_cap) need = body_cap - blen;
            if (need > 0 && socket_recv_exact(fd, body + blen, need, 8000) == 0) {
                blen += need;
            }
        }
        log_fmt3(LOG_INFO, "http", "body_rx", "want", (uint32_t)pinfo.content_length,
                 "got", (uint32_t)blen, "inbuf", (uint32_t)inbuf);
    } else if (body && body_cap > 0 && pinfo.chunked_body) {
        (void)body;
        (void)body_cap;
    }

    if (body_len_out) *body_len_out = blen;

    if (pending && pending_len) {
        size_t pl = 0;
        if (pinfo.content_length > 0 && inbuf > body_in_buf) {
            size_t extra = inbuf - body_in_buf;
            if (extra > pending_cap) extra = pending_cap;
            for (size_t i = 0; i < extra; i++) {
                pending[i] = buf[(size_t)hdr_end + body_in_buf + i];
            }
            pl = extra;
        }
        *pending_len = pl;
        http_save_pending(fd, pending, pending_len, pending_cap);
    }

    return (int)hdr_end;
}

static bool http_mime_gzip_ok(const char* mime) {
    if (!mime) return false;
    return mime[0] == 't' && mime[1] == 'e' && mime[2] == 'x' && mime[3] == 't';
}

static bool http_handle_request(int cfd, const char* req, size_t req_len,
                                const char* body, size_t body_len,
                                uint32_t client_ip, bool* keep_conn) {
    struct http_request_info info;
    int pr = http_parse_request(req, req_len, &info);
    *keep_conn = false;

    if (pr == -3) {
        static const char body_err[] = "HTTP/2 and HTTPS not supported yet\r\n";
        http_send_response(cfd, 505, "text/plain", body_err, sizeof(body_err) - 1, true, false, true, 0);
        http_log_access(HTTP_METHOD_UNKNOWN, "/", 505, client_ip);
        return false;
    }
    if (pr == -2) {
        static const char body_err[] = "Method not allowed\r\n";
        http_send_response(cfd, 405, "text/plain", body_err, sizeof(body_err) - 1, true, false, true, 0);
        http_log_access(HTTP_METHOD_UNKNOWN, "/", 405, client_ip);
        return false;
    }
    if (pr != 0 || !info.valid) {
        static const char body_err[] = "Bad request\r\n";
        http_send_response(cfd, 400, "text/plain", body_err, sizeof(body_err) - 1,
                           info.is_http11, false, true, 0);
        http_log_access(info.method, info.path[0] ? info.path : "/", 400, client_ip);
        return false;
    }

    *keep_conn = info.keep_alive;

    if (info.method == HTTP_METHOD_OPTIONS) {
        http_send_response(cfd, 204, "text/plain", 0, 0, info.is_http11, info.keep_alive, false, 0);
        http_log_access(info.method, info.path, 204, client_ip);
        return info.keep_alive;
    }

    if (!http_path_safe(info.path)) {
        static const char body_err[] = "Bad path\r\n";
        http_send_response(cfd, 400, "text/plain", body_err, sizeof(body_err) - 1,
                           info.is_http11, info.keep_alive, true, 0);
        http_log_access(info.method, info.path, 400, client_ip);
        return info.keep_alive;
    }

    if (http_path_eq(info.path, "/api/echo") && info.method == HTTP_METHOD_POST) {
        if (body_len == 0) {
            static const char empty[] = "";
            http_send_response(cfd, 200, "text/plain; charset=utf-8", empty, 0,
                               info.is_http11, info.keep_alive, true, 0);
        } else {
            http_send_response(cfd, 200, "text/plain; charset=utf-8", body, body_len,
                               info.is_http11, info.keep_alive, true, 0);
        }
        http_log_access(info.method, info.path, 200, client_ip);
        log_fmt3(LOG_INFO, "http", "post_echo", "bytes", (uint32_t)body_len, "ka", info.keep_alive ? 1u : 0u, "ok", 1u);
        return info.keep_alive;
    }

    if (http_path_eq(info.path, "/api/access-log") && info.method == HTTP_METHOD_GET) {
        uint32_t fsize = 0;
        static char log_body[HTTP_ACCESS_MAX];
        size_t rlen = 0;
        if (fs_open(HTTP_ACCESS_LOG, &fsize) == 0 && fsize > 0) {
            if (fsize >= HTTP_ACCESS_MAX) fsize = HTTP_ACCESS_MAX - 1;
            int rd = fs_read(HTTP_ACCESS_LOG, log_body, fsize);
            if (rd > 0) rlen = (size_t)rd;
        }
        if (rlen == 0) {
            const char* none = "# empty access log\n";
            rlen = http_strlen(none);
            for (size_t i = 0; i < rlen; i++) log_body[i] = none[i];
        }
        log_body[rlen] = 0;
        http_send_response(cfd, 200, "text/plain; charset=utf-8", log_body, rlen,
                           info.is_http11, info.keep_alive, true, 0);
        http_log_access(info.method, info.path, 200, client_ip);
        return info.keep_alive;
    }

    if (http_path_eq(info.path, "/api/ports") && info.method == HTTP_METHOD_GET) {
        static char ports_body[4096];
        size_t plen = net_ports_format_table(ports_body, sizeof(ports_body));
        http_send_response(cfd, 200, "text/plain; charset=utf-8", ports_body, plen,
                           info.is_http11, info.keep_alive, true, 0);
        http_log_access(info.method, info.path, 200, client_ip);
        return info.keep_alive;
    }

    if (info.method == HTTP_METHOD_PUT) {
        char fs_path[HTTP_PATH_MAX + 16];
        http_build_fs_path(info.path, fs_path, sizeof(fs_path));
        if (body_len == 0) {
            static const char body_err[] = "Empty PUT body\r\n";
            http_send_response(cfd, 400, "text/plain", body_err, sizeof(body_err) - 1,
                               info.is_http11, info.keep_alive, true, 0);
            http_log_access(info.method, info.path, 400, client_ip);
            return info.keep_alive;
        }
        if (fs_write(fs_path, body, body_len) != 0) {
            static const char body_err[] = "Write failed\r\n";
            http_send_response(cfd, 500, "text/plain", body_err, sizeof(body_err) - 1,
                               info.is_http11, false, true, 0);
            http_log_access(info.method, info.path, 500, client_ip);
            return false;
        }
        static const char ok[] = "Created\r\n";
        http_send_response(cfd, 201, "text/plain; charset=utf-8", ok, sizeof(ok) - 1,
                           info.is_http11, info.keep_alive, true, 0);
        http_log_access(info.method, info.path, 201, client_ip);
        log_fmt3(LOG_INFO, "http", "put", "bytes", (uint32_t)body_len, "path", 0, "ok", 1u);
        return info.keep_alive;
    }

    if (info.method != HTTP_METHOD_GET && info.method != HTTP_METHOD_HEAD) {
        static const char body_err[] = "Method not allowed\r\n";
        http_send_response(cfd, 405, "text/plain", body_err, sizeof(body_err) - 1,
                           info.is_http11, false, true, 0);
        http_log_access(info.method, info.path, 405, client_ip);
        return false;
    }

    char fs_path[HTTP_PATH_MAX + 16];
    http_build_fs_path(info.path, fs_path, sizeof(fs_path));

    uint32_t fsize = 0;
    if (fs_open(fs_path, &fsize) != 0 || fsize == 0) {
        log_msg(LOG_INFO, "http", "404");
        static const char body[] =
            "<html><body><h1>404 Not Found</h1>"
            "<p>Place files under /www on disk.</p></body></html>";
        http_send_response(cfd, 404, "text/html; charset=utf-8", body, sizeof(body) - 1,
                           info.is_http11, info.keep_alive, info.method != HTTP_METHOD_HEAD, 0);
        http_log_access(info.method, info.path, 404, client_ip);
        return info.keep_alive;
    }

    if (fsize > HTTP_FILE_MAX) fsize = HTTP_FILE_MAX;

    static uint8_t file_buf[HTTP_FILE_MAX];
    static uint8_t gzip_buf[HTTP_FILE_MAX + 64];
    int rd = fs_read(fs_path, file_buf, fsize);
    if (rd < 0) {
        static const char body_err[] = "Read error\r\n";
        http_send_response(cfd, 500, "text/plain", body_err, sizeof(body_err) - 1,
                           info.is_http11, false, true, 0);
        http_log_access(info.method, info.path, 500, client_ip);
        return false;
    }

    const char* mime = http_guess_mime(fs_path);
    bool send_body = (info.method != HTTP_METHOD_HEAD);
    const void* out_body = file_buf;
    size_t out_len = (size_t)rd;
    const char* encoding = 0;

    if (send_body && info.accept_gzip && http_mime_gzip_ok(mime) && rd > 0) {
        size_t gz_len = 0;
        if (http_gzip_compress(file_buf, (size_t)rd, gzip_buf, sizeof(gzip_buf), &gz_len) == 0 &&
            gz_len > 0) {
            out_body = gzip_buf;
            out_len = gz_len;
            encoding = "gzip";
            log_msg(LOG_INFO, "http", "gzip");
        }
    }

    http_send_response(cfd, 200, mime, out_body, out_len,
                       info.is_http11, info.keep_alive, send_body, encoding);
    http_log_access(info.method, info.path, 200, client_ip);
    log_fmt3(LOG_INFO, "http", "sent", "bytes", (uint32_t)out_len, "status", 200,
             "ka", info.keep_alive ? 1u : 0u);
    return info.keep_alive;
}

struct httpd_args {
    uint16_t port;
    int max_requests;
    int accept_timeout_ms;
};

static volatile int g_httpd_ready = 0;
static volatile int g_httpd_done = 0;
static volatile int g_httpd_served = 0;
static volatile int g_httpd_pid = -1;
static volatile int g_httpd_running = 0;
static struct httpd_args g_httpd_args;

void http_server_init(void) {
    fs_create_dir("/www");
    uint32_t sz = 0;
    if (fs_open("/www/index.html", &sz) != 0) {
        const char* html =
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>KnitOS</title></head>"
            "<body><h1>KnitOS HTTP Server</h1>"
            "<p>HTTP/1.1: GET, HEAD, OPTIONS, POST, PUT, gzip, access log.</p>"
            "<p>Edit files in <code>/www</code> on disk.</p>"
            "</body></html>";
        fs_write("/www/index.html", html, http_strlen(html));
    }
}

static void httpd_task(void* arg);

int http_server_pid(void) {
    return g_httpd_pid;
}

int http_server_ready(void) {
    return g_httpd_ready;
}

int http_server_last_served(void) {
    return g_httpd_served;
}

static void httpd_task(void* arg) {
    struct httpd_args* a = (struct httpd_args*)arg;
    g_httpd_pid = sched_current_id();
    log_fmt3(LOG_INFO, "autotest", "httpd_kthread_ok", "pid", (uint32_t)g_httpd_pid,
             "port", (uint32_t)a->port, "ok", 1u);
    int n = http_server_run(a->port, a->max_requests, a->accept_timeout_ms);
    g_httpd_served = n;
    g_httpd_done = 1;
    g_httpd_running = 0;
    task_exit();
}

int http_server_start(uint16_t port, int max_requests, int accept_timeout_ms) {
    if (g_httpd_running && !g_httpd_done) return -1;
    if (ip_get_our_ip() == 0) return -1;
    g_httpd_args.port = port;
    g_httpd_args.max_requests = max_requests;
    g_httpd_args.accept_timeout_ms = accept_timeout_ms;
    g_httpd_ready = 0;
    g_httpd_done = 0;
    g_httpd_served = 0;
    g_httpd_pid = -1;
    g_httpd_running = 1;
    int id = task_create(httpd_task, &g_httpd_args, "httpd");
    if (id < 0) {
        g_httpd_running = 0;
        return -1;
    }
    task_enable_aspace(id);
    return id;
}

/* Clear flags after task_kill (victim never reaches httpd_task epilogue). */
void http_server_clear_state(void) {
    g_httpd_done = 1;
    g_httpd_running = 0;
    g_httpd_ready = 0;
    g_httpd_pid = -1;
}

int http_server_wait(int timeout_ms) {
    uint32_t t0 = timer_ms();
    while (!g_httpd_done) {
        if (timeout_ms > 0 && (int)timer_ms_since(t0) >= timeout_ms) return 1;
        nic_process_packets();
        tcp_process_timers();
        sched_yield();
    }
    return 0;
}

int http_server_run(uint16_t port, int max_requests, int accept_timeout_ms) {
    if (ip_get_our_ip() == 0) return -1;
    if (max_requests < 1) max_requests = 1;
    if (accept_timeout_ms < 1000) accept_timeout_ms = 1000;

    http_server_init();
    g_httpd_ready = 0;

    int sfd = socket_create(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return -1;
    socket_set_owner(sfd, "httpd", -1); /* current = httpd kthread pid */
    if (g_httpd_pid < 0) g_httpd_pid = sched_current_id();

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = port;
    addr.sin_addr = 0;

    if (socket_bind(sfd, &addr) != 0) {
        socket_close(sfd);
        return -1;
    }
    if (socket_listen(sfd, 4) != 0) {
        socket_close(sfd);
        return -1;
    }

    log_fmt3(LOG_INFO, "http", "listen", "port", (uint32_t)port, "max", (uint32_t)max_requests, "ok", 1);
    g_httpd_ready = 1;
    log_msg(LOG_INFO, "autotest", "http_ready");
    char ipbuf[16];
    ip_format_address(ip_get_our_ip(), ipbuf, sizeof(ipbuf));
    log_ip(LOG_INFO, "http", "bind ip", ip_get_our_ip());
    terminal_writestring("\n[HTTP] listen ");
    terminal_writestring(ipbuf);
    terminal_writestring(":");
    http_term_u32((uint32_t)port);
    terminal_writestring(" (waiting)");

    int served = 0;
    static char req_buf[HTTP_REQ_MAX];
    static char body_buf[HTTP_BODY_MAX];
    static char pending_buf[HTTP_PENDING_MAX];
    int accept_waits = 0;
    /* Wall-clock idle: exit only after this much quiet time post first request. */
    const uint32_t idle_exit_ms = (uint32_t)accept_timeout_ms * 3u; /* e.g. 15s */
    uint32_t last_activity_ms = timer_ms();

    while (served < max_requests) {
        int cfd = socket_accept(sfd, accept_timeout_ms);
        if (cfd < 0) {
            accept_waits++;
            uint32_t quiet = timer_ms_since(last_activity_ms);
            if (served > 0 && quiet >= idle_exit_ms) {
                log_fmt3(LOG_INFO, "http", "idle_done", "served", (uint32_t)served,
                         "quiet_ms", quiet, "port", (uint32_t)port);
                terminal_writestring("\n[HTTP] idle done served=");
                http_term_u32((uint32_t)served);
                break;
            }
            if (accept_waits <= 3 || (accept_waits % 50) == 0) {
                log_fmt3(LOG_INFO, "http", "wait_accept", "round", (uint32_t)accept_waits,
                         "port", (uint32_t)port, "served", (uint32_t)served);
            }
            if (accept_waits == 1) {
                terminal_writestring("\n[HTTP] waiting for TCP...");
            }
            continue;
        }
        accept_waits = 0;
        last_activity_ms = timer_ms();

        log_msg(LOG_INFO, "http", "accept");
        terminal_writestring("\n[HTTP] accept");

        uint32_t peer_ip = 0;
        socket_get_peer(cfd, &peer_ip, 0);

        int conn_reqs = 0;
        bool conn_alive = true;
        size_t pending_len = 0;
        while (conn_alive && served < max_requests && conn_reqs < HTTP_CONN_MAX) {
            size_t body_len = 0;
            int req_len = http_recv_request(cfd, req_buf, sizeof(req_buf),
                                            body_buf, sizeof(body_buf), &body_len,
                                            pending_buf, &pending_len, sizeof(pending_buf),
                                            conn_reqs > 0);
            if (req_len <= 0) break;

            bool keep_conn = false;
            conn_alive = http_handle_request(cfd, req_buf, (size_t)req_len,
                                               body_buf, body_len, peer_ip, &keep_conn);
            served++;
            conn_reqs++;

            if (!keep_conn) break;
        }
        socket_close(cfd);
    }

    socket_close(sfd);
    return served;
}
