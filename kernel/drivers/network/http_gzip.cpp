#include "http_gzip.h"

static uint32_t http_gzip_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

int http_gzip_compress(const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t out_cap, size_t* out_len) {
    if (!in || !out || !out_len || in_len > 65535) return -1;

    size_t need = 10 + 5 + in_len + 8;
    if (out_cap < need) return -1;

    size_t pos = 0;
    out[pos++] = 0x1f;
    out[pos++] = 0x8b;
    out[pos++] = 0x08;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = 0xff;

    uint16_t chunk = (uint16_t)in_len;
    out[pos++] = 0x01;
    out[pos++] = (uint8_t)(chunk & 0xff);
    out[pos++] = (uint8_t)((chunk >> 8) & 0xff);
    out[pos++] = (uint8_t)(~chunk & 0xff);
    out[pos++] = (uint8_t)((~chunk >> 8) & 0xff);
    for (size_t i = 0; i < in_len; i++) {
        out[pos++] = in[i];
    }

    uint32_t crc = http_gzip_crc32(in, in_len);
    out[pos++] = (uint8_t)(crc & 0xff);
    out[pos++] = (uint8_t)((crc >> 8) & 0xff);
    out[pos++] = (uint8_t)((crc >> 16) & 0xff);
    out[pos++] = (uint8_t)((crc >> 24) & 0xff);
    out[pos++] = (uint8_t)(in_len & 0xff);
    out[pos++] = (uint8_t)((in_len >> 8) & 0xff);
    out[pos++] = (uint8_t)((in_len >> 16) & 0xff);
    out[pos++] = (uint8_t)((in_len >> 24) & 0xff);

    *out_len = pos;
    return 0;
}
