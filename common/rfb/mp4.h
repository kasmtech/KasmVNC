#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define chk_err  if (err != BUF_OK) { printf("Error buf: %s     %s(...)    %s:%d\n", buf_error_to_str(err), __func__, __FILE__, __LINE__); return err; }
#define chk_err_continue  if (err != BUF_OK) { printf("Error buf: %s     %s(...)    %s:%d\n", buf_error_to_str(err), __func__, __FILE__, __LINE__); continue; }

enum BufError {
    BUF_OK = 0,
    BUF_ENDOFBUF_ERROR,
    BUF_MALLOC_ERROR,
    BUF_INCORRECT
};
char* buf_error_to_str(const enum BufError err);

struct BitBuf {
    char *buf;
    uint32_t size;
    uint32_t offset;
};

enum BufError put_skip(struct BitBuf *ptr, const uint32_t count);
enum BufError put_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint8_t* data, const uint32_t size);
enum BufError put(struct BitBuf *ptr, const uint8_t* data, const uint32_t size);
enum BufError put_u8_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint8_t val);
enum BufError put_u8(struct BitBuf *ptr, uint8_t val);
enum BufError put_u16_be_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint16_t val);
enum BufError put_u16_be(struct BitBuf *ptr, const uint16_t val);
enum BufError put_u16_le_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint16_t val);
enum BufError put_u16_le(struct BitBuf *ptr, const uint16_t val);
enum BufError put_u32_be_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint32_t val);
enum BufError put_u32_be(struct BitBuf *ptr, const uint32_t val);
enum BufError put_i32_be(struct BitBuf *ptr, const int32_t val);
enum BufError put_u64_be_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint64_t val);
enum BufError put_u64_be(struct BitBuf *ptr, const uint64_t val);
enum BufError put_u32_le_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint32_t val);
enum BufError put_u32_le(struct BitBuf *ptr, const uint32_t val);
enum BufError put_str4_to_offset(struct BitBuf *ptr, const uint32_t offset, const char str[4]);
enum BufError put_str4(struct BitBuf *ptr, const char str[4]);
enum BufError put_counted_str_to_offset(struct BitBuf *ptr, const uint32_t offset, const char *str, const uint32_t len);
enum BufError put_counted_str(struct BitBuf *ptr, const char *str, const uint32_t len);

struct MoovInfo {
    uint8_t profile_idc;
    uint8_t level_idc;
    uint8_t *sps;
    uint16_t sps_length;
    uint8_t *pps;
    uint16_t pps_length;
    uint16_t width;
    uint16_t height;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t creation_time;
    uint32_t timescale;
};

enum BufError write_header(struct BitBuf *ptr, struct MoovInfo *moov_info);

extern uint32_t pos_sequence_number;
extern uint32_t pos_base_data_offset;
extern uint32_t pos_base_media_decode_time;

struct SampleInfo {
    uint32_t duration;
    uint32_t decode_time;
    uint32_t composition_time;
    uint32_t composition_offset;
    uint32_t size;
    uint32_t flags;
};

enum BufError write_mdat(struct BitBuf *ptr, const uint8_t* data, const uint32_t origlen, const uint32_t len);
enum BufError write_moof(struct BitBuf *ptr,
    const uint32_t sequence_number,
    const uint64_t base_data_offset,
    const uint64_t base_media_decode_time,
    const uint32_t default_sample_duration,
    const struct SampleInfo *samples_info, const uint32_t samples_info_len);

enum NalUnitType { //   Table 7-1 NAL unit type codes
    NalUnitType_Unspecified = 0,                // Unspecified
    NalUnitType_CodedSliceNonIdr = 1,           // Coded slice of a non-IDR picture
    NalUnitType_CodedSliceDataPartitionA = 2,   // Coded slice data partition A
    NalUnitType_CodedSliceDataPartitionB = 3,   // Coded slice data partition B
    NalUnitType_CodedSliceDataPartitionC = 4,   // Coded slice data partition C
    NalUnitType_CodedSliceIdr = 5,              // Coded slice of an IDR picture
    NalUnitType_SEI = 6,                        // Supplemental enhancement information (SEI)
    NalUnitType_SPS = 7,                        // Sequence parameter set
    NalUnitType_PPS = 8,                        // Picture parameter set
    NalUnitType_AUD = 9,                        // Access unit delimiter
    NalUnitType_EndOfSequence = 10,             // End of sequence
    NalUnitType_EndOfStream = 11,               // End of stream
    NalUnitType_Filler = 12,                    // Filler data
    NalUnitType_SpsExt = 13,                    // Sequence parameter set extension
    // 14..18           // Reserved
            NalUnitType_CodedSliceAux = 19,  // Coded slice of an auxiliary coded picture without partitioning
    // 20..23           // Reserved
    // 24..31           // Unspecified
};

