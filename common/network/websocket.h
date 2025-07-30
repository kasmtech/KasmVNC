#include <openssl/ssl.h>
#include <stdint.h>
#include "GetAPIEnums.h"
#include "datelog.h"
#include "kasmpasswd.h"

#define BUFSIZE 65536
#define DBUFSIZE (BUFSIZE * 3) / 4 - 20

#define SERVER_HANDSHAKE_HIXIE "HTTP/1.1 101 Web Socket Protocol Handshake\r\n\
Upgrade: WebSocket\r\n\
Connection: Upgrade\r\n\
%sWebSocket-Origin: %s\r\n\
%sWebSocket-Location: %s://%s%s\r\n\
%sWebSocket-Protocol: %s\r\n\
\r\n%s"

#define SERVER_HANDSHAKE_HYBI "HTTP/1.1 101 Switching Protocols\r\n\
Upgrade: websocket\r\n\
Connection: Upgrade\r\n\
Sec-WebSocket-Accept: %s\r\n\
Sec-WebSocket-Protocol: %s\r\n\
\r\n"

#define HYBI_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define HYBI10_ACCEPTHDRLEN 29

#define HIXIE_MD5_DIGEST_LENGTH 16

#define POLICY_RESPONSE "<cross-domain-policy><allow-access-from domain=\"*\" to-ports=\"*\" /></cross-domain-policy>\n"

#define OPCODE_TEXT    0x01
#define OPCODE_BINARY  0x02

typedef struct {
    char path[1024+1];
    char host[1024+1];
    char origin[1024+1];
    char version[1024+1];
    char connection[1024+1];
    char protocols[1024+1];
    char key1[1024+1];
    char key2[1024+1];
    char key3[8+1];
} headers_t;

typedef struct {
    int        sockfd;
    SSL_CTX   *ssl_ctx;
    SSL       *ssl;
    int        hixie;
    int        hybi;
    int        opcode;
    headers_t *headers;
    char      *cin_buf;
    char      *cout_buf;
    char      *tin_buf;
    char      *tout_buf;

    char      user[USERNAME_LEN];
    char      ip[64];
} ws_ctx_t;

struct wspass_t {
    int csock;
    unsigned id;
    char ip[64];
};

struct kasmpasswd_entry_t;

typedef struct {
    int verbose;
    int listen_sock;
    unsigned int handler_id;
    const char *cert;
    const char *key;
    uint8_t disablebasicauth;
    const char *passwdfile;
    int ssl_only;
    const char *httpdir;

    void *messager;
    uint8_t *(*screenshotCb)(void *messager, uint16_t w, uint16_t h, const uint8_t q,
                             const uint8_t dedup,
                             uint32_t *len, uint8_t *staging);
    uint8_t (*adduserCb)(void *messager, const char name[], const char pw[],
                          const uint8_t read, const uint8_t write, const uint8_t owner);
    uint8_t (*removeCb)(void *messager, const char name[]);
    uint8_t (*updateUserCb)(void *messager, const char name[], const uint64_t mask,
                           const char password[],
                           const uint8_t read, const uint8_t write, const uint8_t owner);
    uint8_t (*addOrUpdateUserCb)(void *messager, const struct kasmpasswd_entry_t *entry);
    void (*bottleneckStatsCb)(void *messager, char *buf, uint32_t len);
    void (*frameStatsCb)(void *messager, char *buf, uint32_t len);
    void (*resetFrameStatsCb)(void *messager);

    uint8_t (*requestFrameStatsNoneCb)(void *messager);
    uint8_t (*requestFrameStatsOwnerCb)(void *messager);
    uint8_t (*requestFrameStatsAllCb)(void *messager);
    uint8_t (*requestFrameStatsOneCb)(void *messager, const char *client);

    uint8_t (*ownerConnectedCb)(void *messager);
    uint8_t (*numActiveUsersCb)(void *messager);
    void (*getUsersCb)(void *messager, const char **buf);
    uint8_t (*getClientFrameStatsNumCb)(void *messager);
    uint8_t (*serverFrameStatsReadyCb)(void *messager);

    void (*clearClipboardCb)(void *messager);

    void (*getSessionsCb)(void *messager, char **buf);
} settings_t;

#ifdef __cplusplus
extern "C" {
#endif

int resolve_host(struct in_addr *sin_addr, const char *hostname);

ssize_t ws_recv(ws_ctx_t *ctx, void *buf, size_t len);

ssize_t ws_send(ws_ctx_t *ctx, const void *buf, size_t len);

/* base64.c declarations */
//int b64_ntop(u_char const *src, size_t srclength, char *target, size_t targsize);
//int b64_pton(char const *src, u_char *target, size_t targsize);

void wslog(char *logbuf, const unsigned websocket, const uint8_t debug);

extern __thread unsigned wsthread_handler_id;

#define gen_handler_msg(stream, ...) \
    if (settings.verbose) { \
        char logbuf[2][1024]; \
        wslog(logbuf[0], wsthread_handler_id, 1); \
        sprintf(logbuf[1], __VA_ARGS__); \
        fprintf(stream, "%s%s", logbuf[0], logbuf[1]); \
    }

#define wserr(...) \
    { \
        char logbuf[2][1024]; \
        wslog(logbuf[0], wsthread_handler_id, 0); \
        sprintf(logbuf[1], __VA_ARGS__); \
        fprintf(stderr, "%s%s", logbuf[0], logbuf[1]); \
    }

#define handler_msg(...) gen_handler_msg(stderr, __VA_ARGS__);
#define handler_emsg(...) gen_handler_msg(stderr, __VA_ARGS__);

void traffic(const char * token);

int encode_hixie(u_char const *src, size_t srclength,
                 char *target, size_t targsize);
int decode_hixie(char *src, size_t srclength,
                 u_char *target, size_t targsize,
                 unsigned int *opcode, unsigned int *left);
int encode_hybi(u_char const *src, size_t srclength,
                char *target, size_t targsize, unsigned int opcode);
int decode_hybi(unsigned char *src, size_t srclength,
                u_char *target, size_t targsize,
                unsigned int *opcode, unsigned int *left);

void *start_server(void *unused);

#ifdef __cplusplus
} // extern C
#endif
