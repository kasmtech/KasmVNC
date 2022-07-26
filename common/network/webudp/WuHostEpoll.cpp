#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "WuHost.h"
#include "WuHttp.h"
#include "WuMath.h"
#include "WuNetwork.h"
#include "WuPool.h"
#include "WuRng.h"
#include "WuString.h"

static pthread_mutex_t wumutex = PTHREAD_MUTEX_INITIALIZER;

struct WuConnectionBuffer {
  size_t size = 0;
  int fd = -1;
  uint8_t requestBuffer[kMaxHttpRequestLength];
};

struct WuHost {
  Wu* wu;
  int udpfd;
  int epfd;
  int pollTimeout;
  WuPool* bufferPool;
  struct epoll_event* events;
  int32_t maxEvents;
  uint16_t port;
  char errBuf[512];
};

static void HostReclaimBuffer(WuHost* host, WuConnectionBuffer* buffer) {
  buffer->fd = -1;
  buffer->size = 0;
  WuPoolRelease(host->bufferPool, buffer);
}

static WuConnectionBuffer* HostGetBuffer(WuHost* host) {
  WuConnectionBuffer* buffer = (WuConnectionBuffer*)WuPoolAcquire(host->bufferPool);
  return buffer;
}

static void HandleErrno(WuHost* host, const char* description) {
  snprintf(host->errBuf, sizeof(host->errBuf), "%s: %s", description,
           strerror(errno));
  WuReportError(host->wu, host->errBuf);
}

static void WriteUDPData(const uint8_t* data, size_t length,
                         const WuClient* client, void* userData) {
  WuHost* host = (WuHost*)userData;

  WuAddress address = WuClientGetAddress(client);
  struct sockaddr_in netaddr;
  netaddr.sin_family = AF_INET;
  netaddr.sin_port = htons(address.port);
  netaddr.sin_addr.s_addr = htonl(address.host);

  sendto(host->udpfd, data, length, 0, (struct sockaddr*)&netaddr,
         sizeof(netaddr));
}

int32_t WuHostServe(WuHost* host, WuEvent* evt, int timeout) {
  if (pthread_mutex_lock(&wumutex))
    abort();
  int32_t hres = WuUpdate(host->wu, evt);
  pthread_mutex_unlock(&wumutex);

  if (hres) {
    return hres;
  }

  int n =
      epoll_wait(host->epfd, host->events, host->maxEvents, timeout);

  for (int i = 0; i < n; i++) {
    struct epoll_event* e = &host->events[i];
    WuConnectionBuffer* c = (WuConnectionBuffer*)e->data.ptr;

    if ((e->events & EPOLLERR) || (e->events & EPOLLHUP) ||
        (!(e->events & EPOLLIN))) {
      close(c->fd);
      HostReclaimBuffer(host, c);
      continue;
    }

    if (host->udpfd == c->fd) {
      struct sockaddr_in remote;
      socklen_t remoteLen = sizeof(remote);
      uint8_t buf[4096];

      ssize_t r = 0;
      while ((r = recvfrom(host->udpfd, buf, sizeof(buf), 0,
                           (struct sockaddr*)&remote, &remoteLen)) > 0) {
        WuAddress address;
        address.host = ntohl(remote.sin_addr.s_addr);
        address.port = ntohs(remote.sin_port);

        if (pthread_mutex_lock(&wumutex))
          abort();
        WuHandleUDP(host->wu, &address, buf, r);
        pthread_mutex_unlock(&wumutex);
      }

    } else {
      WuReportError(host->wu, "Unknown epoll source");
    }
  }

  return 0;
}