struct NAL {
    char *data;
    uint64_t data_size;
    uint32_t picture_order_count;

    // NAL header
    bool forbidden_zero_bit;
    uint8_t ref_idc;
    uint8_t unit_type_value;
    enum NalUnitType unit_type;
};

static inline const char* nal_type_to_str(const enum NalUnitType nal_type) {
    switch (nal_type) {
        case NalUnitType_Unspecified: return "Unspecified";
        case NalUnitType_CodedSliceNonIdr: return "CodedSliceNonIdr";
        case NalUnitType_CodedSliceDataPartitionA: return "CodedSliceDataPartitionA";
        case NalUnitType_CodedSliceDataPartitionB: return "CodedSliceDataPartitionB";
        case NalUnitType_CodedSliceDataPartitionC: return "CodedSliceDataPartitionC";
        case NalUnitType_CodedSliceIdr: return "CodedSliceIdr";
        case NalUnitType_SEI: return "SEI";
        case NalUnitType_SPS: return "SPS";
        case NalUnitType_PPS: return "PPS";
        case NalUnitType_AUD: return "AUD";
        case NalUnitType_EndOfSequence: return "EndOfSequence";
        case NalUnitType_EndOfStream: return "EndOfStream";
        case NalUnitType_Filler: return "Filler";
        case NalUnitType_SpsExt: return "SpsExt";
        case NalUnitType_CodedSliceAux: return "CodedSliceAux";
        default: return "Unknown";
    }
}

static inline void nal_parse_header(struct NAL *nal, const char first_byte) {
    nal->forbidden_zero_bit = ((first_byte & 0x80) >> 7) == 1;
    nal->ref_idc = (first_byte & 0x60) >> 5;
    nal->unit_type = (enum NalUnitType) ((first_byte & 0x1f) >> 0);
}

static inline bool nal_chk4(const uint8_t *buf, const uint32_t offset) {
    if (buf[offset] == 0x00 && buf[offset+1] == 0x00 && buf[offset+2] == 0x01) { return true; }
    if (buf[offset] == 0x00 && buf[offset+1] == 0x00 && buf[offset+2] == 0x00 && buf[offset+3] == 0x01) { return true; }
    return false;
}

static inline bool nal_chk3(const uint8_t *buf, const uint32_t offset) {
    if (buf[offset] == 0x00 && buf[offset+1] == 0x00 && buf[offset+2] == 0x01) { return true; }
    return false;
}

extern uint32_t default_sample_size;

struct Mp4State {
    bool header_sent;

    uint32_t sequence_number;
    uint64_t base_data_offset;
    uint64_t base_media_decode_time;
    uint32_t default_sample_duration;

    uint32_t nals_count;
};

struct Mp4Context {
    char buf_sps[128];
    uint16_t buf_sps_len;
    char buf_pps[128];
    uint16_t buf_pps_len;

    uint16_t w, h, framerate;

    struct BitBuf buf_header;
    struct BitBuf buf_moof;
    struct BitBuf buf_mdat;
};


enum BufError set_slice(struct Mp4Context *ctx, const uint8_t* nal_data, const uint32_t origlen, const uint32_t nal_len, const enum NalUnitType unit_type);
void set_sps(struct Mp4Context *ctx, const uint8_t* nal_data, const uint32_t nal_len);
void set_pps(struct Mp4Context *ctx, const uint8_t* nal_data, const uint32_t nal_len);

enum BufError get_header(struct Mp4Context *ctx, struct BitBuf *ptr);

enum BufError set_mp4_state(struct Mp4Context *ctx, struct Mp4State *state);
enum BufError get_moof(struct Mp4Context *ctx, struct BitBuf *ptr);
enum BufError get_mdat(struct Mp4Context *ctx, struct BitBuf *ptr);

#ifdef __cplusplus
} // extern C
#endif
