// srs_simple — 원본: trunk/src/protocol/srs_protocol_stream.cpp
#include <srs_protocol_stream.hpp>

#include <stdlib.h>
#include <string.h>

#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

// the default recv buffer size, 128KB.
#define SRS_DEFAULT_RECV_BUFFER_SIZE 131072

SrsFastStream::SrsFastStream(int size)
{
    nb_buffer = size ? size : SRS_DEFAULT_RECV_BUFFER_SIZE;
    buffer = (char*)malloc(nb_buffer);
    p = end = buffer;
}

SrsFastStream::~SrsFastStream()
{
    free(buffer);
    buffer = NULL;
}

int SrsFastStream::size()
{
    return (int)(end - p);
}

char* SrsFastStream::bytes()
{
    return p;
}

char SrsFastStream::read_1byte()
{
    srs_assert(end - p >= 1);
    return *p++;
}

char* SrsFastStream::read_slice(int size)
{
    srs_assert(size >= 0);
    srs_assert(end - p >= size);
    srs_assert(p + size >= buffer);

    char* ptr = p;
    p += size;

    return ptr;
}

void SrsFastStream::skip(int size)
{
    srs_assert(end - p >= size);
    srs_assert(p + size >= buffer);
    p += size;
}

srs_error_t SrsFastStream::grow(ISrsReader* reader, int required_size)
{
    srs_error_t err = srs_success;

    // already got required size of bytes.
    if (end - p >= required_size) {
        return err;
    }

    // must be positive.
    srs_assert(required_size > 0);

    // the free space of buffer,
    //      buffer = consumed_bytes + exists_bytes + free_space.
    int nb_free_space = (int)(buffer + nb_buffer - end);

    // the bytes already in buffer
    int nb_exists_bytes = (int)(end - p);
    srs_assert(nb_exists_bytes >= 0);

    // resize the space when no left space.
    if (nb_exists_bytes + nb_free_space < required_size) {
        // reset or move to get more space.
        if (!nb_exists_bytes) {
            // reset when buffer is empty.
            p = end = buffer;
        } else if (nb_exists_bytes < nb_buffer && p > buffer) {
            // move the left bytes to start of buffer.
            // @remark Only move memory when space is enough, or failed at next check.
            // @see https://github.com/ossrs/srs/issues/848
            buffer = (char*)memmove(buffer, p, nb_exists_bytes);
            p = buffer;
            end = p + nb_exists_bytes;
        }

        // check whether enough free space in buffer.
        nb_free_space = (int)(buffer + nb_buffer - end);
        if (nb_exists_bytes + nb_free_space < required_size) {
            return srs_error_new(ERROR_READER_BUFFER_OVERFLOW, "overflow, required=%d, max=%d, left=%d",
                required_size, nb_buffer, nb_free_space);
        }
    }

    // buffer is ok, read required size of bytes.
    while (end - p < required_size) {
        ssize_t nread;
        if ((err = reader->read(end, nb_free_space, &nread)) != srs_success) {
            return srs_error_wrap(err, "read bytes");
        }

        // we just move the ptr to next.
        srs_assert((int)nread > 0);
        end += nread;
        nb_free_space -= (int)nread;
    }

    return err;
}
