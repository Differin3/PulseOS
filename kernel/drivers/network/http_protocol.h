#ifndef HTTP_PROTOCOL_H
#define HTTP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define HTTP_METHOD_UNKNOWN 0
#define HTTP_METHOD_GET     1
#define HTTP_METHOD_HEAD    2
#define HTTP_METHOD_OPTIONS 3
#define HTTP_METHOD_POST    4
#define HTTP_METHOD_PUT     5

#define HTTP_BODY_MAX 4096

struct http_request_info {
    int method;
    char path[192];
    bool is_http11;
    bool keep_alive;
    bool accept_gzip;
    bool expect_continue;
    bool chunked_body;
    int content_length;
    bool valid;
};

// Разбор строки запроса и заголовков (до \r\n\r\n).
int http_parse_request(const char* req, size_t req_len, struct http_request_info* out);

// Заголовки ответа HTTP/1.0 или HTTP/1.1. content_encoding may be NULL.
int http_format_response_header(char* buf, size_t cap, int status,
                                const char* content_type, size_t body_len,
                                bool is_http11, bool keep_alive,
                                const char* content_encoding);

const char* http_status_text(int status);
const char* http_guess_mime(const char* path);

#endif
