// srs_simple — 원본: trunk/src/kernel/srs_kernel_file.hpp
// HLS(S10)가 세그먼트(.ts)/플레이리스트(.m3u8)를 쓴다. 읽기 쪽이던 HTTP 정적 서버는
// S11에서 제거돼(외부 nginx), SrsFileReader는 utest만 사용한다.
// 원본 대비 제거: set_iobuf_size/버퍼링, seek 계열, mock용 함수 포인터 훅 — CLAUDE.md §5.6 S10.
#ifndef SRS_KERNEL_FILE_HPP
#define SRS_KERNEL_FILE_HPP

#include <srs_core.hpp>

#include <stdio.h>

#include <string>

#include <srs_kernel_io.hpp>

// The file writer, to write to a file.
// 원본은 ISrsWriteSeeker(kernel_io) 구현 — seek 계열을 제거해 ISrsWriter만 구현한다.
class SrsFileWriter : public ISrsWriter
{
private:
    std::string path_;
    FILE* fp_;
public:
    SrsFileWriter();
    virtual ~SrsFileWriter();
public:
    // open the file writer, in truncate mode.
    // @param p a string that indicates the path of the file to open.
    virtual srs_error_t open(std::string p);
    // close the current writer.
    // @remark the user can reopen it again.
    virtual void close();
public:
    virtual bool is_open();
    virtual int64_t tellg();
// Interface ISrsWriter
public:
    virtual srs_error_t write(void* buf, size_t count, ssize_t* pnwrite);
    virtual srs_error_t writev(const iovec* iov, int iovcnt, ssize_t* pnwrite);
};

// The file reader, to read from a file.
class SrsFileReader
{
private:
    std::string path;
    int fd;
public:
    SrsFileReader();
    virtual ~SrsFileReader();
public:
    // open the file reader.
    // @param p a string that indicates the path of the file to open.
    virtual srs_error_t open(std::string p);
    // close the current reader.
    // @remark the user can reopen it again.
    virtual void close();
public:
    virtual bool is_open();
    virtual int64_t filesize();
    virtual srs_error_t read(void* buf, size_t count, ssize_t* pnread);
};

// Whether path exists. (원본: kernel/srs_kernel_utility.cpp srs_path_exists)
extern bool srs_path_exists(std::string path);
// Create dir recursively. (원본: srs_create_dir_recursively)
extern srs_error_t srs_create_dir_recursively(std::string dir);

#endif
