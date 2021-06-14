#include "mp4.h"

#define chk_ptr if (!ptr) return BUF_INCORRECT;
#define chk_realloc { enum BufError err; err = try_to_realloc(ptr, pos); if (err != BUF_OK) return err; }

char* buf_error_to_str(const enum BufError err) {
    switch (err) {
        case BUF_OK: return "BUF_OK";
        case BUF_ENDOFBUF_ERROR: return "BUF_ENDOFBUF_ERROR";
        case BUF_MALLOC_ERROR: return "BUF_MALLOC_ERROR";
        case BUF_INCORRECT: return "BUF_INCORRECT";
        default: { static char str[32]; sprintf(str, "Unknown(%d)", err); return str; }
    }
}

enum BufError try_to_realloc(struct BitBuf *ptr, const uint32_t min_size) {
    chk_ptr
    uint32_t new_size = ptr->size + min_size + 1024;
    char* new_buf = realloc(ptr->buf, new_size);
    if (new_buf == NULL) return BUF_MALLOC_ERROR;
    ptr->buf = new_buf;
    ptr->size = new_size;
    return BUF_OK;
}

enum BufError put_skip(struct BitBuf *ptr, const uint32_t count) {
    chk_ptr
    uint32_t pos = ptr->offset + count;
    if (pos >= ptr->size) chk_realloc
    for (uint32_t i = 0; i < count; i++) ptr->buf[ptr->offset + i] = 0;
    ptr->offset = pos;
    return BUF_OK;
}

enum BufError put_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint8_t* data, const uint32_t size) {
    chk_ptr
    uint32_t pos = offset + size;
    if (pos >= ptr->size) chk_realloc
    for (uint32_t i = 0; i < size; i++) ptr->buf[offset + i] = data[i];
    return BUF_OK;
}
enum BufError put(struct BitBuf *ptr, const uint8_t* data, const uint32_t size) {
    chk_ptr
    enum BufError err = put_to_offset(ptr, ptr->offset, data, size); chk_err
    ptr->offset += size;
    return BUF_OK;
}

enum BufError put_u8_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint8_t val) {
    chk_ptr
    uint32_t pos = offset + sizeof(uint8_t);
    if (pos >= ptr->size) chk_realloc
    ptr->buf[offset + 0] = val & 0xff;
    return BUF_OK;
}
enum BufError put_u8(struct BitBuf *ptr, uint8_t val) {
    chk_ptr
    enum BufError err = put_u8_to_offset(ptr, ptr->offset, val); chk_err
    ptr->offset += sizeof(uint8_t);
    return BUF_OK;
}

enum BufError put_u16_be_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint16_t val) {
    chk_ptr
    uint32_t pos = offset + sizeof(uint16_t);
    if (pos >= ptr->size) chk_realloc
    ptr->buf[offset + 0] = (val >> 8) & 0xff;
    ptr->buf[offset + 1] = (val >> 0) & 0xff;
    return BUF_OK;
}
enum BufError put_u16_be(struct BitBuf *ptr, const uint16_t val) {
    chk_ptr
    enum BufError err = put_u16_be_to_offset(ptr, ptr->offset, val); chk_err
    ptr->offset += sizeof(uint16_t);
    return BUF_OK;
}

enum BufError put_u16_le_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint16_t val) {
    chk_ptr
    uint32_t pos = offset + sizeof(uint16_t);
    if (pos >= ptr->size) chk_realloc
    ptr->buf[offset + 0] = (val >> 0) & 0xff;
    ptr->buf[offset + 1] = (val >> 8) & 0xff;
    return BUF_OK;
}
enum BufError put_u16_le(struct BitBuf *ptr, const uint16_t val) {
    chk_ptr
    enum BufError err = put_u16_le_to_offset(ptr, ptr->offset, val); chk_err
    ptr->offset += sizeof(uint16_t);
    return BUF_OK;
}

