// srs_simple — 원본: trunk/src/kernel/srs_kernel_file.cpp
//            + kernel/srs_kernel_utility.cpp의 srs_path_exists/srs_create_dir_recursively
#include <srs_kernel_file.hpp>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <srs_kernel_error.hpp>

using namespace std;

SrsFileWriter::SrsFileWriter()
{
    fp_ = NULL;
}

SrsFileWriter::~SrsFileWriter()
{
    close();
}

srs_error_t SrsFileWriter::open(string p)
{
    srs_error_t err = srs_success;

    if (fp_) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "file %s already opened", path_.c_str());
    }

    if ((fp_ = ::fopen(p.c_str(), "wb")) == NULL) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "open file %s failed", p.c_str());
    }

    path_ = p;

    return err;
}

void SrsFileWriter::close()
{
    if (fp_) {
        ::fclose(fp_);
        fp_ = NULL;
    }
}

bool SrsFileWriter::is_open()
{
    return fp_ != NULL;
}

int64_t SrsFileWriter::tellg()
{
    if (!fp_) {
        return -1;
    }
    return (int64_t)::ftell(fp_);
}

srs_error_t SrsFileWriter::write(void* buf, size_t count, ssize_t* pnwrite)
{
    srs_error_t err = srs_success;

    if (!fp_) {
        return srs_error_new(ERROR_SYSTEM_FILE_WRITE, "file %s is not opened", path_.c_str());
    }

    size_t n = ::fwrite(buf, 1, count, fp_);
    if (n != count) {
        return srs_error_new(ERROR_SYSTEM_FILE_WRITE, "write to file %s failed", path_.c_str());
    }

    if (pnwrite) {
        *pnwrite = (ssize_t)n;
    }

    return err;
}

srs_error_t SrsFileWriter::writev(const iovec* iov, int iovcnt, ssize_t* pnwrite)
{
    srs_error_t err = srs_success;

    ssize_t nwrite = 0;
    for (int i = 0; i < iovcnt; i++) {
        const iovec* piov = iov + i;
        ssize_t this_nwrite = 0;
        if ((err = write(piov->iov_base, piov->iov_len, &this_nwrite)) != srs_success) {
            return srs_error_wrap(err, "writev");
        }
        nwrite += this_nwrite;
    }

    if (pnwrite) {
        *pnwrite = nwrite;
    }

    return err;
}

SrsFileReader::SrsFileReader()
{
    fd = -1;
}

SrsFileReader::~SrsFileReader()
{
    close();
}

srs_error_t SrsFileReader::open(string p)
{
    srs_error_t err = srs_success;

    if (fd > 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "file %s already opened", path.c_str());
    }

    if ((fd = ::open(p.c_str(), O_RDONLY)) < 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "open file %s failed", p.c_str());
    }

    path = p;

    return err;
}

void SrsFileReader::close()
{
    if (fd > 0) {
        ::close(fd);
        fd = -1;
    }
}

bool SrsFileReader::is_open()
{
    return fd > 0;
}

int64_t SrsFileReader::filesize()
{
    int64_t cur = (int64_t)::lseek(fd, 0, SEEK_CUR);
    int64_t size = (int64_t)::lseek(fd, 0, SEEK_END);
    ::lseek(fd, (off_t)cur, SEEK_SET);
    return size;
}

srs_error_t SrsFileReader::read(void* buf, size_t count, ssize_t* pnread)
{
    srs_error_t err = srs_success;

    ssize_t nread = ::read(fd, buf, count);
    if (nread < 0) {
        return srs_error_new(ERROR_SYSTEM_FILE_OPENE, "read from file %s failed", path.c_str());
    }

    if (pnread) {
        *pnread = nread;
    }

    return err;
}

bool srs_path_exists(std::string path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

srs_error_t srs_create_dir_recursively(string dir)
{
    srs_error_t err = srs_success;

    if (dir.empty() || dir == "/" || dir == ".") {
        return err;
    }

    // 부모 디렉터리부터 재귀 생성 (원본과 동일한 자기호출 구조).
    size_t pos = dir.rfind('/');
    if (pos != string::npos && pos != 0) {
        if ((err = srs_create_dir_recursively(dir.substr(0, pos))) != srs_success) {
            return srs_error_wrap(err, "create parent dir");
        }
    }

    if (::mkdir(dir.c_str(), 0755) < 0 && errno != EEXIST) {
        return srs_error_new(ERROR_HLS_CREATE_DIR, "create dir %s failed", dir.c_str());
    }

    return err;
}
