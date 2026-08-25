// srs_simple — 원본: trunk/src/kernel/srs_kernel_io.hpp
// S1~S9에서는 protocol/srs_protocol_io.hpp에 병합되어 있었으나, S10(HLS)에서
// kernel 계층의 소비자(SrsFileWriter, SrsTsContext)가 생겨 원본 위치로 복원했다.
// ISrsWriter는 원본의 ISrsStreamWriter(write) + ISrsVectorWriter(writev) 역할 (CLAUDE.md §5.6).
#ifndef SRS_KERNEL_IO_HPP
#define SRS_KERNEL_IO_HPP

#include <srs_core.hpp>

#include <sys/types.h>
#include <sys/uio.h>

// The reader to read data from a channel.
class ISrsReader
{
public:
    virtual ~ISrsReader() {}
public:
    // Read bytes from reader.
    // @param nread How many bytes were read from the channel. NULL to ignore.
    virtual srs_error_t read(void* buf, size_t size, ssize_t* nread) = 0;
};

// The writer to write stream data to a channel.
// 원본의 ISrsStreamWriter(write) + ISrsVectorWriter(writev) 병합.
class ISrsWriter
{
public:
    virtual ~ISrsWriter() {}
public:
    // Write bytes over the writer.
    // @param nwrite The actual written bytes. NULL to ignore.
    virtual srs_error_t write(void* buf, size_t size, ssize_t* nwrite) = 0;
    // Write an iov over the writer.
    virtual srs_error_t writev(const iovec* iov, int iov_size, ssize_t* nwrite) = 0;
};

#endif
