#ifndef LIBESPHTTPD_BASE64_H
#define LIBESPHTTPD_BASE64_H

int libesphttpd_base64_decode(size_t in_len, const char *in, size_t out_len, unsigned char *out);
int libesphttpd_base64_encode(size_t in_len, const unsigned char *in, size_t out_len, char *out);
#endif