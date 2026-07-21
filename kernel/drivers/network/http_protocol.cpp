#include "http_protocol.h"
#include <stddef.h>
#include <stdint.h>

static char http_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static bool http_header_eq(const char* line, const char* name) {
    size_t i = 0;
    while (name[i]) {
        if (http_tolower(line[i]) != name[i]) return false;
        i++;
    }
    return line[i] == ':' || line[i] == ' ';
}

static const char* http_skip_line(const char* req, size_t req_len, size_t* pos) {
    while (*pos < req_len && req[*pos] != '\r' && req[*pos] != '\n') (*pos)++;
    while (*pos < req_len && (req[*pos] == '\r' || req[*pos] == '\n')) (*pos)++;
    return req + *pos;
}

static bool http_str_icontains(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        while (needle[j] && hay[i + j] &&
               http_tolower(hay[i + j]) == needle[j]) {
            j++;
        }
        if (needle[j] == 0) return true;
    }
    return false;
}

static const char* http_header_value(const char* line) {
    const char* p = line;
    while (*p && *p != ':') p++;
    if (*p != ':') return "";
    p++;
    while (*p == ' ') p++;
    return p;
}

static int http_parse_method(const char* req, size_t len) {
    if (len >= 4 && req[0] == 'G' && req[1] == 'E' && req[2] == 'T' && req[3] == ' ') {
        return HTTP_METHOD_GET;
    }
    if (len >= 5 && req[0] == 'H' && req[1] == 'E' && req[2] == 'A' && req[3] == 'D' && req[4] == ' ') {
        return HTTP_METHOD_HEAD;
    }
    if (len >= 8 && req[0] == 'O' && req[1] == 'P' && req[2] == 'T' && req[3] == 'I' &&
        req[4] == 'O' && req[5] == 'N' && req[6] == 'S' && req[7] == ' ') {
        return HTTP_METHOD_OPTIONS;
    }
    if (len >= 5 && req[0] == 'P' && req[1] == 'O' && req[2] == 'S' && req[3] == 'T' && req[4] == ' ') {
        return HTTP_METHOD_POST;
    }
    if (len >= 4 && req[0] == 'P' && req[1] == 'U' && req[2] == 'T' && req[3] == ' ') {
        return HTTP_METHOD_PUT;
    }
    return HTTP_METHOD_UNKNOWN;
}

static bool http_parse_version(const char* req, size_t req_len, size_t path_end, bool* is_http11) {
    *is_http11 = false;
    size_t i = path_end;
    while (i < req_len && req[i] == ' ') i++;
    if (i + 8 <= req_len && req[i] == 'H' && req[i + 1] == 'T' && req[i + 2] == 'T' &&
        req[i + 3] == 'P' && req[i + 4] == '/') {
        if (req[i + 5] == '1' && req[i + 6] == '.') {
            if (req[i + 7] == '1') {
                *is_http11 = true;
                return true;
            }
            if (req[i + 7] == '0') {
                return true;
            }
        }
        if (req[i + 5] == '2') return false;
    }
    return true;
}

const char* http_status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 426: return "Upgrade Required";
        case 500: return "Internal Server Error";
        case 505: return "HTTP Version Not Supported";
        default:  return "Error";
    }
}

const char* http_guess_mime(const char* path) {
    const char* dot = 0;
    for (const char* p = path; *p; p++) {
        if (*p == '.') dot = p;
    }
    if (!dot) return "application/octet-stream";
    const char* ext = dot + 1;
    if (ext[0] == 'h' && ext[1] == 't' && ext[2] == 'm' && ext[3] == 'l' && ext[4] == 0) return "text/html; charset=utf-8";
    if (ext[0] == 'h' && ext[1] == 't' && ext[2] == 'm' && ext[3] == 0) return "text/html; charset=utf-8";
    if (ext[0] == 't' && ext[1] == 'x' && ext[2] == 't' && ext[3] == 0) return "text/plain; charset=utf-8";
    if (ext[0] == 'c' && ext[1] == 's' && ext[2] == 's' && ext[3] == 0) return "text/css; charset=utf-8";
    if (ext[0] == 'j' && ext[1] == 's' && ext[2] == 0) return "application/javascript; charset=utf-8";
    if (ext[0] == 'j' && ext[1] == 's' && ext[2] == 'o' && ext[3] == 'n' && ext[4] == 0) return "application/json; charset=utf-8";
    if (ext[0] == 's' && ext[1] == 'v' && ext[2] == 'g' && ext[3] == 0) return "image/svg+xml";
    if (ext[0] == 'w' && ext[1] == 'e' && ext[2] == 'b' && ext[3] == 'p' && ext[4] == 0) return "image/webp";
    if (ext[0] == 'p' && ext[1] == 'n' && ext[2] == 'g' && ext[3] == 0) return "image/png";
    if (ext[0] == 'g' && ext[1] == 'i' && ext[2] == 'f' && ext[3] == 0) return "image/gif";
    if (ext[0] == 'i' && ext[1] == 'c' && ext[2] == 'o' && ext[3] == 0) return "image/x-icon";
    if (ext[0] == 'w' && ext[1] == 'o' && ext[2] == 'f' && ext[3] == 'f' && ext[4] == 0) return "font/woff";
    if (ext[0] == 'w' && ext[1] == 'o' && ext[2] == 'f' && ext[3] == 'f' && ext[4] == '2' && ext[5] == 0) return "font/woff2";
    return "application/octet-stream";
}

