#pragma once

#include <stddef.h>
#include <stdint.h>

const uint32_t kSctpDefaultBufferSpace = 1 << 18;
const uint32_t kSctpMinInitAckLength = 32;

enum SctpFlag {
  SctpFlagEndFragment = 0x01,
  SctpFlagBeginFragment = 0x02,
  SctpFlagUnreliable = 0x04
};

const uint8_t kSctpFlagCompleteUnreliable =
    SctpFlagEndFragment | SctpFlagBeginFragment | SctpFlagUnreliable;

enum SctpChunkType {
  Sctp_Data = 0x00,
  Sctp_Init = 0x01,
  Sctp_InitAck = 0x02,
  Sctp_Sack = 0x03,
  Sctp_Heartbeat = 0x04,
  Sctp_HeartbeatAck = 0x05,
  Sctp_Abort = 0x06,
  Sctp_Shutdown = 0x07,
  Sctp_CookieEcho = 0x0A,
  Sctp_CookieAck = 0x0B,
  SctpChunk_ForwardTsn = 0xC0
};

enum SctpParamType {
  Sctp_StateCookie = 0x07,
  Sctp_ForwardTsn = 0xC000,
  Sctp_Random = 0x8002,
  Sctp_AuthChunkList = 0x8003,
  Sctp_HMACAlgo = 0x8004,
  Sctp_SupportedExts = 0x8008
};

struct SctpChunk {
  uint8_t type;
  uint8_t flags;
  uint16_t length;

  union {
    struct {
      uint32_t tsn;
      uint16_t streamId;
      uint16_t streamSeq;
      uint32_t protoId;
      int32_t userDataLength;
      const uint8_t* userData;
    } data;

    struct {
      uint32_t initiateTag;
      uint32_t windowCredit;
      uint16_t numOutboundStreams;
      uint16_t numInboundStreams;
      uint32_t initialTsn;
    } init;

    struct {
      int32_t heartbeatInfoLen;
      const uint8_t* heartbeatInfo;
    } heartbeat;

    struct {
      uint32_t cumulativeTsnAck;
      uint32_t advRecvWindow;
      uint16_t numGapAckBlocks;
      uint16_t numDupTsn;
    } sack;

    struct {
      uint32_t cumulativeTsnAck;
    } shutdown;

    struct {
      uint32_t newCumulativeTsn;
    } forwardTsn;
  } as;
};

struct SctpPacket {
  uint16_t sourcePort;
  uint16_t destionationPort;
  uint32_t verificationTag;
  uint32_t checkSum;
};

int32_t ParseSctpPacket(const uint8_t* buf, size_t len, SctpPacket* packet,
                        SctpChunk* chunks, size_t maxChunks, size_t* nChunk);

size_t SerializeSctpPacket(const SctpPacket* packet, const SctpChunk* chunks,
                           size_t numChunks, uint8_t* dst, size_t dstLen);

int32_t SctpDataChunkLength(int32_t userDataLength);
int32_t SctpChunkLength(int32_t contentLength);
