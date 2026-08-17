// srs_simple — 원본: trunk/src/protocol/srs_protocol_stream.hpp
// merged-read(SRS_PERF_MERGED_READ)와 set_buffer 리사이즈는 성능 최적화라 제거했다.
#ifndef SRS_PROTOCOL_STREAM_HPP
#define SRS_PROTOCOL_STREAM_HPP

#include <srs_core.hpp>

#include <srs_protocol_io.hpp>

/**
 * the buffer provices bytes cache for protocol. generally,
 * protocol recv data from socket, put into buffer, decode to RTMP message.
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
    // the user-space buffer to fill by reader,
    // which use fast index and reset when chunk body read ok.
    // ptr to the current read position.
    char* p;
    // ptr to the content end.
    char* end;
    // ptr to the buffer.
    //      buffer <= p <= end <= buffer+nb_buffer
    char* buffer;
    // the size of buffer.
    int nb_buffer;
public:
    // If buffer is 0, use default size.
    SrsFastStream(int size = 0);
    virtual ~SrsFastStream();
public:
    // get the size of current bytes in buffer.
    virtual int size();
    // get the current bytes in buffer.
    // @remark user should use read_slice() if possible,
    //       the bytes() is used to test bytes, for example, to detect the bytes schema.
    virtual char* bytes();
public:
    // read 1byte from buffer, move to next bytes.
    // @remark assert buffer already grow(1).
    virtual char read_1byte();
    // read a slice in size bytes, move to next bytes.
    // user can use this char* ptr directly, and should never free it.
    // @remark user can use the returned ptr util grow(size),
    //       for the ptr returned maybe invalid after grow(x).
    virtual char* read_slice(int size);
    // skip some bytes in buffer.
    // @param size the bytes to skip. positive to next; negative to previous.
    // @remark assert buffer already grow(size).
    virtual void skip(int size);
public:
    // grow buffer to atleast required size, loop to read from skt to fill.
    // @param reader, read more bytes from reader to fill the buffer to required size.
    // @param required_size, loop to fill to ensure buffer size to required.
    // @remark, we actually maybe read more than required_size, maybe 4k for example.
    virtual srs_error_t grow(ISrsReader* reader, int required_size);
};

#endif
