// srs_simple — 원본: trunk/src/kernel/srs_kernel_buffer.hpp
// SrsBuffer 유지 (ISrsCodec 계층은 라이브 경로에 불필요).
// SrsBitBuffer는 S16에서 SPS 해상도 파싱용으로 서브셋 복원 (CLAUDE.md §5.6 S16).
#ifndef SRS_KERNEL_BUFFER_HPP
#define SRS_KERNEL_BUFFER_HPP

#include <srs_core.hpp>

#include <string>

/**
 * bytes utility, used to:
 * convert basic types to bytes,
 * build basic types from bytes.
 * 네트워크 바이트오더(빅엔디언)가 기본이고, 청크 헤더의 stream_id만 리틀엔디언(le4bytes)이다.
 * @remark the buffer never manage the bytes, user must manage it.
 */
class SrsBuffer
{
private:
    // current position at bytes.
    char* p;
    // the bytes data for buffer to read or write.
    char* bytes;
    // the total number of bytes.
    int nb_bytes;
public:
    // Create buffer with data b and size nn.
    // @remark User must free the data b.
    SrsBuffer(char* b, int nn);
    ~SrsBuffer();
public:
    // Copy the object, keep position of buffer.
    SrsBuffer* copy();
    // Get the data and head of buffer.
    //      current-bytes = head() = data() + pos()
    char* data();
    char* head();
    // Get the total size of buffer.
    //      left-bytes = size() - pos()
    int size();
    // Get the current buffer position.
    int pos();
    // Left bytes in buffer, total size() minus the current pos().
    int left();
    // Whether buffer is empty.
    bool empty();
    // Whether buffer is able to supply required size of bytes.
    // @remark User should check buffer by require then do read/write.
    bool require(int required_size);
public:
    // Skip some size.
    // @param size can be any value. positive to forward; negative to backward.
    void skip(int size);
public:
    // Read 1bytes char from buffer.
    int8_t read_1bytes();
    // Read 2bytes int from buffer.
    int16_t read_2bytes();
    // Read 3bytes int from buffer.
    int32_t read_3bytes();
    // Read 4bytes int from buffer.
    int32_t read_4bytes();
    int32_t read_le4bytes();
    // Read 8bytes int from buffer.
    int64_t read_8bytes();
    // Read string from buffer, length specifies by param len.
    std::string read_string(int len);
    // Read bytes from buffer, length specifies by param len.
    void read_bytes(char* data, int size);
public:
    // Write 1bytes char to buffer.
    void write_1bytes(int8_t value);
    // Write 2bytes int to buffer.
    void write_2bytes(int16_t value);
    // Write 3bytes int to buffer.
    void write_3bytes(int32_t value);
    // Write 4bytes int to buffer.
    void write_4bytes(int32_t value);
    void write_le4bytes(int32_t value);
    // Write 8bytes int to buffer.
    void write_8bytes(int64_t value);
    // Write string to buffer
    void write_string(std::string value);
    // Write bytes to buffer
    void write_bytes(char* data, int size);
};

/**
 * the bit buffer, base on SrsBuffer,
 * for exmaple, the h.264 avc buffer is bit buffer.
 * 원본의 read_bits/ue/se류 없이 read_bit만 유지 — SPS 해상도 파싱(exp-Golomb 헬퍼가
 * read_bit 위에서 동작)에 필요한 최소만 (§5.6 S16).
 */
class SrsBitBuffer
{
private:
    int8_t cb;
    uint8_t cb_left;
    SrsBuffer* stream;
public:
    SrsBitBuffer(SrsBuffer* b);
    ~SrsBitBuffer();
public:
    bool empty();
    int8_t read_bit();
};

#endif
