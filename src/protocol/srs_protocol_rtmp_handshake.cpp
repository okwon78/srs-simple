// srs_simple — 원본: trunk/src/protocol/srs_protocol_rtmp_handshake.cpp:1071 (심플 핸드셰이크)
//              + trunk/src/protocol/srs_protocol_rtmp_stack.cpp:1588-1750 (SrsHandshakeBytes)
#include <srs_protocol_rtmp_handshake.hpp>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <srs_kernel_buffer.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>
#include <srs_protocol_io.hpp>

// 원본: kernel/srs_kernel_utility.cpp srs_random_generate.
// C1/S1의 random(1528바이트) 채우기용 — 암호학적 품질 불필요 (심플 핸드셰이크는 검증하지 않음).
static void srs_random_generate(char *bytes, int size)
{
    for (int i = 0; i < size; i++)
    {
        // the common value in [0x0f, 0xf0]
        bytes[i] = 0x0f + (random() % (256 - 0x0f - 0x0f));
    }
}

SrsHandshakeBytes::SrsHandshakeBytes()
{
    c0c1 = s0s1s2 = c2 = NULL;
}

SrsHandshakeBytes::~SrsHandshakeBytes()
{
    dispose();
}

void SrsHandshakeBytes::dispose()
{
    srs_freepa(c0c1);
    srs_freepa(s0s1s2);
    srs_freepa(c2);
}

srs_error_t SrsHandshakeBytes::read_c0c1(ISrsProtocolReader *io)
{
    srs_error_t err = srs_success;

    if (c0c1)
    {
        return err;
    }

    ssize_t nsize;

    c0c1 = new char[1537];
    if ((err = io->read_fully(c0c1, 1537, &nsize)) != srs_success)
    {
        return srs_error_wrap(err, "read c0c1");
    }

    return err;
}

srs_error_t SrsHandshakeBytes::read_c2(ISrsProtocolReader *io)
{
    srs_error_t err = srs_success;

    if (c2)
    {
        return err;
    }

    ssize_t nsize;

    c2 = new char[1536];
    if ((err = io->read_fully(c2, 1536, &nsize)) != srs_success)
    {
        return srs_error_wrap(err, "read c2");
    }

    return err;
}

srs_error_t SrsHandshakeBytes::create_s0s1s2(const char *c1)
{
    srs_error_t err = srs_success;

    if (s0s1s2)
    {
        return err;
    }

    s0s1s2 = new char[3073];
    srs_random_generate(s0s1s2, 3073);

    // plain text required.
    SrsBuffer stream(s0s1s2, 9);

    stream.write_1bytes(0x03);
    stream.write_4bytes((int32_t)::time(NULL));
    // s1 time2 copy from c1
    if (c0c1)
    {
        stream.write_bytes(c0c1 + 1, 4);
    }

    // if c1 is specified, copy c1 to s2.
    // @see: https://github.com/ossrs/srs/issues/46
    if (c1)
    {
        memcpy(s0s1s2 + 1537, c1, 1536);
    }

    return err;
}

SrsSimpleHandshake::SrsSimpleHandshake()
{
}

SrsSimpleHandshake::~SrsSimpleHandshake()
{
}

// io: Socket
srs_error_t SrsSimpleHandshake::handshake_with_client(SrsHandshakeBytes *hs_bytes, ISrsProtocolReadWriter *io)
{
    srs_error_t err = srs_success;

    ssize_t nsize;

    if ((err = hs_bytes->read_c0c1(io)) != srs_success)
    {
        return srs_error_wrap(err, "read c0c1");
    }

    // plain text required.
    if (hs_bytes->c0c1[0] != 0x03)
    {
        return srs_error_new(ERROR_RTMP_PLAIN_REQUIRED, "only support rtmp plain text, version=%X", (uint8_t)hs_bytes->c0c1[0]);
    }

    // 심플 핸드셰이크의 전부: S1은 새 랜덤, S2는 받은 C1의 복사.
    if ((err = hs_bytes->create_s0s1s2(hs_bytes->c0c1 + 1)) != srs_success)
    {
        return srs_error_wrap(err, "create s0s1s2");
    }

    if ((err = io->write(hs_bytes->s0s1s2, 3073, &nsize)) != srs_success)
    {
        return srs_error_wrap(err, "write s0s1s2");
    }

    // C2는 읽고 폐기 — SRS도 검증하지 않는다 (ffmpeg 호환).
    if ((err = hs_bytes->read_c2(io)) != srs_success)
    {
        return srs_error_wrap(err, "read c2");
    }

    srs_trace("simple handshake success.");

    return err;
}