enum BufError put_u32_be_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint32_t val) {
    chk_ptr
    uint32_t pos = offset + sizeof(uint32_t);
    if (pos >= ptr->size) chk_realloc
    ptr->buf[offset + 0] = (val >> 24) & 0xff;
    ptr->buf[offset + 1] = (val >> 16) & 0xff;
    ptr->buf[offset + 2] = (val >>  8) & 0xff;
    ptr->buf[offset + 3] = (val >>  0) & 0xff;
    return BUF_OK;
}
enum BufError put_u32_be(struct BitBuf *ptr, const uint32_t val) {
    chk_ptr
    enum BufError err = put_u32_be_to_offset(ptr, ptr->offset, val); chk_err
    ptr->offset += sizeof(uint32_t);
    return BUF_OK;
}
enum BufError put_i32_be(struct BitBuf *ptr, const int32_t val) {
    chk_ptr
    enum BufError err = put_u32_be_to_offset(ptr, ptr->offset, val); chk_err
    ptr->offset += sizeof(int32_t);
    return BUF_OK;
}

enum BufError put_u64_be_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint64_t val) {
    chk_ptr
    uint32_t pos = offset + sizeof(uint64_t);
    if (pos > ptr->size) chk_realloc
    ptr->buf[offset + 0] = (val >> 56) & 0xff;
    ptr->buf[offset + 1] = (val >> 48) & 0xff;
    ptr->buf[offset + 2] = (val >> 40) & 0xff;
    ptr->buf[offset + 3] = (val >> 32) & 0xff;
    ptr->buf[offset + 4] = (val >> 24) & 0xff;
    ptr->buf[offset + 5] = (val >> 16) & 0xff;
    ptr->buf[offset + 6] = (val >>  8) & 0xff;
    ptr->buf[offset + 7] = (val >>  0) & 0xff;
    return BUF_OK;
}
enum BufError put_u64_be(struct BitBuf *ptr, const uint64_t val) {
    chk_ptr
    enum BufError err = put_u64_be_to_offset(ptr, ptr->offset, val); chk_err
    ptr->offset += sizeof(uint64_t);
    return BUF_OK;
}

enum BufError put_u32_le_to_offset(struct BitBuf *ptr, const uint32_t offset, const uint32_t val) {
    chk_ptr
    uint32_t pos = offset + 4;
    if (pos >= ptr->size) chk_realloc
    ptr->buf[offset + 0] = (val >>  0) & 0xff;
    ptr->buf[offset + 1] = (val >>  8) & 0xff;
    ptr->buf[offset + 2] = (val >> 16) & 0xff;
    ptr->buf[offset + 3] = (val >> 24) & 0xff;
    return BUF_OK;
}
enum BufError put_u32_le(struct BitBuf *ptr, const uint32_t val) {
    chk_ptr
    enum BufError err = put_u32_le_to_offset(ptr, ptr->offset, val); chk_err
    ptr->offset += sizeof(uint32_t);
    return BUF_OK;
}

enum BufError put_str4_to_offset(struct BitBuf *ptr, const uint32_t offset, const char str[4]) {
    chk_ptr
    uint32_t pos = offset + 4;
    if (pos >= ptr->size) chk_realloc
    for (uint8_t i = 0; i < 4; i++) { ptr->buf[offset + i] = str[i]; }
    return BUF_OK;
}
enum BufError put_str4(struct BitBuf *ptr, const char str[4]) {
    chk_ptr
    enum BufError err = put_str4_to_offset(ptr, ptr->offset, str); chk_err
    ptr->offset += 4;
    return BUF_OK;
}
enum BufError put_counted_str_to_offset(struct BitBuf *ptr, const uint32_t offset, const char *str, const uint32_t len) {
    chk_ptr
    uint32_t pos = offset + len + 1;
    if (pos >= ptr->size) chk_realloc
    for (uint32_t i = 0; i < len+1; i++) { ptr->buf[offset + i] = str[i]; }
    ptr->buf[pos] = 0;
    return BUF_OK;
    }
enum BufError put_counted_str(struct BitBuf *ptr, const char *str, const uint32_t len) {
    chk_ptr
    enum BufError err = put_counted_str_to_offset(ptr, ptr->offset, str, len); chk_err
    ptr->offset += len+1;
    return BUF_OK;
}

// enum BufError hexStr(struct BitBuf *ptr, char *str) : string {
//     let str = '';
//     for(let i = 0; i < this.offset; i++) {
//         if (i % 40 === 0) str += '\n';
//         if (i % 4 === 0 && i+1 < this.data.length) {
//             if (i > 0 && i % 40 !== 0) str += ' ';
//             str += '0x';
//         }
//         str += decimalToHex(this.data[i]);
//     }
//     return str;
// }
