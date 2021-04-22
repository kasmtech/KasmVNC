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
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#include "kasmpasswd.h"

/*
 * Global state
 *
 *   Warning: not thread safe
 */
int ssl_initialized = 0;
int pipe_error = 0;
settings_t settings;


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

static void percent_decode(const char *src, char *dst) {
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
			*dst++ = strtol(hex, NULL, 16);
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
    free(ctx);
}

ws_ctx_t *ws_socket(ws_ctx_t *ctx, int socket) {
    ctx->sockfd = socket;
    return ctx;
}

ws_ctx_t *ws_socket_ssl(ws_ctx_t *ctx, int socket, char * certfile, char * keyfile) {
    int ret;
    char msg[1024];
    char * use_keyfile;
    ws_socket(ctx, socket);

    if (keyfile && (keyfile[0] != '\0')) {
        // Separate key file
        use_keyfile = keyfile;
    } else {
        // Combined key and cert file
        use_keyfile = certfile;
    }

    // Initialize the library
    if (! ssl_initialized) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        ssl_initialized = 1;

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

int ws_b64_ntop(const unsigned char const * src, size_t srclen, char * dst, size_t dstlen) {
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

int ws_b64_pton(const char const * src, unsigned char * dst, size_t dstlen) {
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

    headers->key1[0] = '\0';
    headers->key2[0] = '\0';
    headers->key3[0] = '\0';

    if ((strlen(handshake) < 92) || (bcmp(handshake, "GET ", 4) != 0) ||
        (!strstr(handshake, "Upgrade: websocket"))) {
        return 0;
    }
    start = handshake+4;
    end = strstr(start, " HTTP/1.1");
    if (!end) { return 0; }
    strncpy(headers->path, start, end-start);
    headers->path[end-start] = '\0';

    start = strstr(handshake, "\r\nHost: ");
    if (!start) { return 0; }
    start += 8;
    end = strstr(start, "\r\n");
    strncpy(headers->host, start, end-start);
    headers->host[end-start] = '\0';

    headers->origin[0] = '\0';
    start = strstr(handshake, "\r\nOrigin: ");
    if (start) {
        start += 10;
    } else {
        start = strstr(handshake, "\r\nSec-WebSocket-Origin: ");
        if (!start) { return 0; }
        start += 24;
    }
    end = strstr(start, "\r\n");
    strncpy(headers->origin, start, end-start);
    headers->origin[end-start] = '\0';

    start = strstr(handshake, "\r\nSec-WebSocket-Version: ");
    if (start) {
        // HyBi/RFC 6455
        start += 25;
        end = strstr(start, "\r\n");
        strncpy(headers->version, start, end-start);
        headers->version[end-start] = '\0';
        ws_ctx->hixie = 0;
        ws_ctx->hybi = strtol(headers->version, NULL, 10);

        start = strstr(handshake, "\r\nSec-WebSocket-Key: ");
        if (!start) { return 0; }
        start += 21;
        end = strstr(start, "\r\n");
        strncpy(headers->key1, start, end-start);
        headers->key1[end-start] = '\0';

        start = strstr(handshake, "\r\nConnection: ");
        if (!start) { return 0; }
        start += 14;
        end = strstr(start, "\r\n");
        strncpy(headers->connection, start, end-start);
        headers->connection[end-start] = '\0';

        start = strstr(handshake, "\r\nSec-WebSocket-Protocol: ");
        if (!start) { return 0; }
        start += 26;
        end = strstr(start, "\r\n");
        strncpy(headers->protocols, start, end-start);
        headers->protocols[end-start] = '\0';
    } else {
        // Hixie 75 or 76
        ws_ctx->hybi = 0;

        start = strstr(handshake, "\r\n\r\n");
        if (!start) { return 0; }
        start += 4;
        if (strlen(start) == 8) {
            ws_ctx->hixie = 76;
            strncpy(headers->key3, start, 8);
            headers->key3[8] = '\0';

            start = strstr(handshake, "\r\nSec-WebSocket-Key1: ");
            if (!start) { return 0; }
            start += 22;
            end = strstr(start, "\r\n");
            strncpy(headers->key1, start, end-start);
            headers->key1[end-start] = '\0';

            start = strstr(handshake, "\r\nSec-WebSocket-Key2: ");
            if (!start) { return 0; }
            start += 22;
            end = strstr(start, "\r\n");
            strncpy(headers->key2, start, end-start);
            headers->key2[end-start] = '\0';
        } else {
            ws_ctx->hixie = 75;
        }

    }

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
    int r;

    SHA1_Init(&c);
    SHA1_Update(&c, headers->key1, strlen(headers->key1));
    SHA1_Update(&c, HYBI_GUID, 36);
    SHA1_Final(hash, &c);

    r = ws_b64_ntop(hash, sizeof hash, target, HYBI10_ACCEPTHDRLEN);
    //assert(r == HYBI10_ACCEPTHDRLEN - 1);
}

static const char *name2mime(const char *name) {

    const char *end = strrchr(name, '.');
    if (!end)
        goto def;
    end++;

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

static void dirlisting(ws_ctx_t *ws_ctx, const char fullpath[], const char path[]) {
    char buf[4096];
    unsigned i;

    // Redirect?
    const unsigned len = strlen(fullpath);
    if (fullpath[len - 1] != '/') {
        sprintf(buf, "HTTP/1.1 301 Moved Permanently\r\n"
                     "Server: KasmVNC/4.0\r\n"
                     "Location: %s/\r\n"
                     "Connection: close\r\n"
                     "Content-type: text/plain\r\n"
                     "\r\n", path);
        ws_send(ws_ctx, buf, strlen(buf));
        return;
    }

    const char *start = memrchr(path, '/', len - 1);
    if (!start) start = path;

    // Directory listing
    sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/html\r\n"
                 "\r\n<html><title>Directory Listing</title><body><h2>%s</h2><hr><ul>", path);
    ws_send(ws_ctx, buf, strlen(buf));

    struct dirent **names;
    const unsigned num = scandir(fullpath, &names, NULL, alphasort);

    for (i = 0; i < num; i++) {
        if (!strcmp(names[i]->d_name, ".") || !strcmp(names[i]->d_name, ".."))
            continue;

        if (names[i]->d_type == DT_DIR)
	        sprintf(buf, "<li><a href=\"%s/\">%s/</a></li>", names[i]->d_name,
	                names[i]->d_name);
        else
	        sprintf(buf, "<li><a href=\"%s\">%s</a></li>", names[i]->d_name,
	                names[i]->d_name);

        ws_send(ws_ctx, buf, strlen(buf));
    }

    sprintf(buf, "</ul></body></html>");
    ws_send(ws_ctx, buf, strlen(buf));
}

static void servefile(ws_ctx_t *ws_ctx, const char *in) {
    char buf[4096], path[4096], fullpath[4096];

    //fprintf(stderr, "http servefile input '%s'\n", in);

    if (strncmp(in, "GET ", 4)) {
        wserr("non-GET request, rejecting\n");
        goto nope;
    }
    in += 4;
    const char *end = strchr(in, ' ');
    unsigned len = end - in;

    if (len < 1 || len > 1024 || strstr(in, "../")) {
        wserr("Request too long (%u) or attempted dir traversal attack, rejecting\n", len);
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

    wserr("Requested file '%s'\n", path);
    sprintf(fullpath, "%s/%s", settings.httpdir, path);

    DIR *dir = opendir(fullpath);
    if (dir) {
        closedir(dir);
        dirlisting(ws_ctx, fullpath, path);
        return;
    }

    FILE *f = fopen(fullpath, "r");
    if (!f) {
        wserr("file not found or insufficient permissions\n");
        goto nope;
    }

    fseek(f, 0, SEEK_END);
    const unsigned filesize = ftell(f);
    rewind(f);

    sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: %s\r\n"
                 "Content-length: %u\r\n"
                 "\r\n",
                 name2mime(path), filesize);
    ws_send(ws_ctx, buf, strlen(buf));

    //fprintf(stderr, "http servefile output '%s'\n", buf);

    unsigned count;
    while ((count = fread(buf, 1, 4096, f))) {
        ws_send(ws_ctx, buf, count);
    }
    fclose(f);

    return;
nope:
    sprintf(buf, "HTTP/1.1 404 Not Found\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "\r\n"
                 "404");
    ws_send(ws_ctx, buf, strlen(buf));
}

static uint8_t ownerapi(ws_ctx_t *ws_ctx, const char *in) {
    char buf[4096], path[4096], args[4096] = "";
    uint8_t ret = 0; // 0 = continue checking

    if (strncmp(in, "GET ", 4)) {
        wserr("non-GET request, rejecting\n");
        return 0;
    }
    in += 4;
    const char *end = strchr(in, ' ');
    unsigned len = end - in;

    if (len < 1 || len > 1024 || strstr(in, "../")) {
        wserr("Request too long (%u) or attempted dir traversal attack, rejecting\n", len);
        return 0;
    }

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
        wserr("Requested owner api '%s' with args (skipped, includes password)\n", path);
    } else {
        wserr("Requested owner api '%s' with args '%s'\n", path, args);
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
                     "\r\n", len);
            ws_send(ws_ctx, buf, strlen(buf));
            ws_send(ws_ctx, staging, len);

            wserr("Screenshot hadn't changed and dedup was requested, sent hash\n");
            ret = 1;
        } else if (len) {
            sprintf(buf, "HTTP/1.1 200 OK\r\n"
                     "Server: KasmVNC/4.0\r\n"
                     "Connection: close\r\n"
                     "Content-type: image/jpeg\r\n"
                     "Content-length: %u\r\n"
                     "\r\n", len);
            ws_send(ws_ctx, buf, strlen(buf));
            ws_send(ws_ctx, staging, len);

            wserr("Sent screenshot %u bytes\n", len);
            ret = 1;
        }

        free(staging);

        if (!len) {
            wserr("Invalid params to screenshot\n");
            goto nope;
        }
    } else entry("/api/create_user") {
        char decname[1024] = "", decpw[1024] = "";
        uint8_t write = 0;

        param = parse_get(args, "name", &len);
        if (len) {
            memcpy(buf, param, len);
            buf[len] = '\0';
            percent_decode(buf, decname);
        }

        param = parse_get(args, "password", &len);
        if (len) {
            memcpy(buf, param, len);
            buf[len] = '\0';
            percent_decode(buf, decpw);

            struct crypt_data cdata;
            cdata.initialized = 0;

            const char *encrypted = crypt_r(decpw, "$5$kasm$", &cdata);
            strcpy(decpw, encrypted);
        }

        param = parse_get(args, "write", &len);
        if (len && isalpha(param[0])) {
            if (!strncmp(param, "true", len))
                write = 1;
        }

        if (!decname[0] || !decpw[0])
            goto nope;

        if (!settings.adduserCb(settings.messager, decname, decpw, write)) {
            wserr("Invalid params to create_user\n");
            goto nope;
        }

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "\r\n"
                 "200 OK");
        ws_send(ws_ctx, buf, strlen(buf));

        ret = 1;
    } else entry("/api/remove_user") {
        char decname[1024] = "";

        param = parse_get(args, "name", &len);
        if (len) {
            memcpy(buf, param, len);
            buf[len] = '\0';
            percent_decode(buf, decname);
        }

        if (!decname[0])
            goto nope;

        if (!settings.removeCb(settings.messager, decname)) {
            wserr("Invalid params to remove_user\n");
            goto nope;
        }

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "\r\n"
                 "200 OK");
        ws_send(ws_ctx, buf, strlen(buf));

        wserr("Passed remove_user request to main thread\n");
        ret = 1;
    } else entry("/api/give_control") {
        char decname[1024] = "";

        param = parse_get(args, "name", &len);
        if (len) {
            memcpy(buf, param, len);
            buf[len] = '\0';
            percent_decode(buf, decname);
        }

        if (!decname[0])
            goto nope;

        if (!settings.givecontrolCb(settings.messager, decname)) {
            wserr("Invalid params to give_control\n");
            goto nope;
        }

        sprintf(buf, "HTTP/1.1 200 OK\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "Content-length: 6\r\n"
                 "\r\n"
                 "200 OK");
        ws_send(ws_ctx, buf, strlen(buf));

        wserr("Passed give_control request to main thread\n");
        ret = 1;
    }

    #undef entry

    return ret;
