// srs_simple — 원본: trunk/src/kernel/srs_kernel_stream.hpp
// utest의 MockBufferIO가 소켓 흉내용 in/out 버퍼로 사용한다.
#ifndef SRS_KERNEL_STREAM_HPP
#define SRS_KERNEL_STREAM_HPP

#include <srs_core.hpp>

#include <vector>

/**
 * The simple data buffer, append-only with erase from head.
 */
class SrsSimpleStream
{
private:
    std::vector<char> data;
public:
    SrsSimpleStream();
    virtual ~SrsSimpleStream();
public:
    // Get the length of the buffer. empty if zero.
    // @remark assert length() is not negative.
    virtual int length();
    // Get the buffer bytes.
    // @return the bytes, NULL if empty.
    virtual char* bytes();
    // Erase size bytes from the beginning.
    // @param size the number of bytes to erase. clear if size is greater than or equal to length()
    // @remark ignored if size is not positive.
    virtual void erase(int size);
    // Append the specified bytes to the buffer.
    // @param size the size in bytes
    // @remark assert size is positive.
    virtual void append(const char* bytes, int size);
    virtual void append(SrsSimpleStream* src);
};

#endif
