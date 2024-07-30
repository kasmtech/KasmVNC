/*
 * WebSocket lib with support for "wss://" encryption.
 * Copyright 2010 Joel Martin
 * Licensed under LGPL version 3 (see docs/LICENSE.LGPL-3)
 *
 * You can make a cert/key with openssl using:
 * openssl req -new -x509 -days 365 -nodes -out self.pem -keyout self.pem
 * as taken from http://docs.python.org/dev/library/ssl.html#certificates
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>  // daemonizing
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/bio.h> /* base64 encode/decode */
#include <openssl/md5.h> /* md5 hash */
#include <openssl/sha.h> /* sha1 hash */
#include "websocket.h"
#include "jsonescape.h"
#include <network/Blacklist.h>

/*
 * Global state
 *
 *   Warning: not thread safe
 */
int pipe_error = 0;
settings_t settings;

extern int wakeuppipe[2];

extern char *extra_headers;
extern unsigned extra_headers_len;

void traffic(const char * token) {
    /*if ((settings.verbose) && (! settings.daemon)) {
        fprintf(stdout, "%s", token);
        fflush(stdout);
    }*/
}

void error(char *msg)
{
    perror(msg);
}

void fatal(char *msg)
{
    perror(msg);
    exit(1);
}

// 2022-05-18 19:51:26,810 [INFO] websocket 0: 71.62.44.0 172.12.15.5 - "GET /api/get_frame_stats?client=auto HTTP/1.1" 403 2
static void weblog(const unsigned code, const unsigned websocket,
                   const uint8_t debug,
                   const char *origip, const char *ip,
                   const char *user, const uint8_t get,
                   const char *url, const uint64_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm local;
    localtime_r(&tv.tv_sec, &local);

    char timebuf[128];
    strftime(timebuf, sizeof(timebuf), DATELOGFMT, &local);

    const unsigned msec = tv.tv_usec / 1000;
    const char *levelname = debug ? "DEBUG" : "INFO";

    fprintf(stderr, " %s,%03u [%s] websocket %u: %s %s %s \"%s %s HTTP/1.1\" %u %lu\n",
            timebuf, msec, levelname, websocket, origip, ip, user,
            get ? "GET" : "POST", url, code, len);
}

void wslog(char *logbuf, const unsigned websocket, const uint8_t debug)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm local;
    localtime_r(&tv.tv_sec, &local);

    char timebuf[128];
    strftime(timebuf, sizeof(timebuf), DATELOGFMT, &local);

    const unsigned msec = tv.tv_usec / 1000;
    const char *levelname = debug ? "DEBUG" : "INFO";

    sprintf(logbuf, " %s,%03u [%s] websocket %u: ",
            timebuf, msec, levelname, websocket);
}

/* resolve host with also IP address parsing */
int resolve_host(struct in_addr *sin_addr, const char *hostname)
{
    if (!inet_aton(hostname, sin_addr)) {
        struct addrinfo *ai, *cur;
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        if (getaddrinfo(hostname, NULL, &hints, &ai))
            return -1;
        for (cur = ai; cur; cur = cur->ai_next) {
            if (cur->ai_family == AF_INET) {
                *sin_addr = ((struct sockaddr_in *)cur->ai_addr)->sin_addr;
                freeaddrinfo(ai);
                return 0;
            }
        }
        freeaddrinfo(ai);
        return -1;
    }
    return 0;
}

static const char *parse_get(const char * const in, const char * const opt, unsigned *len) {
	const char *start = in;
	const char *end = strchrnul(start, '&');
	const unsigned optlen = strlen(opt);
	*len = 0;

	while (1) {
		if (!strncmp(start, opt, optlen)) {
			const char *arg = strchr(start, '=');
			if (!arg)
				return "";
			arg++;
			*len = end - arg;
			return arg;
		}

		if (!*end)
			break;

		end++;
		start = end;
		end = strchrnul(start, '&');
	}

	return "";
}

static void percent_decode(const char *src, char *dst, const uint8_t filter) {
	while (1) {
		if (!*src)
			break;
		if (*src != '%') {
			*dst++ = *src++;
		} else {
			src++;
			if (!src[0] || !src[1])
				break;
			char hex[3];
			hex[0] = src[0];
			hex[1] = src[1];
			hex[2] = '\0';

			src += 2;
			*dst = strtol(hex, NULL, 16);

			if (filter) {
				// Avoid directory traversal
				if (*dst == '/')
					*dst = '_';
			}

			dst++;
		}
	}

	*dst = '\0';
}

static void percent_encode(const char *src, char *dst) {
	while (1) {
		if (!*src)
			break;
		if (isalnum(*src) || *src == '.' || *src == ',') {
			*dst++ = *src++;
		} else {
			*dst++ = '%';
			sprintf(dst, "%02X", *src);
			dst += 2;
			src++;
		}
	}

	*dst = '\0';
}

/*
 * SSL Wrapper Code
 */

ssize_t ws_recv(ws_ctx_t *ctx, void *buf, size_t len) {
    if (ctx->ssl) {
        //handler_msg("SSL recv\n");
        return SSL_read(ctx->ssl, buf, len);
    } else {
        return recv(ctx->sockfd, buf, len, 0);
    }
}

ssize_t ws_send(ws_ctx_t *ctx, const void *buf, size_t len) {
    if (ctx->ssl) {
        //handler_msg("SSL send\n");
        return SSL_write(ctx->ssl, buf, len);
    } else {
        return send(ctx->sockfd, buf, len, 0);
    }
}

ws_ctx_t *alloc_ws_ctx() {
    ws_ctx_t *ctx;
    if (! (ctx = calloc(sizeof(ws_ctx_t), 1)) )
        { fatal("malloc()"); }

    if (! (ctx->cin_buf = malloc(BUFSIZE)) )
        { fatal("malloc of cin_buf"); }
    if (! (ctx->cout_buf = malloc(BUFSIZE)) )
        { fatal("malloc of cout_buf"); }
    if (! (ctx->tin_buf = malloc(BUFSIZE)) )
        { fatal("malloc of tin_buf"); }
    if (! (ctx->tout_buf = malloc(BUFSIZE)) )
        { fatal("malloc of tout_buf"); }

    ctx->headers = malloc(sizeof(headers_t));
    ctx->ssl = NULL;
    ctx->ssl_ctx = NULL;
    return ctx;
}

void free_ws_ctx(ws_ctx_t *ctx) {
    free(ctx->cin_buf);
    free(ctx->cout_buf);
    free(ctx->tin_buf);
    free(ctx->tout_buf);
    free(ctx->headers);
    free(ctx);
}

ws_ctx_t *ws_socket(ws_ctx_t *ctx, int socket) {
    ctx->sockfd = socket;
    return ctx;
}

ws_ctx_t *ws_socket_ssl(ws_ctx_t *ctx, int socket, const char * certfile, const char * keyfile) {
    int ret;
    char msg[1024];
    const char * use_keyfile;
    ws_socket(ctx, socket);

    if (keyfile && (keyfile[0] != '\0')) {
        // Separate key file
        use_keyfile = keyfile;
    } else {
        // Combined key and cert file
        use_keyfile = certfile;
    }

    ctx->ssl_ctx = SSL_CTX_new(SSLv23_server_method());
    if (ctx->ssl_ctx == NULL) {
        ERR_print_errors_fp(stderr);
        fatal("Failed to configure SSL context");
    }

    SSL_CTX_set_options(ctx->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, use_keyfile,
                                    SSL_FILETYPE_PEM) <= 0) {
        sprintf(msg, "Unable to load private key file %s\n", use_keyfile);
        fatal(msg);
    }

    if (SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, certfile) <= 0) {
        sprintf(msg, "Unable to load certificate file %s\n", certfile);
        fatal(msg);
    }

