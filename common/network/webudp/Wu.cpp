#include "Wu.h"
#include <assert.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string.h>
#include "WuArena.h"
#include "WuClock.h"
#include "WuCrypto.h"
#include "WuMath.h"
#include "WuPool.h"
#include "WuQueue.h"
#include "WuRng.h"
#include "WuSctp.h"
#include "WuSdp.h"
#include "WuStun.h"

struct Wu {
  WuArena* arena;
  double time;
  double dt;
  char host[256];
  uint16_t port;
  WuQueue* pendingEvents;
  int32_t maxClients;
  int32_t numClients;

  WuPool* clientPool;
  WuClient** clients;
  ssl_ctx_st* sslCtx;

  char certFingerprint[96];

  char errBuf[512];
  void* userData;
  WuErrorFn errorCallback;
  WuErrorFn debugCallback;
  WuWriteFn writeUdpData;
};

const double kMaxClientTtl = 9.0;
const double heartbeatInterval = 4.0;
const int kDefaultMTU = 1400;

static void DefaultErrorCallback(const char*, void*) {}
static void WriteNothing(const uint8_t*, size_t, const WuClient*, void*) {}

enum DataChannelMessageType { DCMessage_Ack = 0x02, DCMessage_Open = 0x03 };

enum DataChanProtoIdentifier {
  DCProto_Control = 50,
  DCProto_String = 51,
  DCProto_Binary = 53,
  DCProto_EmptyString = 56,
  DCProto_EmptyBinary = 57
};

struct DataChannelPacket {
  uint8_t messageType;

  union {
    struct {
      uint8_t channelType;
      uint16_t priority;
      uint32_t reliability;
    } open;
  } as;
};

enum WuClientState {
  WuClient_Dead,
  WuClient_WaitingRemoval,
  WuClient_DTLSHandshake,
  WuClient_SCTPEstablished,
  WuClient_DataChannelOpen
};

static int32_t ParseDataChannelControlPacket(const uint8_t* buf, size_t len,
                                             DataChannelPacket* packet) {
  ReadScalarSwapped(buf, &packet->messageType);
  return 0;
}

void WuReportError(Wu* wu, const char* description) {
  wu->errorCallback(description, wu->userData);
}

void WuReportDebug(Wu* wu, const char* description) {
  wu->debugCallback(description, wu->userData);
}

struct WuClient {
  StunUserIdentifier serverUser;
  StunUserIdentifier serverPassword;
  StunUserIdentifier remoteUser;
  StunUserIdentifier remoteUserPassword;
  WuAddress address;
  WuClientState state;
  uint16_t localSctpPort;
  uint16_t remoteSctpPort;
  uint32_t sctpVerificationTag;
  uint32_t remoteTsn;
  uint32_t tsn;
  double ttl;
  double nextHeartbeat;

  SSL* ssl;
  BIO* inBio;
  BIO* outBio;

  void* user;
};

void WuClientSetUserData(WuClient* client, void* user) { client->user = user; }

void* WuClientGetUserData(const WuClient* client) { return client->user; }

static void WuClientFinish(WuClient* client) {
  SSL_free(client->ssl);
  client->ssl = NULL;
  client->inBio = NULL;
  client->outBio = NULL;
  client->state = WuClient_Dead;
}

