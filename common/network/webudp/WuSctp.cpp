#include "WuSctp.h"
#include <arpa/inet.h>
#include <cstring>
#include "CRC32.h"
#include "WuBufferOp.h"
#include "WuMath.h"
#include "WuNetwork.h"

static constexpr u_int8_t ValidPacketSize = 16;
static constexpr u_int8_t ChunkHeaderSize = 4;
static constexpr u_int8_t MaxChunkPayload = ValidPacketSize - ChunkHeaderSize;

size_t SctpPacketSize(const SctpChunk *chunk) {
    static constexpr size_t SctpDataPayloadSize = sizeof(SctpChunk::as.data.tsn)
                                                  + sizeof(SctpChunk::as.data.streamId)
                                                  + sizeof(SctpChunk::as.data.streamSeq)
                                                  + sizeof(SctpChunk::as.data.protoId);
    static constexpr size_t SctpInitAckPayloadSize = sizeof(SctpChunk::as.init.initiateTag)
                                                     + sizeof(SctpChunk::as.init.windowCredit)
                                                     + sizeof(SctpChunk::as.init.numOutboundStreams)
                                                     + sizeof(SctpChunk::as.init.numInboundStreams)
                                                     + sizeof(SctpChunk::as.init.initialTsn)
                                                     + 4 + 8;
    static constexpr size_t SctpSackPayloadSize = sizeof(SctpChunk::as.sack.cumulativeTsnAck)
                                                  + sizeof(SctpChunk::as.sack.advRecvWindow)
                                                  + sizeof(SctpChunk::as.sack.numGapAckBlocks)
                                                  + sizeof(SctpChunk::as.sack.numDupTsn);
    static constexpr size_t SctpShutdownPayloadSize = sizeof(SctpChunk::as.shutdown.cumulativeTsnAck);
    static constexpr size_t SctpForwardTsnPayloadSize = sizeof(SctpChunk::as.forwardTsn.newCumulativeTsn);

    size_t payloadSize = 0;

    switch (chunk->type) {
        case Sctp_Data: {
            auto *dc = &chunk->as.data;
            payloadSize = SctpDataPayloadSize + dc->userDataLength + PadSize(dc->userDataLength, 4);
            break;
        }
        case Sctp_InitAck:
            payloadSize = SctpInitAckPayloadSize;
            break;
        case Sctp_Sack:
            payloadSize = SctpSackPayloadSize;
            break;
        case Sctp_Heartbeat:
        case Sctp_HeartbeatAck: {
            auto *hb = &chunk->as.heartbeat;
            payloadSize = ChunkHeaderSize + hb->heartbeatInfoLen + PadSize(hb->heartbeatInfoLen, 4);
            break;
        }
        case Sctp_Shutdown:
            payloadSize = SctpShutdownPayloadSize;
            break;
        case SctpChunk_ForwardTsn:
            payloadSize = SctpForwardTsnPayloadSize;
            break;
        default:
            return 0;
    }

    return ChunkHeaderSize + payloadSize;
}

int32_t ParseSctpPacket(const uint8_t* buf, size_t len, SctpPacket* packet, SctpChunk* chunks, size_t maxChunks, size_t* nChunk) {
  if (len < ValidPacketSize)
    return 0;

  int32_t offset = ReadScalarSwapped(buf, &packet->sourcePort);
  offset += ReadScalarSwapped(buf + offset, &packet->destinationPort);
  offset += ReadScalarSwapped(buf + offset, &packet->verificationTag);
  offset += ReadScalarSwapped(buf + offset, &packet->checkSum);

  if (offset > len)
      return 0;

  uint32_t left = len - offset;

  size_t chunkNum = 0;
  while (left >= ChunkHeaderSize && chunkNum < maxChunks) {
    SctpChunk* chunk = &chunks[chunkNum++];

    offset += ReadScalarSwapped(buf + offset, &chunk->type);
    offset += ReadScalarSwapped(buf + offset, &chunk->flags);
    offset += ReadScalarSwapped(buf + offset, &chunk->length);

    *nChunk += 1;

    const auto *src = buf + offset;
    const auto payloadSize = chunk->length - ChunkHeaderSize;

    if (payloadSize < 0 || chunk->length > left)
        return 0;

    switch (chunk->type) {
        case Sctp_Data:
        {
            if (payloadSize < MaxChunkPayload)
                return 0;

            auto* p = &chunk->as.data;
            size_t chunkOffset = ReadScalarSwapped(src, &p->tsn);
            chunkOffset += ReadScalarSwapped(src + chunkOffset, &p->streamId);
            chunkOffset += ReadScalarSwapped(src + chunkOffset, &p->streamSeq);
            chunkOffset += ReadScalarSwapped(src + chunkOffset, &p->protoId);

            p->userDataLength = payloadSize - MaxChunkPayload;
            p->userData = src + chunkOffset;

            break;
        }
        case Sctp_Sack:
        {
            if (payloadSize < MaxChunkPayload)
                return 0;

            auto* sack = &chunk->as.sack;
            size_t chunkOffset = ReadScalarSwapped(src, &sack->cumulativeTsnAck);
            chunkOffset += ReadScalarSwapped(src + chunkOffset, &sack->advRecvWindow);
            chunkOffset += ReadScalarSwapped(src + chunkOffset, &sack->numGapAckBlocks);
            ReadScalarSwapped(src + chunkOffset, &sack->numDupTsn);

            break;
        }

        case Sctp_Heartbeat:
        {
            if (payloadSize < ChunkHeaderSize)
                return 0;

            auto* p = &chunk->as.heartbeat;
            size_t chunkOffset = 2;  // skip type
            uint16_t heartbeatLen;
            chunkOffset += ReadScalarSwapped(src + chunkOffset, &heartbeatLen);

            if (heartbeatLen > payloadSize || heartbeatLen < ChunkHeaderSize)
                return 0;

            p->heartbeatInfoLen = heartbeatLen - ChunkHeaderSize;
            p->heartbeatInfo = src + chunkOffset;

            break;
        }
        case Sctp_Init:
        {
            if (payloadSize < ValidPacketSize)
                return 0;

            size_t chunkOffset = ReadScalarSwapped(src, &chunk->as.init.initiateTag);
            chunkOffset += ReadScalarSwapped(src + chunkOffset, &chunk->as.init.windowCredit);
            chunkOffset += ReadScalarSwapped(src + chunkOffset, &chunk->as.init.numOutboundStreams);
            chunkOffset += ReadScalarSwapped(src + chunkOffset, &chunk->as.init.numInboundStreams);
            ReadScalarSwapped(src + chunkOffset, &chunk->as.init.initialTsn);

            break;
        }
        default:
            break;
    }

    int32_t valueLength = chunk->length - ChunkHeaderSize;
    int32_t pad = PadSize(valueLength, ChunkHeaderSize);
    offset += valueLength + pad;

    if (offset > len)
        return 0;

    left = len - offset;
  }

  return 1;
}