//    if (SSL_CTX_set_cipher_list(ctx->ssl_ctx, "DEFAULT") != 1) {
//        sprintf(msg, "Unable to set cipher\n");
//        fatal(msg);
//    }

    // Associate socket and ssl object
    ctx->ssl = SSL_new(ctx->ssl_ctx);
    SSL_set_fd(ctx->ssl, socket);

    ret = SSL_accept(ctx->ssl);
    if (ret < 0) {
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    return ctx;
}

void ws_socket_free(ws_ctx_t *ctx) {
    if (ctx->ssl) {
        SSL_free(ctx->ssl);
        ctx->ssl = NULL;
    }
    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
    }
    if (ctx->sockfd) {
        shutdown(ctx->sockfd, SHUT_RDWR);
        close(ctx->sockfd);
        ctx->sockfd = 0;
    }
}

int ws_b64_ntop(const unsigned char * const src, size_t srclen, char * dst, size_t dstlen) {
    int len = 0;
    int total_len = 0;

    BIO *buff, *b64f;
    BUF_MEM *ptr;

    b64f = BIO_new(BIO_f_base64());
    buff = BIO_new(BIO_s_mem());
    buff = BIO_push(b64f, buff);

    BIO_set_flags(buff, BIO_FLAGS_BASE64_NO_NL);
    BIO_set_close(buff, BIO_CLOSE);
    do {
        len = BIO_write(buff, src + total_len, srclen - total_len);
        if (len > 0)
            total_len += len;
    } while (len && BIO_should_retry(buff));

    BIO_flush(buff);

    BIO_get_mem_ptr(buff, &ptr);
    len = ptr->length;

    memcpy(dst, ptr->data, dstlen < len ? dstlen : len);
    dst[dstlen < len ? dstlen : len] = '\0';

    BIO_free_all(buff);

    if (dstlen < len)
        return -1;

    return len;
}

int ws_b64_pton(const char * const src, unsigned char * dst, size_t dstlen) {
    int len = 0;
    int total_len = 0;
    int pending = 0;

    BIO *buff, *b64f;

    b64f = BIO_new(BIO_f_base64());
    buff = BIO_new_mem_buf(src, -1);
    buff = BIO_push(b64f, buff);

    BIO_set_flags(buff, BIO_FLAGS_BASE64_NO_NL);
    BIO_set_close(buff, BIO_CLOSE);
    do {
        len = BIO_read(buff, dst + total_len, dstlen - total_len);
        if (len > 0)
            total_len += len;
    } while (len && BIO_should_retry(buff));

    dst[total_len] = '\0';

    pending = BIO_ctrl_pending(buff);

    BIO_free_all(buff);

    if (pending)
        return -1;

    return len;
}

/* ------------------------------------------------------- */


int encode_hixie(u_char const *src, size_t srclength,
                 char *target, size_t targsize) {
    int sz = 0, len = 0;
    target[sz++] = '\x00';
    len = ws_b64_ntop(src, srclength, target+sz, targsize-sz);
    if (len < 0) {
        return len;
    }
    sz += len;
    target[sz++] = '\xff';
    return sz;
}

int decode_hixie(char *src, size_t srclength,
                 u_char *target, size_t targsize,
                 unsigned int *opcode, unsigned int *left) {
    char *start, *end, cntstr[4];
    int len, framecount = 0, retlen = 0;
    if ((src[0] != '\x00') || (src[srclength-1] != '\xff')) {
        handler_emsg("WebSocket framing error\n");
        return -1;
    }
    *left = srclength;

    if (srclength == 2 &&
        (src[0] == '\xff') &&
        (src[1] == '\x00')) {
        // client sent orderly close frame
        *opcode = 0x8; // Close frame
        return 0;
    }
    *opcode = 0x1; // Text frame

    start = src+1; // Skip '\x00' start
    do {
        /* We may have more than one frame */
        end = (char *)memchr(start, '\xff', srclength);
        *end = '\x00';
        len = ws_b64_pton(start, target+retlen, targsize-retlen);
        if (len < 0) {
            return len;
        }
        retlen += len;
        start = end + 2; // Skip '\xff' end and '\x00' start
        framecount++;
    } while (end < (src+srclength-1));
    if (framecount > 1) {
        snprintf(cntstr, 3, "%d", framecount);
        traffic(cntstr);
    }
    *left = 0;
    return retlen;
}

int encode_hybi(u_char const *src, size_t srclength,
                char *target, size_t targsize, unsigned int opcode)
{
    unsigned long long payload_offset = 2;
    int len = 0;

    if (opcode != OPCODE_TEXT && opcode != OPCODE_BINARY) {
        handler_emsg("Invalid opcode. Opcode must be 0x01 for text mode, or 0x02 for binary mode.\n");
        return -1;
    }

    target[0] = (char)((opcode & 0x0F) | 0x80);

    if ((int)srclength <= 0) {
        return 0;
    }

    if (opcode & OPCODE_TEXT) {
        len = ((srclength - 1) / 3) * 4 + 4;
    } else {
        len = srclength;
    }

    if (len <= 125) {
        target[1] = (char) len;
        payload_offset = 2;
    } else if ((len > 125) && (len < 65536)) {
        target[1] = (char) 126;
        *(u_short*)&(target[2]) = htons(len);
        payload_offset = 4;
    } else {
        handler_emsg("Sending frames larger than 65535 bytes not supported\n");
        return -1;
        //target[1] = (char) 127;
        // *(u_long*)&(target[2]) = htonl(b64_sz);
        //payload_offset = 10;
    }

    if (opcode & OPCODE_TEXT) {
        len = ws_b64_ntop(src, srclength, target+payload_offset, targsize-payload_offset);
    } else {
        memcpy(target+payload_offset, src, srclength);
        len = srclength;
    }

    if (len < 0) {
        return len;
    }

    return len + payload_offset;
}