nope:
    sprintf(buf, "HTTP/1.1 400 Bad Request\r\n"
                 "Server: KasmVNC/4.0\r\n"
                 "Connection: close\r\n"
                 "Content-type: text/plain\r\n"
                 "\r\n"
                 "400 Bad Request");
    ws_send(ws_ctx, buf, strlen(buf));
    return 1;
}

ws_ctx_t *do_handshake(int sock) {
    char handshake[4096], response[4096], sha1[29], trailer[17];
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
            free_ws_ctx(ws_ctx);
            return NULL;
        } else if (9 == i) {
            handler_emsg("Incomplete handshake\n");
            free_ws_ctx(ws_ctx);
            return NULL;
        }
        usleep(10);
    }

    unsigned char owner = 0;
    if (!settings.disablebasicauth) {
        const char *hdr = strstr(handshake, "Authorization: Basic ");
        if (!hdr) {
            handler_emsg("BasicAuth required, but client didn't send any. 401 Unauth\n");
            sprintf(response, "HTTP/1.1 401 Unauthorized\r\n"
                              "WWW-Authenticate: Basic realm=\"Websockify\"\r\n"
                              "\r\n");
            ws_send(ws_ctx, response, strlen(response));
            free_ws_ctx(ws_ctx);
            return NULL;
        }

        hdr += sizeof("Authorization: Basic ") - 1;
        const char *end = strchr(hdr, '\r');
        if (!end || end - hdr > 256) {
            handler_emsg("Client sent invalid BasicAuth, dropping connection\n");
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
            if (resppw && *resppw && resppw - response < 32) {
                char pwbuf[4096];
                struct kasmpasswd_t *set = readkasmpasswd(settings.passwdfile);
                if (!set->num) {
                    fprintf(stderr, " websocket %d: Error: BasicAuth configured to read password from file %s, but the file doesn't exist or has no valid users\n",
                            wsthread_handler_id,
                            settings.passwdfile);
                } else {
                    unsigned i;
                    char inuser[32];
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
                        handler_emsg("BasicAuth user %s not found\n", inuser);
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
            }
        }

        if (len <= 0 || strcmp(authbuf, response)) {
            handler_emsg("BasicAuth user/pw did not match\n");
            sprintf(response, "HTTP/1.1 401 Forbidden\r\n"
                              "\r\n");
            ws_send(ws_ctx, response, strlen(response));
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
                if (ownerapi(ws_ctx, handshake))
                    goto done;
            } else {
                sprintf(response, "HTTP/1.1 401 Unauthorized\r\n"
                        "Server: KasmVNC/4.0\r\n"
                        "Connection: close\r\n"
                        "Content-type: text/plain\r\n"
                        "\r\n"
                        "401 Unauthorized");
                ws_send(ws_ctx, response, strlen(response));
                goto done;
            }
        }

        if (settings.httpdir && settings.httpdir[0])
            servefile(ws_ctx, handshake);

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

    const struct wspass_t * const pass = ptr;

    const int csock = pass->csock;
    wsthread_handler_id = pass->id;

    ws_ctx_t *ws_ctx;

    ws_ctx = do_handshake(csock);
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
    int csock, pid;
    struct sockaddr_in cli_addr;
    socklen_t clilen;

//    printf("Waiting for connections on %s:%d\n",
//            settings.listen_host, settings.listen_port);

    while (1) {
        clilen = sizeof(cli_addr);
        pipe_error = 0;
        pid = 0;
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
        fprintf(stderr, " websocket %d: got client connection from %s\n",
                    settings.handler_id,
                    pass->ip);

        pthread_t tid;
        pass->id = settings.handler_id;
        pass->csock = csock;
        pthread_create(&tid, NULL, subthread, pass);

        settings.handler_id += 1;
    }
    handler_msg("websockify exit\n");

    return NULL;
}