size_t SerializeSctpPacket(const SctpPacket* packet, const SctpChunk* chunks,
                           size_t numChunks, uint8_t* dst, size_t dstLen) {
    if (dstLen < ValidPacketSize)
        return 0;

  size_t offset = WriteScalar(dst, htons(packet->sourcePort));
  offset += WriteScalar(dst + offset, htons(packet->destinationPort));
  offset += WriteScalar(dst + offset, htonl(packet->verificationTag));

  size_t crcOffset = offset;
  offset += WriteScalar(dst + offset, 0);

  for (size_t i = 0; i < numChunks; i++) {
      const SctpChunk *chunk = &chunks[i];
      if (SctpPacketSize(chunk) > dstLen - offset)
          return 0;

    offset += WriteScalar(dst + offset, chunk->type);
    offset += WriteScalar(dst + offset, chunk->flags);
    offset += WriteScalar(dst + offset, htons(chunk->length));

    switch (chunk->type) {
      case Sctp_Data: {
        auto* dc = &chunk->as.data;
        offset += WriteScalar(dst + offset, htonl(dc->tsn));
        offset += WriteScalar(dst + offset, htons(dc->streamId));
        offset += WriteScalar(dst + offset, htons(dc->streamSeq));
        offset += WriteScalar(dst + offset, htonl(dc->protoId));
        memcpy(dst + offset, dc->userData, dc->userDataLength);

        int32_t pad = PadSize(dc->userDataLength, 4);
        offset += dc->userDataLength + pad;

        break;
      }
      case Sctp_InitAck: {
        offset += WriteScalar(dst + offset, htonl(chunk->as.init.initiateTag));
        offset += WriteScalar(dst + offset, htonl(chunk->as.init.windowCredit));
        offset += WriteScalar(dst + offset, htons(chunk->as.init.numOutboundStreams));
        offset += WriteScalar(dst + offset, htons(chunk->as.init.numInboundStreams));
        offset += WriteScalar(dst + offset, htonl(chunk->as.init.initialTsn));

        offset += WriteScalar(dst + offset, htons(Sctp_StateCookie));
        offset += WriteScalar(dst + offset, htons(8));
        offset += WriteScalar(dst + offset, htonl(0xB00B1E5));
        offset += WriteScalar(dst + offset, htons(Sctp_ForwardTsn));
        offset += WriteScalar(dst + offset, htons(4));

        break;
      }
      case Sctp_Sack: {
        auto* sack = &chunk->as.sack;
        offset += WriteScalar(dst + offset, htonl(sack->cumulativeTsnAck));
        offset += WriteScalar(dst + offset, htonl(sack->advRecvWindow));
        offset += WriteScalar(dst + offset, htons(sack->numGapAckBlocks));
        offset += WriteScalar(dst + offset, htons(sack->numDupTsn));

        break;
      }
      case Sctp_Heartbeat:
      case Sctp_HeartbeatAck: {
        auto* hb = &chunk->as.heartbeat;
        offset += WriteScalar(dst + offset, htons(1));
        offset += WriteScalar(dst + offset, htons(hb->heartbeatInfoLen + 4));
        memcpy(dst + offset, hb->heartbeatInfo, hb->heartbeatInfoLen);
        offset += hb->heartbeatInfoLen + PadSize(hb->heartbeatInfoLen, 4);

        break;
      }
      case Sctp_Shutdown: {
        auto* shutdown = &chunk->as.shutdown;
        offset += WriteScalar(dst + offset, htonl(shutdown->cumulativeTsnAck));

        break;
      }
      case SctpChunk_ForwardTsn: {
        auto* forwardTsn = &chunk->as.forwardTsn;
        offset += WriteScalar(dst + offset, htonl(forwardTsn->newCumulativeTsn));

        break;
      }
      default:
        break;
    }
  }

  const uint32_t crc = SctpCRC32(dst, offset);
  WriteScalar(dst + crcOffset, htonl(crc));

  return offset;
}

int32_t SctpDataChunkLength(int32_t userDataLength) {
  return ValidPacketSize + userDataLength;
}

int32_t SctpChunkLength(int32_t contentLength) { return ChunkHeaderSize + contentLength; }