int decode_hybi(unsigned char *src, size_t srclength,
                u_char *target, size_t targsize,
                unsigned int *opcode, unsigned int *left)
{
    unsigned char *frame, *mask, *payload, save_char;
    char cntstr[4];
    int masked = 0;
    int i = 0, len, framecount = 0;
    size_t remaining = 0;
    unsigned int target_offset = 0, hdr_length = 0, payload_length = 0;

    *left = srclength;
    frame = src;

    //printf("Deocde new frame\n");
    while (1) {
        // Need at least two bytes of the header
        // Find beginning of next frame. First time hdr_length, masked and
        // payload_length are zero
        frame += hdr_length + 4*masked + payload_length;
        //printf("frame[0..3]: 0x%x 0x%x 0x%x 0x%x (tot: %d)\n",
        //       (unsigned char) frame[0],
        //       (unsigned char) frame[1],
        //       (unsigned char) frame[2],
        //       (unsigned char) frame[3], srclength);

        if (frame > src + srclength) {
            //printf("Truncated frame from client, need %d more bytes\n", frame - (src + srclength) );
            break;
        }
        remaining = (src + srclength) - frame;
        if (remaining < 2) {
            //printf("Truncated frame header from client\n");
            break;
        }
        framecount ++;

        *opcode = frame[0] & 0x0f;
        masked = (frame[1] & 0x80) >> 7;

        if (*opcode == 0x8) {
            // client sent orderly close frame
            break;
        }

        payload_length = frame[1] & 0x7f;
        if (payload_length < 126) {
            hdr_length = 2;
            //frame += 2 * sizeof(char);
        } else if (payload_length == 126) {
            payload_length = (frame[2] << 8) + frame[3];
            hdr_length = 4;
        } else {
            handler_emsg("Receiving frames larger than 65535 bytes not supported\n");
            return -1;
        }
        if ((hdr_length + 4*masked + payload_length) > remaining) {
            continue;
        }
        //printf("    payload_length: %u, raw remaining: %u\n", payload_length, remaining);
        payload = frame + hdr_length + 4*masked;

        if (*opcode != OPCODE_TEXT && *opcode != OPCODE_BINARY) {
            handler_msg("Ignoring non-data frame, opcode 0x%x\n", *opcode);
            continue;
        }

        if (payload_length == 0) {
            handler_msg("Ignoring empty frame\n");
            continue;
        }

        if ((payload_length > 0) && (!masked)) {
            handler_emsg("Received unmasked payload from client\n");
            return -1;
        }

        // Terminate with a null for base64 decode
        save_char = payload[payload_length];
        payload[payload_length] = '\0';

        // unmask the data
        mask = payload - 4;
        for (i = 0; i < payload_length; i++) {
            payload[i] ^= mask[i%4];
        }

        if (*opcode & OPCODE_TEXT) {
            // base64 decode the data
            len = ws_b64_pton((const char*)payload, target+target_offset, targsize);
        } else {
            memcpy(target+target_offset, payload, payload_length);
            len = payload_length;
        }

        // Restore the first character of the next frame
        payload[payload_length] = save_char;
        if (len < 0) {
            handler_emsg("Base64 decode error code %d", len);
            return len;
        }
        target_offset += len;

        //printf("    len %d, raw %s\n", len, frame);
    }

    if (framecount > 1) {
        snprintf(cntstr, 3, "%d", framecount);
        traffic(cntstr);
    }

    *left = remaining;
    return target_offset;
}



int parse_handshake(ws_ctx_t *ws_ctx, char *handshake) {
    char *start, *end;
    headers_t *headers = ws_ctx->headers;
    const uint8_t extralogs = !!strstr(handshake, "/websockify");
    #define err(x) if (extralogs) { wserr("/websockify request failed websocket checks, " x "\n"); }

    headers->key1[0] = '\0';
    headers->key2[0] = '\0';
    headers->key3[0] = '\0';

    if ((strlen(handshake) < 92) || (bcmp(handshake, "GET ", 4) != 0) ||
        (!strcasestr(handshake, "Upgrade: websocket"))) {
        err("not a GET request, or missing Upgrade header");
        return 0;
    }
    start = handshake+4;
    end = strstr(start, " HTTP/1.1");
    if (!end) { err("not HTTP"); return 0; }
    strncpy(headers->path, start, end-start);
    headers->path[end-start] = '\0';

    start = strstr(handshake, "\r\nHost: ");
    if (!start) { err("missing Host header"); return 0; }
    start += 8;
    end = strstr(start, "\r\n");
    strncpy(headers->host, start, end-start);
    headers->host[end-start] = '\0';

    headers->origin[0] = '\0';
    start = strcasestr(handshake, "\r\nOrigin: ");
    if (start) {
        start += 10;
    } else {
        start = strcasestr(handshake, "\r\nSec-WebSocket-Origin: ");
        if (!start) { err("missing Sec-WebSocket-Origin header"); return 0; }
        start += 24;
    }
    end = strstr(start, "\r\n");
    strncpy(headers->origin, start, end-start);
    headers->origin[end-start] = '\0';

    start = strcasestr(handshake, "\r\nSec-WebSocket-Version: ");
    if (start) {
        // HyBi/RFC 6455
        start += 25;
        end = strstr(start, "\r\n");
        strncpy(headers->version, start, end-start);
        headers->version[end-start] = '\0';
        ws_ctx->hixie = 0;
        ws_ctx->hybi = strtol(headers->version, NULL, 10);

        start = strcasestr(handshake, "\r\nSec-WebSocket-Key: ");
        if (!start) { err("missing Sec-WebSocket-Key header"); return 0; }
        start += 21;
        end = strstr(start, "\r\n");
        strncpy(headers->key1, start, end-start);
        headers->key1[end-start] = '\0';

        start = strstr(handshake, "\r\nConnection: ");
        if (!start) { err("missing Connection header"); return 0; }
        start += 14;
        end = strstr(start, "\r\n");
        strncpy(headers->connection, start, end-start);
        headers->connection[end-start] = '\0';

        start = strcasestr(handshake, "\r\nSec-WebSocket-Protocol: ");
        if (!start) { err("missing Sec-WebSocket-Protocol header"); return 0; }
        start += 26;
        end = strstr(start, "\r\n");
        strncpy(headers->protocols, start, end-start);
        headers->protocols[end-start] = '\0';
    } else {
        // Hixie 75 or 76
        ws_ctx->hybi = 0;

        start = strstr(handshake, "\r\n\r\n");
        if (!start) { err("headers too large"); return 0; }
        start += 4;
        if (strlen(start) == 8) {
            ws_ctx->hixie = 76;
            strncpy(headers->key3, start, 8);
            headers->key3[8] = '\0';

            start = strcasestr(handshake, "\r\nSec-WebSocket-Key1: ");
            if (!start) { err("missing Sec-WebSocket-Key1 header"); return 0; }
            start += 22;
            end = strstr(start, "\r\n");
            strncpy(headers->key1, start, end-start);
            headers->key1[end-start] = '\0';

            start = strcasestr(handshake, "\r\nSec-WebSocket-Key2: ");
            if (!start) { err("missing Sec-WebSocket-Key2 header"); return 0; }
            start += 22;
            end = strstr(start, "\r\n");
            strncpy(headers->key2, start, end-start);
            headers->key2[end-start] = '\0';
        } else {
            ws_ctx->hixie = 75;
        }

    }

    #undef err

    return 1;
}

int parse_hixie76_key(char * key) {
    unsigned long i, spaces = 0, num = 0;
    for (i=0; i < strlen(key); i++) {
        if (key[i] == ' ') {
            spaces += 1;
        }
        if ((key[i] >= 48) && (key[i] <= 57)) {
            num = num * 10 + (key[i] - 48);
        }
    }
    return num / spaces;
}

int gen_md5(headers_t *headers, char *target) {
    unsigned long key1 = parse_hixie76_key(headers->key1);
    unsigned long key2 = parse_hixie76_key(headers->key2);
    char *key3 = headers->key3;

    MD5_CTX c;
    char in[HIXIE_MD5_DIGEST_LENGTH] = {
        key1 >> 24, key1 >> 16, key1 >> 8, key1,
        key2 >> 24, key2 >> 16, key2 >> 8, key2,
        key3[0], key3[1], key3[2], key3[3],
        key3[4], key3[5], key3[6], key3[7]
    };

    MD5_Init(&c);
    MD5_Update(&c, (void *)in, sizeof in);
    MD5_Final((void *)target, &c);

    target[HIXIE_MD5_DIGEST_LENGTH] = '\0';

    return 1;
}