static void WuClientStart(const Wu* wu, WuClient* client) {
  client->state = WuClient_DTLSHandshake;
  client->remoteSctpPort = 0;
  client->sctpVerificationTag = 0;
  client->remoteTsn = 0;
  client->tsn = 1;
  client->ttl = kMaxClientTtl;
  client->nextHeartbeat = heartbeatInterval;
  client->user = NULL;

  client->ssl = SSL_new(wu->sslCtx);

  client->inBio = BIO_new(BIO_s_mem());
  BIO_set_mem_eof_return(client->inBio, -1);
  client->outBio = BIO_new(BIO_s_mem());
  BIO_set_mem_eof_return(client->outBio, -1);
  SSL_set_bio(client->ssl, client->inBio, client->outBio);
  SSL_set_options(client->ssl, SSL_OP_SINGLE_ECDH_USE);
  SSL_set_options(client->ssl, SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
  SSL_set_tmp_ecdh(client->ssl, EC_KEY_new_by_curve_name(NID_X9_62_prime256v1));
  SSL_set_accept_state(client->ssl);
  SSL_set_mtu(client->ssl, kDefaultMTU);
}

static void WuSendSctp(const Wu* wu, WuClient* client, const SctpPacket* packet,
                       const SctpChunk* chunks, int32_t numChunks);

static WuClient* WuNewClient(Wu* wu) {
  WuClient* client = (WuClient*)WuPoolAcquire(wu->clientPool);

  if (client) {
    memset(client, 0, sizeof(WuClient));
    WuClientStart(wu, client);
    wu->clients[wu->numClients++] = client;
    return client;
  }

  return NULL;
}

static void WuPushEvent(Wu* wu, WuEvent evt) {
  WuQueuePush(wu->pendingEvents, &evt);
}

static void WuSendSctpShutdown(Wu* wu, WuClient* client) {
  SctpPacket response;
  response.sourcePort = client->localSctpPort;
  response.destionationPort = client->remoteSctpPort;
  response.verificationTag = client->sctpVerificationTag;

  SctpChunk rc;
  rc.type = Sctp_Shutdown;
  rc.flags = 0;
  rc.length = SctpChunkLength(sizeof(rc.as.shutdown.cumulativeTsnAck));
  rc.as.shutdown.cumulativeTsnAck = client->remoteTsn;

  WuSendSctp(wu, client, &response, &rc, 1);
}

void WuRemoveClient(Wu* wu, WuClient* client) {
  for (int32_t i = 0; i < wu->numClients; i++) {
    if (wu->clients[i] == client) {
      WuSendSctpShutdown(wu, client);
      WuClientFinish(client);
      WuPoolRelease(wu->clientPool, client);
      wu->clients[i] = wu->clients[wu->numClients - 1];
      wu->numClients--;
      return;
    }
  }
}

static WuClient* WuFindClient(Wu* wu, const WuAddress* address) {
  for (int32_t i = 0; i < wu->numClients; i++) {
    WuClient* client = wu->clients[i];
    if (client->address.host == address->host &&
        client->address.port == address->port) {
      return client;
    }
  }

  return NULL;
}

static WuClient* WuFindClientByCreds(Wu* wu, const StunUserIdentifier* svUser,
                                     const StunUserIdentifier* clUser) {
  for (int32_t i = 0; i < wu->numClients; i++) {
    WuClient* client = wu->clients[i];
    if (StunUserIdentifierEqual(&client->serverUser, svUser) &&
        StunUserIdentifierEqual(&client->remoteUser, clUser)) {
      return client;
    }
  }

  return NULL;
}

static void WuClientSendPendingDTLS(const Wu* wu, WuClient* client) {
  uint8_t sendBuffer[4096];

  while (BIO_ctrl_pending(client->outBio) > 0) {
    int bytes = BIO_read(client->outBio, sendBuffer, sizeof(sendBuffer));
    if (bytes > 0) {
      wu->writeUdpData(sendBuffer, bytes, client, wu->userData);
    }
  }
}

static void TLSSend(const Wu* wu, WuClient* client, const void* data,
                    int32_t length) {
  if (client->state < WuClient_DTLSHandshake ||
      !SSL_is_init_finished(client->ssl)) {
    return;
  }

  SSL_write(client->ssl, data, length);
  WuClientSendPendingDTLS(wu, client);
}

static void WuSendSctp(const Wu* wu, WuClient* client, const SctpPacket* packet,
                       const SctpChunk* chunks, int32_t numChunks) {
  uint8_t outBuffer[4096];
  memset(outBuffer, 0, sizeof(outBuffer));
  size_t bytesWritten = SerializeSctpPacket(packet, chunks, numChunks,
                                            outBuffer, sizeof(outBuffer));
  TLSSend(wu, client, outBuffer, bytesWritten);
}

static void WuHandleSctp(Wu* wu, WuClient* client, const uint8_t* buf,
                         int32_t len) {
  const size_t maxChunks = 8;
  SctpChunk chunks[maxChunks];
  SctpPacket sctpPacket;
  size_t nChunk = 0;

  if (!ParseSctpPacket(buf, len, &sctpPacket, chunks, maxChunks, &nChunk)) {
    return;
  }

  for (size_t n = 0; n < nChunk; n++) {
    SctpChunk* chunk = &chunks[n];
    if (chunk->type == Sctp_Data) {
      auto* dataChunk = &chunk->as.data;
      const uint8_t* userDataBegin = dataChunk->userData;
      const int32_t userDataLength = dataChunk->userDataLength;

      client->remoteTsn = Max(chunk->as.data.tsn, client->remoteTsn);
      client->ttl = kMaxClientTtl;

      if (dataChunk->protoId == DCProto_Control) {
        DataChannelPacket packet;
        ParseDataChannelControlPacket(userDataBegin, userDataLength, &packet);
        if (packet.messageType == DCMessage_Open) {
          client->remoteSctpPort = sctpPacket.sourcePort;
          uint8_t outType = DCMessage_Ack;
          SctpPacket response;
          response.sourcePort = sctpPacket.destionationPort;
          response.destionationPort = sctpPacket.sourcePort;
          response.verificationTag = client->sctpVerificationTag;

          SctpChunk rc;
          rc.type = Sctp_Data;
          rc.flags = kSctpFlagCompleteUnreliable;
          rc.length = SctpDataChunkLength(1);

          auto* dc = &rc.as.data;
          dc->tsn = client->tsn++;
          dc->streamId = chunk->as.data.streamId;
          dc->streamSeq = 0;
          dc->protoId = DCProto_Control;
          dc->userData = &outType;
          dc->userDataLength = 1;

          if (client->state != WuClient_DataChannelOpen) {
            client->state = WuClient_DataChannelOpen;
            WuEvent event;
            event.type = WuEvent_ClientJoin;
            event.client = client;
            WuPushEvent(wu, event);
          }

          WuSendSctp(wu, client, &response, &rc, 1);
        }
      } else if (dataChunk->protoId == DCProto_String) {
        WuEvent evt;
        evt.type = WuEvent_TextData;
        evt.client = client;
        evt.data = dataChunk->userData;
        evt.length = dataChunk->userDataLength;
        WuPushEvent(wu, evt);
      } else if (dataChunk->protoId == DCProto_Binary) {
        WuEvent evt;
        evt.type = WuEvent_BinaryData;
        evt.client = client;
        evt.data = dataChunk->userData;
        evt.length = dataChunk->userDataLength;
        WuPushEvent(wu, evt);
      }

      SctpPacket sack;
      sack.sourcePort = sctpPacket.destionationPort;
      sack.destionationPort = sctpPacket.sourcePort;
      sack.verificationTag = client->sctpVerificationTag;

      SctpChunk rc;
      rc.type = Sctp_Sack;
      rc.flags = 0;
      rc.length = SctpChunkLength(12);
      rc.as.sack.cumulativeTsnAck = client->remoteTsn;
      rc.as.sack.advRecvWindow = kSctpDefaultBufferSpace;
      rc.as.sack.numGapAckBlocks = 0;
      rc.as.sack.numDupTsn = 0;

      WuSendSctp(wu, client, &sack, &rc, 1);
    } else if (chunk->type == Sctp_Init) {
      SctpPacket response;
      response.sourcePort = sctpPacket.destionationPort;
      response.destionationPort = sctpPacket.sourcePort;
      response.verificationTag = chunk->as.init.initiateTag;
      client->sctpVerificationTag = response.verificationTag;
      client->remoteTsn = chunk->as.init.initialTsn - 1;

      SctpChunk rc;
      rc.type = Sctp_InitAck;
      rc.flags = 0;
      rc.length = kSctpMinInitAckLength;

      rc.as.init.initiateTag = WuRandomU32();
      rc.as.init.windowCredit = kSctpDefaultBufferSpace;
      rc.as.init.numOutboundStreams = chunk->as.init.numInboundStreams;
      rc.as.init.numInboundStreams = chunk->as.init.numOutboundStreams;
      rc.as.init.initialTsn = client->tsn;

      WuSendSctp(wu, client, &response, &rc, 1);
      break;
    } else if (chunk->type == Sctp_CookieEcho) {
      if (client->state < WuClient_SCTPEstablished) {
        client->state = WuClient_SCTPEstablished;
      }
      SctpPacket response;
      response.sourcePort = sctpPacket.destionationPort;
      response.destionationPort = sctpPacket.sourcePort;
      response.verificationTag = client->sctpVerificationTag;

      SctpChunk rc;
      rc.type = Sctp_CookieAck;
      rc.flags = 0;
      rc.length = SctpChunkLength(0);

      WuSendSctp(wu, client, &response, &rc, 1);
    } else if (chunk->type == Sctp_Heartbeat) {
      SctpPacket response;
      response.sourcePort = sctpPacket.destionationPort;
      response.destionationPort = sctpPacket.sourcePort;
      response.verificationTag = client->sctpVerificationTag;

      SctpChunk rc;
      rc.type = Sctp_HeartbeatAck;
      rc.flags = 0;
      rc.length = chunk->length;
      rc.as.heartbeat.heartbeatInfoLen = chunk->as.heartbeat.heartbeatInfoLen;
      rc.as.heartbeat.heartbeatInfo = chunk->as.heartbeat.heartbeatInfo;

      client->ttl = kMaxClientTtl;

      WuSendSctp(wu, client, &response, &rc, 1);
    } else if (chunk->type == Sctp_HeartbeatAck) {
      client->ttl = kMaxClientTtl;
    } else if (chunk->type == Sctp_Abort) {
      client->state = WuClient_WaitingRemoval;
      return;
    } else if (chunk->type == Sctp_Sack) {
      client->ttl = kMaxClientTtl;

      auto* sack = &chunk->as.sack;
      if (sack->numGapAckBlocks > 0) {
        SctpPacket fwdResponse;
        fwdResponse.sourcePort = sctpPacket.destionationPort;
        fwdResponse.destionationPort = sctpPacket.sourcePort;
        fwdResponse.verificationTag = client->sctpVerificationTag;

        SctpChunk fwdTsnChunk;
        fwdTsnChunk.type = SctpChunk_ForwardTsn;
        fwdTsnChunk.flags = 0;
        fwdTsnChunk.length = SctpChunkLength(4);
        fwdTsnChunk.as.forwardTsn.newCumulativeTsn = client->tsn;
        WuSendSctp(wu, client, &fwdResponse, &fwdTsnChunk, 1);
      }
    }
  }
}

static void WuReceiveDTLSPacket(Wu* wu, const uint8_t* data, size_t length,
                                const WuAddress* address) {
  WuClient* client = WuFindClient(wu, address);
  if (!client) {
    WuReportDebug(wu, "DTLS: No client found");
    return;
  }

  BIO_write(client->inBio, data, length);

  if (!SSL_is_init_finished(client->ssl)) {
    int r = SSL_do_handshake(client->ssl);

    if (r <= 0) {
      r = SSL_get_error(client->ssl, r);
      if (SSL_ERROR_WANT_READ == r) {
        WuClientSendPendingDTLS(wu, client);
      } else if (SSL_ERROR_NONE != r) {
        char* error = ERR_error_string(r, NULL);
        if (error) {
          WuReportError(wu, error);
        }
      }
    }
  } else {
    WuClientSendPendingDTLS(wu, client);

    while (BIO_ctrl_pending(client->inBio) > 0) {
      uint8_t receiveBuffer[8092];
      int bytes = SSL_read(client->ssl, receiveBuffer, sizeof(receiveBuffer));

      if (bytes > 0) {
        uint8_t* buf = (uint8_t*)WuArenaAcquire(wu->arena, bytes);
        memcpy(buf, receiveBuffer, bytes);
        WuHandleSctp(wu, client, buf, bytes);
      }
    }
  }
}

static void WuHandleStun(Wu* wu, const StunPacket* packet,
                         const WuAddress* remote) {
  WuClient* client =
      WuFindClientByCreds(wu, &packet->serverUser, &packet->remoteUser);

  if (!client) {
    WuReportDebug(wu, "Stun: No client found");
    // TODO: Send unauthorized
    return;
  }

  StunPacket outPacket;
  outPacket.type = Stun_SuccessResponse;
  memcpy(outPacket.transactionId, packet->transactionId,
         kStunTransactionIdLength);
  outPacket.xorMappedAddress.family = Stun_IPV4;
  outPacket.xorMappedAddress.port = ByteSwap(remote->port ^ kStunXorMagic);
  outPacket.xorMappedAddress.address.ipv4 =
      ByteSwap(remote->host ^ kStunCookie);

  uint8_t stunResponse[512];
  size_t serializedSize =
      SerializeStunPacket(&outPacket, client->serverPassword.identifier,
                          client->serverPassword.length, stunResponse, 512);

  client->localSctpPort = remote->port;
  client->address = *remote;

  wu->writeUdpData(stunResponse, serializedSize, client, wu->userData);
}

static void WuPurgeDeadClients(Wu* wu) {
  for (int32_t i = 0; i < wu->numClients; i++) {
    WuClient* client = wu->clients[i];
    if (client->ttl <= 0.0 || client->state == WuClient_WaitingRemoval) {

      if (client->ttl <= 0.0)
        WuReportDebug(wu, "Removing dead client due to no messages in 9s");
      else
        WuReportDebug(wu, "Removing client due to its own request");

      WuEvent evt;
      evt.type = WuEvent_ClientLeave;
      evt.client = client;
      WuPushEvent(wu, evt);
    }
  }
}

static int32_t WuCryptoInit(Wu* wu) {

  wu->sslCtx = SSL_CTX_new(DTLS_server_method());
  if (!wu->sslCtx) {
    ERR_print_errors_fp(stderr);
    return 0;
  }

  int sslStatus =
      SSL_CTX_set_cipher_list(wu->sslCtx, "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
  if (sslStatus != 1) {
    ERR_print_errors_fp(stderr);
    return 0;
  }

  SSL_CTX_set_verify(wu->sslCtx, SSL_VERIFY_NONE, NULL);

  WuCert cert;

  sslStatus = SSL_CTX_use_PrivateKey(wu->sslCtx, cert.key);

  if (sslStatus != 1) {
    ERR_print_errors_fp(stderr);
    return 0;
  }

  sslStatus = SSL_CTX_use_certificate(wu->sslCtx, cert.x509);

  if (sslStatus != 1) {
    ERR_print_errors_fp(stderr);
    return 0;
  }

  sslStatus = SSL_CTX_check_private_key(wu->sslCtx);

  if (sslStatus != 1) {
    ERR_print_errors_fp(stderr);
    return 0;
  }

  SSL_CTX_set_options(wu->sslCtx, SSL_OP_NO_QUERY_MTU);

  memcpy(wu->certFingerprint, cert.fingerprint, sizeof(cert.fingerprint));

  return 1;
}

int32_t WuCreate(const char* host, uint16_t port, int maxClients, Wu** wu) {
  *wu = NULL;

  Wu* ctx = (Wu*)calloc(1, sizeof(Wu));

  if (!ctx) {
    return WU_OUT_OF_MEMORY;
  }

  ctx->arena = (WuArena*)calloc(1, sizeof(WuArena));

  if (!ctx->arena) {
    WuDestroy(ctx);
    return WU_OUT_OF_MEMORY;
  }

  WuArenaInit(ctx->arena, 1 << 20);

  ctx->time = MsNow() * 0.001;
  ctx->port = port;
  ctx->pendingEvents = WuQueueCreate(sizeof(WuEvent), 1024);
  ctx->errorCallback = DefaultErrorCallback;
  ctx->debugCallback = DefaultErrorCallback;
  ctx->writeUdpData = WriteNothing;

  strncpy(ctx->host, host, sizeof(ctx->host));

  if (!WuCryptoInit(ctx)) {
    WuDestroy(ctx);
    return WU_ERROR;
  }

  ctx->maxClients = maxClients <= 0 ? 256 : maxClients;
  ctx->clientPool = WuPoolCreate(sizeof(WuClient), ctx->maxClients);
  ctx->clients = (WuClient**)calloc(ctx->maxClients, sizeof(WuClient*));

  *wu = ctx;
  return WU_OK;
}

static void WuSendHeartbeat(Wu* wu, WuClient* client) {
  SctpPacket packet;
  packet.sourcePort = wu->port;
  packet.destionationPort = client->remoteSctpPort;
  packet.verificationTag = client->sctpVerificationTag;

  SctpChunk rc;
  rc.type = Sctp_Heartbeat;
  rc.flags = kSctpFlagCompleteUnreliable;
  rc.length = SctpChunkLength(4 + 8);
  rc.as.heartbeat.heartbeatInfo = (const uint8_t*)&wu->time;
  rc.as.heartbeat.heartbeatInfoLen = sizeof(wu->time);

  WuSendSctp(wu, client, &packet, &rc, 1);
}

static void WuUpdateClients(Wu* wu) {
  double t = MsNow() * 0.001;
  wu->dt = t - wu->time;
  wu->time = t;

  for (int32_t i = 0; i < wu->numClients; i++) {
    WuClient* client = wu->clients[i];
    client->ttl -= wu->dt;
    client->nextHeartbeat -= wu->dt;

    if (client->nextHeartbeat <= 0.0) {
      client->nextHeartbeat = heartbeatInterval;
      WuSendHeartbeat(wu, client);
    }

    WuClientSendPendingDTLS(wu, client);
  }
}

int32_t WuUpdate(Wu* wu, WuEvent* evt) {
  if (WuQueuePop(wu->pendingEvents, evt)) {
    return 1;
  }

  WuUpdateClients(wu);
  WuArenaReset(wu->arena);

  WuPurgeDeadClients(wu);

  return 0;
}

static int32_t WuSendData(Wu* wu, WuClient* client, const uint8_t* data,
                          int32_t length, DataChanProtoIdentifier proto) {
  if (client->state < WuClient_DataChannelOpen) {
    return -1;
  }

  SctpPacket packet;
  packet.sourcePort = wu->port;
  packet.destionationPort = client->remoteSctpPort;
  packet.verificationTag = client->sctpVerificationTag;

  SctpChunk rc;
  rc.type = Sctp_Data;
  rc.flags = kSctpFlagCompleteUnreliable;
  rc.length = SctpDataChunkLength(length);

  auto* dc = &rc.as.data;
  dc->tsn = client->tsn++;
  dc->streamId = 0;  // TODO: Does it matter?
  dc->streamSeq = 0;
  dc->protoId = proto;
  dc->userData = data;
  dc->userDataLength = length;

  WuSendSctp(wu, client, &packet, &rc, 1);
  return 0;
}

int32_t WuSendText(Wu* wu, WuClient* client, const char* text, int32_t length) {
  return WuSendData(wu, client, (const uint8_t*)text, length, DCProto_String);
}

int32_t WuSendBinary(Wu* wu, WuClient* client, const uint8_t* data,
                     int32_t length) {
  return WuSendData(wu, client, data, length, DCProto_Binary);
}

SDPResult WuExchangeSDP(Wu* wu, const char* sdp, int32_t length) {
  ICESdpFields iceFields;
  if (!ParseSdp(sdp, length, &iceFields)) {
    return {WuSDPStatus_InvalidSDP, NULL, NULL, 0};
  }

  WuClient* client = WuNewClient(wu);

  if (!client) {
    return {WuSDPStatus_MaxClients, NULL, NULL, 0};
  }

  client->serverUser.length = 4;
  WuRandomString((char*)client->serverUser.identifier,
                 client->serverUser.length);
  client->serverPassword.length = 24;
  WuRandomString((char*)client->serverPassword.identifier,
                 client->serverPassword.length);
  memcpy(client->remoteUser.identifier, iceFields.ufrag.value,
         Min(iceFields.ufrag.length, kMaxStunIdentifierLength));
  client->remoteUser.length = iceFields.ufrag.length;
  memcpy(client->remoteUserPassword.identifier, iceFields.password.value,
         Min(iceFields.password.length, kMaxStunIdentifierLength));

  int sdpLength = 0;
  const char* responseSdp = GenerateSDP(
      wu->arena, wu->certFingerprint, wu->host, wu->port,
      (char*)client->serverUser.identifier, client->serverUser.length,
      (char*)client->serverPassword.identifier, client->serverPassword.length,
      &iceFields, &sdpLength);

  if (!responseSdp) {
    return {WuSDPStatus_Error, NULL, NULL, 0};
  }

  return {WuSDPStatus_Success, client, responseSdp, sdpLength};
}

void WuSetUserData(Wu* wu, void* userData) { wu->userData = userData; }

void WuHandleUDP(Wu* wu, const WuAddress* remote, const uint8_t* data,
                 int32_t length) {
  StunPacket stunPacket;
  if (ParseStun(data, length, &stunPacket)) {
    //WuReportDebug(wu, "Received stun packet");
    WuHandleStun(wu, &stunPacket, remote);
  } else {
    //WuReportDebug(wu, "Received DTLS packet");
    WuReceiveDTLSPacket(wu, data, length, remote);
  }
}

void WuSetUDPWriteFunction(Wu* wu, WuWriteFn write) {
  wu->writeUdpData = write;
}

WuAddress WuClientGetAddress(const WuClient* client) { return client->address; }

void WuSetErrorCallback(Wu* wu, WuErrorFn callback) {
  if (callback) {
    wu->errorCallback = callback;
  } else {
    wu->errorCallback = DefaultErrorCallback;
  }
}

void WuSetDebugCallback(Wu* wu, WuErrorFn callback) {
  if (callback) {
    wu->debugCallback = callback;
  } else {
    wu->debugCallback = DefaultErrorCallback;
  }
}

void WuDestroy(Wu* wu) {
  if (!wu) {
    return;
  }

  free(wu);
}

WuClient* WuFindClient(const Wu* wu, WuAddress address) {
  for (int32_t i = 0; i < wu->numClients; i++) {
    WuClient* c = wu->clients[i];

    if (c->address.host == address.host && c->address.port == address.port) {
      return c;
    }
  }

  return NULL;
}
