// srs_simple — 원본: trunk/src/protocol/srs_protocol_io.hpp
// ISrsReader/ISrsWriter는 S10에서 원본 위치(kernel/srs_kernel_io.hpp)로 복원 — CLAUDE.md §5.6.
#ifndef SRS_PROTOCOL_IO_HPP
#define SRS_PROTOCOL_IO_HPP

#include <srs_core.hpp>

#include <sys/types.h>
#include <sys/uio.h>

#include <srs_kernel_io.hpp>

/**
 * The system io reader/writer architecture:
 *   ISrsReader (read)         ISrsWriter (write/writev)     ← kernel/srs_kernel_io.hpp
 *        |                          |
 *   ISrsProtocolReader         ISrsProtocolWriter
 *   (+read_fully, recv 타임아웃)  (+send 타임아웃)
 *        \                        /
 *         ISrsProtocolReadWriter   ← SrsProtocol/핸드셰이크가 의존하는 유일한 소켓 추상화.
 *                                    실소켓(SrsStSocket)과 utest(MockBufferIO)가 구현한다.
 */

// Get the statistic of channel.
class ISrsProtocolStatistic
{
public:
    virtual ~ISrsProtocolStatistic() {}
public:
    // Get the total recv bytes over underlay fd.
    virtual int64_t get_recv_bytes() = 0;
    // Get the total send bytes over underlay fd.
    virtual int64_t get_send_bytes() = 0;
};

// The reader for the protocol to read from whatever channel.
class ISrsProtocolReader : public ISrsReader, virtual public ISrsProtocolStatistic
{
public:
    virtual ~ISrsProtocolReader() {}
// For protocol
public:
    // Set the timeout tm in srs_utime_t for recv bytes from peer.
    // @remark Use SRS_UTIME_NO_TIMEOUT to never timeout.
    virtual void set_recv_timeout(srs_utime_t tm) = 0;
    // Get the timeout in srs_utime_t for recv bytes from peer.
    virtual srs_utime_t get_recv_timeout() = 0;
// For handshake.
public:
    // Read specified size bytes of data.
    // @param nread, the actually read size, NULL to ignore.
    virtual srs_error_t read_fully(void* buf, size_t size, ssize_t* nread) = 0;
};

// The writer for the protocol to write to whatever channel.
class ISrsProtocolWriter : public ISrsWriter, virtual public ISrsProtocolStatistic
{
public:
    virtual ~ISrsProtocolWriter() {}
// For protocol
public:
    // Set the timeout tm in srs_utime_t for send bytes to peer.
    // @remark Use SRS_UTIME_NO_TIMEOUT to never timeout.
    virtual void set_send_timeout(srs_utime_t tm) = 0;
    // Get the timeout in srs_utime_t for send bytes to peer.
    virtual srs_utime_t get_send_timeout() = 0;
};

// The reader and writer.
class ISrsProtocolReadWriter : public ISrsProtocolReader, public ISrsProtocolWriter
{
public:
    virtual ~ISrsProtocolReadWriter() {}
};

#endif