static void gen_sha1(headers_t *headers, char *target) {
    SHA_CTX c;
    unsigned char hash[SHA_DIGEST_LENGTH];
    //int r;

    SHA1_Init(&c);
    SHA1_Update(&c, headers->key1, strlen(headers->key1));
    SHA1_Update(&c, HYBI_GUID, 36);
    SHA1_Final(hash, &c);

    /*r =*/ ws_b64_ntop(hash, sizeof hash, target, HYBI10_ACCEPTHDRLEN);
    //assert(r == HYBI10_ACCEPTHDRLEN - 1);
}

static const char *name2mime(const char *name) {

    const char *end = strrchr(name, '.');
    if (!end)
        goto def;
    end++;

    // Everything under Downloads/ should be treated as binary
    if (strcasestr(name, "Downloads/"))
        goto def;

    #define CMP(s) if (!strncmp(end, s, sizeof(s) - 1))

    CMP("htm")
        return "text/html";
    CMP("txt")
        return "text/plain";

    CMP("css")
        return "text/css";
    CMP("js")
        return "application/javascript";

    CMP("gif")
        return "image/gif";
    CMP("jpg")
        return "image/jpeg";
    CMP("jpeg")
        return "image/jpeg";
    CMP("png")
        return "image/png";
    CMP("svg")
        return "image/svg+xml";

    #undef CMP

def:
    return "application/octet-stream";
}

static uint8_t isValidIp(const char *str, const unsigned len) {
    unsigned i;

    // Just a quick check for now
    for (i = 0; i < len; i++) {
        if (!isxdigit(str[i]) && str[i] != '.' && str[i] != ':')
            return 0;
    }

    return 1;
}

static void dirlisting(ws_ctx_t *ws_ctx, const char fullpath[], const char path[],
                       const char * const user, const char * const ip,
                       const char * const origip) {
    char buf[4096];
    char enc[PATH_MAX * 3 + 1];
    unsigned i;

    // Redirect?
    const unsigned len = strlen(fullpath);
    if (fullpath[len - 1] != '/') {
        sprintf(buf, "HTTP/1.1 301 Moved Permanently\r\n"
                     "Server: KasmVNC/4.0\r\n"
                     "Location: %s/\r\n"
                     "Connection: close\r\n"
                     "Content-type: text/plain\r\n"
                     "%s"
                     "\r\n", path, extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        weblog(301, wsthread_handler_id, 0, origip, ip, user, 1, path, strlen(buf));
        return;
    }

    const char *start = memrchr(path, '/', len - 1);
    if (!start) start = path;

    // Directory listing
    sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/html\r\n"
                 "%s"
                 "\r\n<html><title>Directory Listing</title><body><h2>%s</h2><hr><ul>",
                 extra_headers ? extra_headers : "", path);
    ws_send(ws_ctx, buf, strlen(buf));
    unsigned totallen = strlen(buf);

    struct dirent **names;
    const unsigned num = scandir(fullpath, &names, NULL, alphasort);

    for (i = 0; i < num; i++) {
        if (!strcmp(names[i]->d_name, ".") || !strcmp(names[i]->d_name, ".."))
            continue;

        percent_encode(names[i]->d_name, enc);

        if (names[i]->d_type == DT_DIR)
	        sprintf(buf, "<li><a href=\"%s/\">%s/</a></li>", enc,
	                names[i]->d_name);
        else
	        sprintf(buf, "<li><a href=\"%s\">%s</a></li>", enc,
	                names[i]->d_name);

        ws_send(ws_ctx, buf, strlen(buf));
        totallen += strlen(buf);
    }

    sprintf(buf, "</ul></body></html>");
    ws_send(ws_ctx, buf, strlen(buf));
    totallen += strlen(buf);
    weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, path, totallen);
}

static void servefile(ws_ctx_t *ws_ctx, const char *in, const char * const user,
                      const char * const ip, const char * const origip) {
    char buf[4096], path[4096], fullpath[4096];

    //fprintf(stderr, "http servefile input '%s'\n", in);

    if (strncmp(in, "GET ", 4)) {
        handler_msg("non-GET request, rejecting\n");
        goto nope;
    }
    in += 4;
    const char *end = strchr(in, ' ');
    unsigned len = end - in;

    if (len < 1 || len > 1024 || strstr(in, "../")) {
        handler_msg("Request too long (%u) or attempted dir traversal attack, rejecting\n", len);
        goto nope;
    }

    end = memchr(in, '?', len);
    if (end)
        len = end - in;

    memcpy(path, in, len);
    path[len] = '\0';
    if (!strcmp(path, "/")) {
        strcpy(path, "/index.html");
        len = strlen(path);
    }

    percent_decode(path, buf, 1);

    // in case they percent-encoded dots
    if (strstr(buf, "../")) {
        handler_msg("Attempted dir traversal attack, rejecting\n", len);
        goto nope;
    }

    handler_msg("Requested file '%s'\n", buf);
    sprintf(fullpath, "%s/%s", settings.httpdir, buf);

    DIR *dir = opendir(fullpath);
    if (dir) {
        closedir(dir);
        dirlisting(ws_ctx, fullpath, buf, user, ip, origip);
        return;
    }

    FILE *f = fopen(fullpath, "r");
    if (!f) {
        handler_msg("file not found or insufficient permissions\n");
        goto nope;
    }

    fseeko(f, 0, SEEK_END);
    const uint64_t filesize = ftello(f);
    rewind(f);

    sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: %s\r\n"
                 "Content-length: %" PRIu64 "\r\n"
                 "%s"
                 "\r\n",
                 name2mime(path), filesize, extra_headers ? extra_headers : "");
    const unsigned hdrlen = strlen(buf);
    ws_send(ws_ctx, buf, hdrlen);

    //fprintf(stderr, "http servefile output '%s'\n", buf);

    unsigned count;
    while ((count = fread(buf, 1, 4096, f))) {
        ws_send(ws_ctx, buf, count);
    }
    fclose(f);

    weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, path, hdrlen + filesize);

    return;
nope:
    sprintf(buf, "HTTP/1.1 404 Not Found\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "%s"
                 "\r\n"
                 "404", extra_headers ? extra_headers : "");
    ws_send(ws_ctx, buf, strlen(buf));
    weblog(404, wsthread_handler_id, 0, origip, ip, user, 1, path, strlen(buf));
}

static uint8_t allUsersPresent(const struct kasmpasswd_t * const inset) {
    struct kasmpasswd_t *fullset = readkasmpasswd(settings.passwdfile);
    if (!fullset->num) {
        free(fullset);
        return 0;
    }

    unsigned f, i;
    for (i = 0; i < inset->num; i++) {
        uint8_t found = 0;
        for (f = 0; f < fullset->num; f++) {
            if (!strcmp(inset->entries[i].user, fullset->entries[f].user)) {
                found = 1;
                break;
            }
        }

        if (!found)
            goto notfound;
    }

    free(fullset->entries);
    free(fullset);

    return 1;

notfound:

    free(fullset->entries);
    free(fullset);

    return 0;
}