int32_t WuHostCreate(const char* hostAddr, uint16_t port, int32_t maxClients, WuHost** host) {
  *host = NULL;

  WuHost* ctx = (WuHost*)calloc(1, sizeof(WuHost));


  if (!ctx) {
    return WU_OUT_OF_MEMORY;
  }

  int32_t status = WuCreate(hostAddr, port, maxClients, &ctx->wu);

  if (status != WU_OK) {
    free(ctx);
    return status;
  }

  ctx->udpfd = CreateSocket(port);

  if (ctx->udpfd == -1) {
    WuHostDestroy(ctx);
    return WU_ERROR;
  }

  status = MakeNonBlocking(ctx->udpfd);
  if (status == -1) {
    WuHostDestroy(ctx);
    return WU_ERROR;
  }

  ctx->epfd = epoll_create(1024);
  if (ctx->epfd == -1) {
    WuHostDestroy(ctx);
    return WU_ERROR;
  }

  const int32_t maxEvents = 128;
  ctx->bufferPool = WuPoolCreate(sizeof(WuConnectionBuffer), maxEvents + 2);

  if (!ctx->bufferPool) {
    WuHostDestroy(ctx);
    return WU_OUT_OF_MEMORY;
  }

  WuConnectionBuffer* udpBuf = HostGetBuffer(ctx);
  udpBuf->fd = ctx->udpfd;

  struct epoll_event event;
  event.events = EPOLLIN | EPOLLET;
  event.data.ptr = udpBuf;
  status = epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->udpfd, &event);
  if (status == -1) {
    WuHostDestroy(ctx);
    return WU_ERROR;
  }

  ctx->maxEvents = maxEvents;
  ctx->events = (struct epoll_event*)calloc(ctx->maxEvents, sizeof(event));

  if (!ctx->events) {
    WuHostDestroy(ctx);
    return WU_OUT_OF_MEMORY;
  }

  WuSetUserData(ctx->wu, ctx);
  WuSetUDPWriteFunction(ctx->wu, WriteUDPData);

  *host = ctx;

  return WU_OK;
}

void WuHostRemoveClient(WuHost* host, WuClient* client) {
  WuRemoveClient(host->wu, client);
}

int32_t WuHostSendText(WuHost* host, WuClient* client, const char* text,
                       int32_t length) {
  return WuSendText(host->wu, client, text, length);
}

int32_t WuHostSendBinary(WuHost* host, WuClient* client, const uint8_t* data,
                         int32_t length) {
  if (pthread_mutex_lock(&wumutex))
    abort();
  int32_t ret = WuSendBinary(host->wu, client, data, length);
  pthread_mutex_unlock(&wumutex);

  return ret;
}

void WuHostSetErrorCallback(WuHost* host, WuErrorFn callback) {
  WuSetErrorCallback(host->wu, callback);
}

void WuHostSetDebugCallback(WuHost* host, WuErrorFn callback) {
  WuSetDebugCallback(host->wu, callback);
}

void WuHostDestroy(WuHost* host) {
  if (!host) {
    return;
  }

  WuDestroy(host->wu);

  if (host->udpfd != -1) {
    close(host->udpfd);
  }

  if (host->epfd != -1) {
    close(host->epfd);
  }

  if (host->bufferPool) {
    free(host->bufferPool);
  }

  if (host->events) {
    free(host->events);
  }
}

WuClient* WuHostFindClient(const WuHost* host, WuAddress address) {
  return WuFindClient(host->wu, address);
}

void WuGotHttp(WuHost *host, const char msg[], const uint32_t msglen,
               char resp[]) {

  const SDPResult sdp = WuExchangeSDP(
      host->wu, msg, msglen);

  if (sdp.status == WuSDPStatus_Success) {
    snprintf(resp, 4096,
             "%.*s",
             sdp.sdpLength, sdp.sdp);
  } else if (sdp.status == WuSDPStatus_MaxClients) {
    WuReportError(host->wu, "Too many connections");
    strcpy(resp, HTTP_UNAVAILABLE);
  } else if (sdp.status == WuSDPStatus_InvalidSDP) {
    WuReportError(host->wu, "Invalid SDP");
    strcpy(resp, HTTP_BAD_REQUEST);
  } else {
    WuReportError(host->wu, "Other error");
    strcpy(resp, HTTP_SERVER_ERROR);
  }
}