int http_parse_request(const char* req, size_t req_len, struct http_request_info* out) {
    if (!req || !out || req_len < 16) return -1;

    out->method = HTTP_METHOD_UNKNOWN;
    out->path[0] = 0;
    out->is_http11 = false;
    out->keep_alive = false;
    out->accept_gzip = false;
    out->expect_continue = false;
    out->chunked_body = false;
    out->content_length = 0;
    out->valid = false;

    int method = http_parse_method(req, req_len);
    if (method == HTTP_METHOD_UNKNOWN) return -2;

    size_t start = 0;
    while (start < req_len && req[start] != ' ') start++;
    if (start >= req_len) return -1;
    start++;
    size_t end = start;
    while (end < req_len && req[end] != ' ' && req[end] != '\r' && req[end] != '\n') end++;
    if (end == start) return -1;

    size_t o = 0;
    for (size_t i = start; i < end && o + 1 < sizeof(out->path); i++) {
        char c = req[i];
        if (c == '?') break;
        out->path[o++] = c;
    }
    out->path[o] = 0;

    bool version_ok = http_parse_version(req, req_len, end, &out->is_http11);
    if (!version_ok) return -3;

    out->method = method;
    out->keep_alive = out->is_http11;

    size_t pos = 0;
    while (pos < req_len && (req[pos] != '\r' && req[pos] != '\n')) pos++;
    http_skip_line(req, req_len, &pos);

    bool conn_close = false;
    bool conn_keep = false;

    while (pos < req_len) {
        if (pos + 1 < req_len && req[pos] == '\r' && req[pos + 1] == '\n') break;
        size_t line_start = pos;
        http_skip_line(req, req_len, &pos);
        size_t line_len = pos - line_start;
        while (line_len > 0 && (req[line_start + line_len - 1] == '\r' ||
                                req[line_start + line_len - 1] == '\n')) {
            line_len--;
        }
        if (line_len == 0) continue;

        char hname[32];
        size_t hi = 0;
        while (hi + 1 < sizeof(hname) && hi < line_len && req[line_start + hi] != ':') {
            hname[hi] = req[line_start + hi];
            hi++;
        }
        hname[hi] = 0;

        const char* val = http_header_value(req + line_start);

        if (http_header_eq(hname, "connection")) {
            if (http_str_icontains(val, "close")) conn_close = true;
            if (http_str_icontains(val, "keep-alive")) conn_keep = true;
        } else if (http_header_eq(hname, "content-length")) {
            int cl = 0;
            while (*val >= '0' && *val <= '9') {
                cl = cl * 10 + (*val - '0');
                val++;
            }
            if (cl >= 0 && cl <= HTTP_BODY_MAX) out->content_length = cl;
        } else if (http_header_eq(hname, "accept-encoding")) {
            if (http_str_icontains(val, "gzip")) out->accept_gzip = true;
        } else if (http_header_eq(hname, "expect")) {
            if (http_str_icontains(val, "100-continue")) out->expect_continue = true;
        } else if (http_header_eq(hname, "transfer-encoding")) {
            if (http_str_icontains(val, "chunked")) out->chunked_body = true;
        }
    }

    if (conn_close) out->keep_alive = false;
    else if (conn_keep) out->keep_alive = true;
    else if (!out->is_http11) out->keep_alive = false;

    out->valid = true;
    return 0;
}

static void http_put_str(char* buf, size_t cap, size_t* hp, const char* s) {
    while (*s && *hp + 1 < cap) buf[(*hp)++] = *s++;
}

static void http_put_u32(char* buf, size_t cap, size_t* hp, uint32_t n) {
    char tmp[12];
    int t = 0;
    if (n == 0) {
        if (*hp + 1 < cap) buf[(*hp)++] = '0';
        return;
    }
    while (n > 0 && t < 11) {
        tmp[t++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (t > 0 && *hp + 1 < cap) buf[(*hp)++] = tmp[--t];
}

int http_format_response_header(char* buf, size_t cap, int status,
                                const char* content_type, size_t body_len,
                                bool is_http11, bool keep_alive,
                                const char* content_encoding) {
    if (!buf || cap < 64) return -1;
    size_t hp = 0;

    http_put_str(buf, cap, &hp, is_http11 ? "HTTP/1.1 " : "HTTP/1.0 ");
    if (status == 200) http_put_str(buf, cap, &hp, "200 ");
    else {
        char sn[8];
        int si = 0;
        uint32_t st = (uint32_t)status;
        char tmp[8];
        int t = 0;
        while (st > 0 && t < 7) { tmp[t++] = (char)('0' + (st % 10)); st /= 10; }
        while (t > 0 && si < 7) sn[si++] = tmp[--t];
        sn[si++] = ' ';
        sn[si] = 0;
        http_put_str(buf, cap, &hp, sn);
    }
    http_put_str(buf, cap, &hp, http_status_text(status));
    http_put_str(buf, cap, &hp, "\r\nContent-Type: ");
    http_put_str(buf, cap, &hp, content_type);
    http_put_str(buf, cap, &hp, "\r\nContent-Length: ");
    http_put_u32(buf, cap, &hp, (uint32_t)body_len);
    if (content_encoding && content_encoding[0]) {
        http_put_str(buf, cap, &hp, "\r\nContent-Encoding: ");
        http_put_str(buf, cap, &hp, content_encoding);
    }
    http_put_str(buf, cap, &hp, "\r\nConnection: ");
    http_put_str(buf, cap, &hp, keep_alive ? "keep-alive" : "close");
    http_put_str(buf, cap, &hp, "\r\nServer: MyOS-HTTP/1.1");
    if (status == 200 || status == 204) {
        http_put_str(buf, cap, &hp, "\r\nAccept-Ranges: bytes");
    }
    if (status == 405 || status == 204) {
        http_put_str(buf, cap, &hp, "\r\nAllow: GET, HEAD, OPTIONS, POST, PUT");
    }
    http_put_str(buf, cap, &hp, "\r\n\r\n");
    if (hp < cap) buf[hp] = 0;
    return (int)hp;
}