static void send403(ws_ctx_t *ws_ctx, const char * const origip, const char * const ip) {
    char buf[4096];
    sprintf(buf, "HTTP/1.1 403 Forbidden\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "%s"
                 "\r\n"
                 "403 Forbidden", extra_headers ? extra_headers : "");
    ws_send(ws_ctx, buf, strlen(buf));
    weblog(403, wsthread_handler_id, 0, origip, ip, "-", 1, "-", strlen(buf));
}

static void send400(ws_ctx_t *ws_ctx, const char * const origip, const char * const ip,
                    const char *info) {
    char buf[4096];
    sprintf(buf, "HTTP/1.1 400 Bad Request\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "%s"
                 "\r\n"
                 "400 Bad Request%s", extra_headers ? extra_headers : "", info);
    ws_send(ws_ctx, buf, strlen(buf));
    weblog(400, wsthread_handler_id, 0, origip, ip, "-", 1, "-", strlen(buf));
}

static uint8_t ownerapi_post(ws_ctx_t *ws_ctx, const char *in, const char * const user,
                             const char * const ip, const char * const origip) {
    char buf[4096], path[4096];
    uint8_t ret = 0; // 0 = continue checking

    in += 5;
    const char *end = strchr(in, ' ');
    unsigned len = end - in;

    if (len < 1 || len > 1024 || strstr(in, "../")) {
        handler_msg("Request too long (%u) or attempted dir traversal attack, rejecting\n", len);
        return 0;
    }

    end = memchr(in, '?', len);
    if (end) {
        handler_msg("Attempted GET params in a POST request, rejecting\n");
        return 0;
    }

    memcpy(path, in, len);
    path[len] = '\0';

    handler_msg("Requested owner POST api '%s'\n", path);

    in = strstr(in, "\r\n\r\n");
    if (!in) {
        handler_msg("No content\n");
        return 0;
    }
    in += 4;

    #define entry(x) if (!strcmp(path, x))

    struct kasmpasswd_t *set = NULL;
    unsigned s;

    entry("/api/create_user") {
        set = parseJsonUsers(in);
        if (!set) {
            wserr("JSON parse error\n");
            goto nope;
        }

        for (s = 0; s < set->num; s++) {
            if (!set->entries[s].user[0] || !set->entries[s].password[0]) {
                wserr("Username or password missing\n");
                goto nope;
            }

            struct crypt_data cdata;
            cdata.initialized = 0;

            const char *encrypted = crypt_r(set->entries[s].password, "$5$kasm$", &cdata);
            strcpy(set->entries[s].password, encrypted);

            if (!settings.addOrUpdateUserCb(settings.messager, &set->entries[s])) {
                wserr("Couldn't add or update user\n");
                goto nope;
            }
        }

        free(set->entries);
        free(set);

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "%s"
                 "\r\n"
                 "200 OK", extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 0, path, strlen(buf));

        ret = 1;
    } else entry("/api/remove_user") {
        set = parseJsonUsers(in);
        if (!set) {
            wserr("JSON parse error\n");
            goto nope;
        }

        if (!allUsersPresent(set)) {
            wserr("One or more users didn't exist\n");
            goto nope;
        }

        for (s = 0; s < set->num; s++) {
            if (!set->entries[s].user[0]) {
                wserr("Username missing\n");
                goto nope;
            }

            if (!settings.removeCb(settings.messager, set->entries[s].user)) {
                wserr("Invalid params to remove_user\n");
                goto nope;
            }
        }

        free(set->entries);
        free(set);

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "%s"
                 "\r\n"
                 "200 OK", extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 0, path, strlen(buf));

        ret = 1;
    } else entry("/api/update_user") {
        set = parseJsonUsers(in);
        if (!set) {
            wserr("JSON parse error\n");
            goto nope;
        }

        if (!allUsersPresent(set)) {
            wserr("One or more users didn't exist\n");
            goto nope;
        }

        for (s = 0; s < set->num; s++) {
            if (!set->entries[s].user[0]) {
                wserr("Username missing\n");
                goto nope;
            }

            uint64_t mask = USER_UPDATE_READ_MASK | USER_UPDATE_WRITE_MASK |
                            USER_UPDATE_OWNER_MASK;

            if (set->entries[s].password[0]) {
                struct crypt_data cdata;
                cdata.initialized = 0;

                const char *encrypted = crypt_r(set->entries[s].password, "$5$kasm$", &cdata);
                strcpy(set->entries[s].password, encrypted);

                mask |= USER_UPDATE_PASSWORD_MASK;
            }

            if (!settings.updateUserCb(settings.messager, set->entries[s].user, mask,
                                       set->entries[s].password,
                                       set->entries[s].read,
                                       set->entries[s].write, set->entries[s].owner)) {
                handler_msg("Invalid params to update_user\n");
                goto nope;
            }
        }

        free(set->entries);
        free(set);

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "%s"
                 "\r\n"
                 "200 OK", extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 0, path, strlen(buf));

        ret = 1;
    }

    #undef entry

    return ret;
nope:
    sprintf(buf, "HTTP/1.1 400 Bad Request\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "%s"
                 "\r\n"
                 "400 Bad Request", extra_headers ? extra_headers : "");
    ws_send(ws_ctx, buf, strlen(buf));
    weblog(400, wsthread_handler_id, 0, origip, ip, user, 0, path, strlen(buf));
    return 1;
}

