#pragma once

#include <stddef.h>

#define HTTP_BAD_REQUEST "HTTP/1.1 400 Bad request\r\n\r\n"
#define HTTP_UNAVAILABLE "HTTP/1.1 503 Service Unavailable\r\n\r\n"
#define HTTP_SERVER_ERROR "HTTP/1.1 500 Internal Server Error\r\n\r\n"

const size_t kMaxHttpRequestLength = 4096;
