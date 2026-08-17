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
    // Get the length of buffer. empty if zero.
    // @remark assert length() is not negative.
    virtual int length();
    // Get the buffer bytes.
    // @return the bytes, NULL if empty.
    virtual char* bytes();
    // Erase size of bytes from begin.
    // @param size to erase size of bytes. clear if size greater than or equals to length()
    // @remark ignore size is not positive.
    virtual void erase(int size);
    // Append specified bytes to buffer.
    // @param size the size of bytes
    // @remark assert size is positive.
    virtual void append(const char* bytes, int size);
    virtual void append(SrsSimpleStream* src);
};

#endif