static uint8_t ownerapi(ws_ctx_t *ws_ctx, const char *in, const char * const user,
                        const char * const ip, const char * const origip) {
    char buf[4096], path[4096], args[4096] = "", origpath[4096];
    uint8_t ret = 0; // 0 = continue checking

    if (strncmp(in, "GET ", 4)) {
        if (!strncmp(in, "POST ", 5))
            return ownerapi_post(ws_ctx, in, user, ip, origip);

        handler_msg("non-GET, non-POST request, rejecting\n");
        return 0;
    }
    in += 4;
    const char *end = strchr(in, ' ');
    unsigned len = end - in;

    if (len < 1 || len > 1024 || strstr(in, "../")) {
        handler_msg("Request too long (%u) or attempted dir traversal attack, rejecting\n", len);
        return 0;
    }

    memcpy(origpath, in, len);
    origpath[len] = '\0';

    end = memchr(in, '?', len);
    if (end) {
        len = end - in;
        end++;

        const char *argend = strchr(end, ' ');
        strncpy(args, end, argend - end);
        args[argend - end] = '\0';
    }

    memcpy(path, in, len);
    path[len] = '\0';

    if (strstr(args, "password=")) {
        handler_msg("Requested owner api '%s' with args (skipped, includes password)\n", path);
    } else {
        handler_msg("Requested owner api '%s' with args '%s'\n", path, args);
    }

    #define entry(x) if (!strcmp(path, x))

    const char *param;

    entry("/api/get_screenshot") {
        uint8_t q = 7, dedup = 0;
        uint16_t w = 4096, h = 4096;

        param = parse_get(args, "width", &len);
        if (len && isdigit(param[0]))
            w = atoi(param);

        param = parse_get(args, "height", &len);
        if (len && isdigit(param[0]))
            h = atoi(param);

        param = parse_get(args, "quality", &len);
        if (len && isdigit(param[0]))
            q = atoi(param);

        param = parse_get(args, "deduplicate", &len);
        if (len && isalpha(param[0])) {
            if (!strncmp(param, "true", len))
                dedup = 1;
        }

        uint8_t *staging = malloc(1024 * 1024 * 8);

        settings.screenshotCb(settings.messager, w, h, q, dedup, &len, staging);

        if (len == 16) {
            sprintf(buf, "HTTP/1.1 200 OK\r\n"
                     "Server: KasmVNC/4.0\r\n"
                     "Connection: close\r\n"
                     "Content-type: text/plain\r\n"
                     "Content-length: %u\r\n"
                     "%s"
                     "\r\n", len, extra_headers ? extra_headers : "");
            ws_send(ws_ctx, buf, strlen(buf));
            ws_send(ws_ctx, staging, len);
            weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf) + len);

            handler_msg("Screenshot hadn't changed and dedup was requested, sent hash\n");
            ret = 1;
        } else if (len) {
            sprintf(buf, "HTTP/1.1 200 OK\r\n"
                     "Server: KasmVNC/4.0\r\n"
                     "Connection: close\r\n"
                     "Content-type: image/jpeg\r\n"
                     "Content-length: %u\r\n"
                     "%s"
                     "\r\n", len, extra_headers ? extra_headers : "");
            ws_send(ws_ctx, buf, strlen(buf));
            ws_send(ws_ctx, staging, len);
            weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf) + len);

            handler_msg("Sent screenshot %u bytes\n", len);
            ret = 1;
        }

        free(staging);

        if (!len) {
            handler_msg("Invalid params to screenshot\n");
            goto nope;
        }
    } else entry("/api/create_user") {
        char decname[1024] = "", decpw[1024] = "";
        uint8_t read = 0, write = 0, owner = 0;

        param = parse_get(args, "name", &len);
        if (len) {
            memcpy(buf, param, len);
            buf[len] = '\0';
            percent_decode(buf, decname, 0);
        }

        param = parse_get(args, "password", &len);
        if (len) {
            memcpy(buf, param, len);
            buf[len] = '\0';
            percent_decode(buf, decpw, 0);

            struct crypt_data cdata;
            cdata.initialized = 0;

            const char *encrypted = crypt_r(decpw, "$5$kasm$", &cdata);
            strcpy(decpw, encrypted);
        }

        param = parse_get(args, "read", &len);
        if (len && isalpha(param[0])) {
            if (!strncmp(param, "true", len))
                read = 1;
        }

        param = parse_get(args, "write", &len);
        if (len && isalpha(param[0])) {
            if (!strncmp(param, "true", len))
                write = 1;
        }

        param = parse_get(args, "owner", &len);
        if (len && isalpha(param[0])) {
            if (!strncmp(param, "true", len))
                owner = 1;
        }

        if (!decname[0] || !decpw[0])
            goto nope;

        if (!settings.adduserCb(settings.messager, decname, decpw, read, write, owner)) {
            handler_msg("Invalid params to create_user\n");
            goto nope;
        }

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "%s"
                 "\r\n"
                 "200 OK", extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf));

        ret = 1;
    } else entry("/api/remove_user") {
        char decname[1024] = "";

        param = parse_get(args, "name", &len);
        if (len) {
            memcpy(buf, param, len);
            buf[len] = '\0';
            percent_decode(buf, decname, 0);
        }

        if (!decname[0])
            goto nope;

        if (!settings.removeCb(settings.messager, decname)) {
            handler_msg("Invalid params to remove_user\n");
            goto nope;
        }

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "%s"
                 "\r\n"
                 "200 OK", extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf));

        ret = 1;
    } else entry("/api/update_user") {
        char decname[1024] = "";

        param = parse_get(args, "name", &len);
        if (len) {
            memcpy(buf, param, len);
            buf[len] = '\0';
            percent_decode(buf, decname, 0);
        }

        if (!decname[0])
            goto nope;

	uint64_t mask = 0;
        uint8_t myread = 0;
        param = parse_get(args, "read", &len);
        if (len && isalpha(param[0])) {
            mask |= USER_UPDATE_READ_MASK;
            if (!strncmp(param, "true", len))
                myread = 1;
        }

        uint8_t mywrite = 0;
        param = parse_get(args, "write", &len);
        if (len && isalpha(param[0])) {
            mask |= USER_UPDATE_WRITE_MASK;
            if (!strncmp(param, "true", len))
                mywrite = 1;
        }

        uint8_t myowner = 0;
        param = parse_get(args, "owner", &len);
        if (len && isalpha(param[0])) {
            mask |= USER_UPDATE_OWNER_MASK;
            if (!strncmp(param, "true", len))
                myowner = 1;
        }

        if (!settings.updateUserCb(settings.messager, decname, mask, "",
                                   myread, mywrite, myowner)) {
            handler_msg("Invalid params to update_user\n");
            goto nope;
        }

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "%s"
                 "\r\n"
                 "200 OK", extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf));

        ret = 1;
    } else entry("/api/get_bottleneck_stats") {
        char statbuf[4096];
        settings.bottleneckStatsCb(settings.messager, statbuf, 4096);

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: %lu\r\n"
                 "%s"
                 "\r\n", strlen(statbuf), extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        ws_send(ws_ctx, statbuf, strlen(statbuf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf) + strlen(statbuf));

        handler_msg("Sent bottleneck stats to API caller\n");
        ret = 1;
    } else entry("/api/get_users") {
        const char *ptr;
        settings.getUsersCb(settings.messager, &ptr);

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: %lu\r\n"
                 "%s"
                 "\r\n", strlen(ptr), extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        ws_send(ws_ctx, ptr, strlen(ptr));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf) + strlen(ptr));

        free((char *) ptr);

        handler_msg("Sent user list to API caller\n");
        ret = 1;
    } else entry("/api/get_frame_stats") {
        char statbuf[4096], decname[1024];
        unsigned waitfor;

        param = parse_get(args, "client", &len);
        if (len) {
            memcpy(buf, param, len);
            buf[len] = '\0';
            percent_decode(buf, decname, 0);
        } else {
            handler_msg("client param required\n");
            goto nope;
        }

        if (!decname[0])
            goto nope;

        if (!strcmp(decname, "none")) {
            waitfor = 0;
            if (!settings.requestFrameStatsNoneCb(settings.messager))
                goto nope;
        } else if (!strcmp(decname, "auto")) {
            waitfor = settings.ownerConnectedCb(settings.messager);
            if (!waitfor) {
                if (!settings.requestFrameStatsNoneCb(settings.messager))
                    goto nope;
            } else {
                if (!settings.requestFrameStatsOwnerCb(settings.messager))
                    goto nope;
            }
        } else if (!strcmp(decname, "all")) {
            waitfor = settings.numActiveUsersCb(settings.messager);
            if (!settings.requestFrameStatsAllCb(settings.messager))
                goto nope;
        } else {
            waitfor = 1;
            if (!settings.requestFrameStatsOneCb(settings.messager, decname))
                goto nope;
        }

        unsigned waits;
        {
            uint8_t failed = 1;
            for (waits = 0; waits < 500; waits++) { // wait up to 10s
                usleep(20 * 1000);
                if (settings.serverFrameStatsReadyCb(settings.messager)) {
                    failed = 0;
                    break;
                }
            }

            if (failed) {
                handler_msg("Main thread didn't respond, aborting (bug, if nothing happened for 10s)\n");
                settings.resetFrameStatsCb(settings.messager);
                goto timeout;
            }
        }

        if (waitfor) {
            for (waits = 0; waits < 20; waits++) { // wait up to 2s
                if (settings.getClientFrameStatsNumCb(settings.messager) >= waitfor)
                    break;
                usleep(100 * 1000);
            }
        }

        settings.frameStatsCb(settings.messager, statbuf, 4096);

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: %lu\r\n"
                 "%s"
                 "\r\n", strlen(statbuf), extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        ws_send(ws_ctx, statbuf, strlen(statbuf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf) + strlen(statbuf));

        handler_msg("Sent frame stats to API caller\n");
        ret = 1;
    } else entry("/api/send_full_frame") {
        write(wakeuppipe[1], "", 1);

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "%s"
                 "\r\n"
                 "200 OK", extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf));

        ret = 1;
    } else entry("/api/clear_clipboard") {
        settings.clearClipboardCb(settings.messager);
        write(wakeuppipe[1], "", 1);

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "%s"
                 "\r\n"
                 "200 OK", extra_headers ? extra_headers : "");
        ws_send(ws_ctx, buf, strlen(buf));
        weblog(200, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf));

        ret = 1;
    }

    #undef entry

    return ret;

