#ifndef HTTP_GZIP_H
#define HTTP_GZIP_H

#include <stddef.h>
#include <stdint.h>

// Wrap uncompressed data in a valid gzip stream (deflate stored blocks).
// Returns 0 on success and sets *out_len.
int http_gzip_compress(const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t out_cap, size_t* out_len);

#endif
