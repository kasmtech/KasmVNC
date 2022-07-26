#include "WuSctp.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include "CRC32.h"
#include "WuBufferOp.h"
#include "WuMath.h"
#include "WuNetwork.h"

int32_t ParseSctpPacket(const uint8_t* buf, size_t len, SctpPacket* packet,
                        SctpChunk* chunks, size_t maxChunks, size_t* nChunk) {
  if (len < 16) {
    return 0;
  }

  int32_t offset = ReadScalarSwapped(buf, &packet->sourcePort);
  offset += ReadScalarSwapped(buf + offset, &packet->destionationPort);
  offset += ReadScalarSwapped(buf + offset, &packet->verificationTag);
  offset += ReadScalarSwapped(buf + offset, &packet->checkSum);

  int32_t left = len - offset;

  size_t chunkNum = 0;
  while (left >= 4 && chunkNum < maxChunks) {
    SctpChunk* chunk = &chunks[chunkNum++];

    offset += ReadScalarSwapped(buf + offset, &chunk->type);
    offset += ReadScalarSwapped(buf + offset, &chunk->flags);
    offset += ReadScalarSwapped(buf + offset, &chunk->length);

    *nChunk += 1;

    if (chunk->type == Sctp_Data) {
      auto* p = &chunk->as.data;
      size_t chunkOffset = ReadScalarSwapped(buf + offset, &p->tsn);
      chunkOffset +=
          ReadScalarSwapped(buf + offset + chunkOffset, &p->streamId);
      chunkOffset +=
          ReadScalarSwapped(buf + offset + chunkOffset, &p->streamSeq);
      chunkOffset += ReadScalarSwapped(buf + offset + chunkOffset, &p->protoId);
      p->userDataLength = Max(int32_t(chunk->length) - 16, 0);
      p->userData = buf + offset + chunkOffset;
    } else if (chunk->type == Sctp_Sack) {
      auto* sack = &chunk->as.sack;
      size_t chunkOffset =
          ReadScalarSwapped(buf + offset, &sack->cumulativeTsnAck);
      chunkOffset +=
          ReadScalarSwapped(buf + offset + chunkOffset, &sack->advRecvWindow);
      chunkOffset +=
          ReadScalarSwapped(buf + offset + chunkOffset, &sack->numGapAckBlocks);
      ReadScalarSwapped(buf + offset + chunkOffset, &sack->numDupTsn);
    } else if (chunk->type == Sctp_Heartbeat) {
      auto* p = &chunk->as.heartbeat;
      size_t chunkOffset = 2;  // skip type
      uint16_t heartbeatLen;
      chunkOffset +=
          ReadScalarSwapped(buf + offset + chunkOffset, &heartbeatLen);
      p->heartbeatInfoLen = int32_t(heartbeatLen) - 4;
      p->heartbeatInfo = buf + offset + chunkOffset;
    } else if (chunk->type == Sctp_Init) {
      size_t chunkOffset =
          ReadScalarSwapped(buf + offset, &chunk->as.init.initiateTag);
      chunkOffset += ReadScalarSwapped(buf + offset + chunkOffset,
                                       &chunk->as.init.windowCredit);
      chunkOffset += ReadScalarSwapped(buf + offset + chunkOffset,
                                       &chunk->as.init.numOutboundStreams);
      chunkOffset += ReadScalarSwapped(buf + offset + chunkOffset,
                                       &chunk->as.init.numInboundStreams);
      ReadScalarSwapped(buf + offset + chunkOffset, &chunk->as.init.initialTsn);
    }

    int32_t valueLength = chunk->length - 4;
    int32_t pad = PadSize(valueLength, 4);
    offset += valueLength + pad;
    left = len - offset;
  }

  return 1;
}

size_t SerializeSctpPacket(const SctpPacket* packet, const SctpChunk* chunks,
                           size_t numChunks, uint8_t* dst, size_t dstLen) {
  size_t offset = WriteScalar(dst, htons(packet->sourcePort));
  offset += WriteScalar(dst + offset, htons(packet->destionationPort));
  offset += WriteScalar(dst + offset, htonl(packet->verificationTag));

  size_t crcOffset = offset;
  offset += WriteScalar(dst + offset, uint32_t(0));

  for (size_t i = 0; i < numChunks; i++) {
    const SctpChunk* chunk = &chunks[i];

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
        offset +=
            WriteScalar(dst + offset, htons(chunk->as.init.numOutboundStreams));
        offset +=
            WriteScalar(dst + offset, htons(chunk->as.init.numInboundStreams));
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
        offset +=
            WriteScalar(dst + offset, htonl(forwardTsn->newCumulativeTsn));
        break;
      }
      default:
        break;
    }
  }

  uint32_t crc = SctpCRC32(dst, offset);
  WriteScalar(dst + crcOffset, htonl(crc));

  return offset;
}

int32_t SctpDataChunkLength(int32_t userDataLength) {
  return 16 + userDataLength;
}

int32_t SctpChunkLength(int32_t contentLength) { return 4 + contentLength; }