nope:
    sprintf(buf, "HTTP/1.1 400 Bad Request\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "%s"
                 "\r\n"
                 "400 Bad Request", extra_headers ? extra_headers : "");
    ws_send(ws_ctx, buf, strlen(buf));
    weblog(400, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf));
    return 1;

timeout:
    sprintf(buf, "HTTP/1.1 503 Service Unavailable\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "%s"
                 "\r\n"
                 "503 Service Unavailable", extra_headers ? extra_headers : "");
    ws_send(ws_ctx, buf, strlen(buf));
    weblog(503, wsthread_handler_id, 0, origip, ip, user, 1, origpath, strlen(buf));
    return 1;
}

ws_ctx_t *do_handshake(int sock, char * const ip) {
    char handshake[16 * 1024], response[4096], sha1[29], trailer[17];
    char *scheme, *pre;
    headers_t *headers;
    int len, i, offset;
    ws_ctx_t * ws_ctx;
    char *response_protocol;

    // Peek, but don't read the data
    len = recv(sock, handshake, 1024, MSG_PEEK);
    handshake[len] = 0;
    if (len == 0) {
        handler_msg("ignoring empty handshake\n");
        return NULL;
    } else if ((bcmp(handshake, "\x16", 1) == 0) ||
               (bcmp(handshake, "\x80", 1) == 0)) {
        // SSL
        if (!settings.cert) {
            handler_msg("SSL connection but no cert specified\n");
            return NULL;
        } else if (access(settings.cert, R_OK) != 0) {
            handler_msg("SSL connection but '%s' not found\n",
                        settings.cert);
            return NULL;
        }
        ws_ctx = alloc_ws_ctx();
        ws_socket_ssl(ws_ctx, sock, settings.cert, settings.key);
        if (! ws_ctx) { return NULL; }
        scheme = "wss";
        handler_msg("using SSL socket\n");
    } else if (settings.ssl_only) {
        handler_msg("non-SSL connection disallowed\n");
        return NULL;
    } else {
        ws_ctx = alloc_ws_ctx();
        ws_socket(ws_ctx, sock);
        if (! ws_ctx) { return NULL; }
        scheme = "ws";
        handler_msg("using plain (not SSL) socket\n");
    }
    offset = 0;
    for (i = 0; i < 10; i++) {
        /* (offset + 1): reserve one byte for the trailing '\0' */
        if (0 > (len = ws_recv(ws_ctx, handshake + offset, sizeof(handshake) - (offset + 1)))) {
            handler_emsg("Read error during handshake: %m\n");
            free_ws_ctx(ws_ctx);
            return NULL;
        } else if (0 == len) {
            handler_emsg("Client closed during handshake\n");
            free_ws_ctx(ws_ctx);
            return NULL;
        }
        offset += len;
        handshake[offset] = 0;
        if (strstr(handshake, "\r\n\r\n")) {
            break;
        } else if (sizeof(handshake) <= (size_t)(offset + 1)) {
            handler_emsg("Oversized handshake\n");
            send400(ws_ctx, "-", ip, ", too large");
            free_ws_ctx(ws_ctx);
            return NULL;
        } else if (9 == i) {
            handler_emsg("Incomplete handshake\n");
            free_ws_ctx(ws_ctx);
            return NULL;
        }
        usleep(10);
    }

    // Proxied?
    char origip[64];
    memcpy(origip, ip, 64);
    const char *fwd = strcasestr(handshake, "X-Forwarded-For: ");
    if (fwd) {
        fwd += 17;
        const char *end = strchr(fwd, '\r');
        const char *comma = memchr(fwd, ',', end - fwd);
        if (comma)
            end = comma;

        // Sanity checks, in case it's malicious input
        if (!isValidIp(fwd, end - fwd) || (end - fwd) >= 64) {
            wserr("An invalid X-Forwarded-For was passed, maybe an attack\n");
        } else {
            memcpy(ip, fwd, end - fwd);
            ip[end - fwd] = '\0';
            handler_msg("X-Forwarded-For ip '%s'\n", ip);
        }
    }

    // Early URL parsing for the auth denies
    char url[2048] = "-";
    const char *end = strchr(handshake + 4, ' ');
    if (end) {
        len = end - (handshake + 4);
        if (len > 1024)
            len = 1024;

        memcpy(url, handshake + 4, len);
        url[len] = '\0';
    }

    if (bl_isBlacklisted(ip)) {
        wserr("IP %s is blacklisted, dropping\n", ip);
        sprintf(response, "HTTP/1.1 401 Forbidden\r\n"
                          "\r\n");
        ws_send(ws_ctx, response, strlen(response));
        weblog(401, wsthread_handler_id, 0, origip, ip, "-", 1, url, strlen(response));
        free_ws_ctx(ws_ctx);
        return NULL;
    }

    unsigned char owner = 0;
    char inuser[USERNAME_LEN] = "-";
    if (!settings.disablebasicauth) {
        const char *hdr = strstr(handshake, "Authorization: Basic ");
        if (!hdr) {
            bl_addFailure(ip);
            wserr("Authentication attempt failed, BasicAuth required, but client didn't send any\n");
            sprintf(response, "HTTP/1.1 401 Unauthorized\r\n"
                              "WWW-Authenticate: Basic realm=\"Websockify\"\r\n"
                              "%s"
                              "\r\n", extra_headers ? extra_headers : "");
            ws_send(ws_ctx, response, strlen(response));
            weblog(401, wsthread_handler_id, 0, origip, ip, "-", 1, url, strlen(response));
            free_ws_ctx(ws_ctx);
            return NULL;
        }

        hdr += sizeof("Authorization: Basic ") - 1;
        const char *end = strchr(hdr, '\r');
        if (!end || end - hdr > 256) {
            wserr("Authentication attempt failed, client sent invalid BasicAuth\n");
            bl_addFailure(ip);
            send403(ws_ctx, origip, ip);
            free_ws_ctx(ws_ctx);
            return NULL;
        }
        len = end - hdr;
        char tmp[257];
        memcpy(tmp, hdr, len);
        tmp[len] = '\0';
        len = ws_b64_pton(tmp, response, 256);

        char authbuf[4096] = "";

        // Do we need to read it from the file?
        char *resppw = strchr(response, ':');
        if (resppw && *resppw)
            resppw++;
        if (settings.passwdfile) {
            if (resppw && *resppw && resppw - response < USERNAME_LEN + 1) {
                char pwbuf[4096];
                struct kasmpasswd_t *set = readkasmpasswd(settings.passwdfile);
                if (!set->num) {
                    wserr("Error: BasicAuth configured to read password from file %s, but the file doesn't exist or has no valid users\n",
                            settings.passwdfile);
                } else {
                    unsigned i;
                    unsigned char found = 0;
                    memcpy(inuser, response, resppw - response - 1);
                    inuser[resppw - response - 1] = '\0';

                    for (i = 0; i < set->num; i++) {
                        if (!strcmp(set->entries[i].user, inuser)) {
                            found = 1;
                            strcpy(ws_ctx->user, inuser);
                            snprintf(authbuf, 4096, "%s:%s", set->entries[i].user,
                                     set->entries[i].password);
                            authbuf[4095] = '\0';

                            if (set->entries[i].owner)
                                owner = 1;
                            break;
                        }
                    }

                    if (!found)
                        wserr("Authentication attempt failed, user %s does not exist\n", inuser);
                }
                free(set->entries);
                free(set);

                struct crypt_data cdata;
                cdata.initialized = 0;

                const char *encrypted = crypt_r(resppw, "$5$kasm$", &cdata);
                *resppw = '\0';

                snprintf(pwbuf, 4096, "%s%s", response, encrypted);
                pwbuf[4095] = '\0';
                strcpy(response, pwbuf);
            } else {
                // Client tried an empty password, just fail them
                response[0] = '\0';
                authbuf[0] = 'a';
                authbuf[1] = '\0';
            }
        }

        if (len <= 0 || strcmp(authbuf, response)) {
            wserr("Authentication attempt failed, wrong password for user %s\n", inuser);
            bl_addFailure(ip);
            sprintf(response, "HTTP/1.1 401 Forbidden\r\n"
                              "%s"
                              "\r\n", extra_headers ? extra_headers : "");
            ws_send(ws_ctx, response, strlen(response));
            weblog(401, wsthread_handler_id, 0, origip, ip, inuser, 1, url, strlen(response));
            free_ws_ctx(ws_ctx);
            return NULL;
        }
        handler_emsg("BasicAuth matched\n");
    }

    //handler_msg("handshake: %s\n", handshake);
    if (!parse_handshake(ws_ctx, handshake)) {
        handler_emsg("Invalid WS request, maybe a HTTP one\n");

        if (strstr(handshake, "/api/")) {
            handler_emsg("HTTP request under /api/\n");

            if (owner) {
                if (ownerapi(ws_ctx, handshake, inuser, ip, origip))
                    goto done;
            } else {
                sprintf(response, "HTTP/1.1 401 Unauthorized\r\n"
                        "Server: KasmVNC/4.0\r\n"
                        "Connection: close\r\n"
                        "Content-type: text/plain\r\n"
                        "%s"
                        "\r\n"
                        "401 Unauthorized", extra_headers ? extra_headers : "");
                ws_send(ws_ctx, response, strlen(response));
                weblog(401, wsthread_handler_id, 0, origip, ip, inuser, 1, url, strlen(response));
                goto done;
            }
        }

        if (settings.httpdir && settings.httpdir[0])
            servefile(ws_ctx, handshake, inuser, ip, origip);

done:
        free_ws_ctx(ws_ctx);
        return NULL;
    }

    headers = ws_ctx->headers;

    response_protocol = strtok(headers->protocols, ",");
    if (!response_protocol || !strlen(response_protocol)) {
        ws_ctx->opcode = OPCODE_BINARY;
        response_protocol = "null";
    } else if (!strcmp(response_protocol, "base64")) {
      ws_ctx->opcode = OPCODE_TEXT;
    } else {
        ws_ctx->opcode = OPCODE_BINARY;
    }

    if (ws_ctx->hybi > 0) {
        handler_msg("using protocol HyBi/IETF 6455 %d\n", ws_ctx->hybi);
        gen_sha1(headers, sha1);
        snprintf(response, sizeof(response), SERVER_HANDSHAKE_HYBI, sha1, response_protocol);
    } else {
        if (ws_ctx->hixie == 76) {
            handler_msg("using protocol Hixie 76\n");
            gen_md5(headers, trailer);
            pre = "Sec-";
        } else {
            handler_msg("using protocol Hixie 75\n");
            trailer[0] = '\0';
            pre = "";
        }
        snprintf(response, sizeof(response), SERVER_HANDSHAKE_HIXIE, pre, headers->origin,
                 pre, scheme, headers->host, headers->path, pre, "base64", trailer);
    }

    //handler_msg("response: %s\n", response);
    ws_send(ws_ctx, response, strlen(response));

    return ws_ctx;
}

