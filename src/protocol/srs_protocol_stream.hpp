// srs_simple — 원본: trunk/src/protocol/srs_protocol_stream.hpp
// merged-read(SRS_PERF_MERGED_READ)와 set_buffer 리사이즈는 성능 최적화라 제거했다.
#ifndef SRS_PROTOCOL_STREAM_HPP
#define SRS_PROTOCOL_STREAM_HPP

#include <srs_core.hpp>

#include <srs_protocol_io.hpp>

/**
 * the buffer provides a bytes cache for the protocol. generally,
 * the protocol receives data from the socket, puts it into the buffer, and decodes it to an RTMP message.
 * Usage:
 *       ISrsReader* r = ......;
 *       SrsFastStream* fb = ......;
 *       fb->grow(r, 1024);
 *       char* header = fb->read_slice(100);
 *       char* payload = fb->read_slice(924);
 */
class SrsFastStream
{
private:
    // the user-space buffer to be filled by the reader,
    // which uses a fast index and resets when the chunk body is read ok.
    // ptr to the current read position.
    char* p;
    // ptr to the content end.
    char* end;
    // ptr to the buffer.
    //      buffer <= p <= end <= buffer+nb_buffer
    char* buffer;
    // the size of the buffer.
    int nb_buffer;
public:
    // If buffer is 0, use the default size.
    SrsFastStream(int size = 0);
    virtual ~SrsFastStream();
public:
    // get the size of the current bytes in the buffer.
    virtual int size();
    // get the current bytes in the buffer.
    // @remark the user should use read_slice() if possible,
    //       the bytes() is used to test bytes, for example, to detect the bytes schema.
    virtual char* bytes();
public:
    // read 1 byte from the buffer, move to the next bytes.
    // @remark assert grow(1) was already called.
    virtual char read_1byte();
    // read a slice of size bytes, move to the next bytes.
    // the user can use this char* ptr directly, and should never free it.
    // @remark the user can use the returned ptr until grow(size),
    //       for the returned ptr may be invalid after grow(x).
    virtual char* read_slice(int size);
    // skip some bytes in the buffer.
    // @param size the bytes to skip. positive to next; negative to previous.
    // @remark assert grow(size) was already called.
    virtual void skip(int size);
public:
    // grow the buffer to at least the required size, loop to read from skt to fill.
    // @param reader, read more bytes from the reader to fill the buffer to the required size.
    // @param required_size, loop to fill to ensure the buffer reaches the required size.
    // @remark, we may actually read more than required_size, maybe 4k for example.
    virtual srs_error_t grow(ISrsReader* reader, int required_size);
};

#endif