void proxy_handler(ws_ctx_t *ws_ctx);

__thread unsigned wsthread_handler_id;

void *subthread(void *ptr) {

    struct wspass_t * const pass = ptr;

    const int csock = pass->csock;
    wsthread_handler_id = pass->id;

    ws_ctx_t *ws_ctx;

    ws_ctx = do_handshake(csock, pass->ip);
    if (ws_ctx == NULL) {
        handler_msg("No connection after handshake\n");
        goto out;   // Child process exits
    }

    memcpy(ws_ctx->ip, pass->ip, sizeof(pass->ip));

    proxy_handler(ws_ctx);
    if (pipe_error) {
        handler_emsg("Closing due to SIGPIPE\n");
    }
out:
    free((void *) pass);

    if (ws_ctx) {
        ws_socket_free(ws_ctx);
        free_ws_ctx(ws_ctx);
    } else {
        shutdown(csock, SHUT_RDWR);
        close(csock);
    }
    handler_msg("handler exit\n");

    return NULL;
}

void *start_server(void *unused) {
    int csock;
    struct sockaddr_in cli_addr;
    socklen_t clilen;

//    printf("Waiting for connections on %s:%d\n",
//            settings.listen_host, settings.listen_port);

    while (1) {
        clilen = sizeof(cli_addr);
        pipe_error = 0;
        do {
            csock = accept(settings.listen_sock,
                           (struct sockaddr *) &cli_addr,
                           &clilen);
        } while (csock == -1 && errno == EINTR);

        if (csock < 0) {
            error("ERROR on accept");
            continue;
        }
        struct wspass_t *pass = calloc(1, sizeof(struct wspass_t));
        inet_ntop(cli_addr.sin_family, &cli_addr.sin_addr, pass->ip, sizeof(pass->ip));

        char logbuf[2][1024];
        wslog(logbuf[0], settings.handler_id, 0);
        sprintf(logbuf[1], "got client connection from %s\n",
                    pass->ip);
        fprintf(stderr, "%s%s", logbuf[0], logbuf[1]);

        pthread_t tid;
        pass->id = settings.handler_id;
        pass->csock = csock;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        pthread_create(&tid, &attr, subthread, pass);

        pthread_attr_destroy(&attr);

        settings.handler_id += 1;
    }
    handler_msg("websockify exit\n");

    return NULL;
}
