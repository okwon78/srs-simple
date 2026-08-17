// srs_simple — 원본: trunk/src/utest/srs_utest_protocol.cpp (MockBufferIO 구현 + FastStream 테스트)
#include <srs_utest_protocol.hpp>

#include <string.h>

#include <srs_kernel_buffer.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_protocol_amf0.hpp>
#include <srs_protocol_rtmp_handshake.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_protocol_utility.hpp>

MockBufferIO::MockBufferIO()
{
    rtm = stm = SRS_UTIME_NO_TIMEOUT;
    rbytes = sbytes = 0;
    in_err = out_err = srs_success;
}

MockBufferIO::~MockBufferIO()
{
    srs_freep(in_err);
    srs_freep(out_err);
}

int MockBufferIO::length()
{
    return in_buffer.length();
}

MockBufferIO* MockBufferIO::append(string data)
{
    in_buffer.append(data.data(), (int)data.length());
    return this;
}

MockBufferIO* MockBufferIO::append(uint8_t* data, int size)
{
    in_buffer.append((char*)data, size);
    return this;
}

int MockBufferIO::out_length()
{
    return out_buffer.length();
}

srs_error_t MockBufferIO::read_fully(void* buf, size_t size, ssize_t* nread)
{
    if (in_err != srs_success) {
        return srs_error_copy(in_err);
    }

    if (in_buffer.length() < (int)size) {
        return srs_error_new(ERROR_SOCKET_READ, "read");
    }
    memcpy(buf, in_buffer.bytes(), size);

    rbytes += size;
    if (nread) {
        *nread = size;
    }
    in_buffer.erase((int)size);
    return srs_success;
}

srs_error_t MockBufferIO::write(void* buf, size_t size, ssize_t* nwrite)
{
    if (out_err != srs_success) {
        return srs_error_copy(out_err);
    }

    sbytes += size;
    if (nwrite) {
        *nwrite = size;
    }
    out_buffer.append((char*)buf, (int)size);
    return srs_success;
}

void MockBufferIO::set_recv_timeout(srs_utime_t tm)
{
    rtm = tm;
}

srs_utime_t MockBufferIO::get_recv_timeout()
{
    return rtm;
}

int64_t MockBufferIO::get_recv_bytes()
{
    return rbytes;
}

void MockBufferIO::set_send_timeout(srs_utime_t tm)
{
    stm = tm;
}

srs_utime_t MockBufferIO::get_send_timeout()
{
    return stm;
}

int64_t MockBufferIO::get_send_bytes()
{
    return sbytes;
}

srs_error_t MockBufferIO::writev(const iovec* iov, int iov_size, ssize_t* nwrite)
{
    srs_error_t err = srs_success;

    if (out_err != srs_success) {
        return srs_error_copy(out_err);
    }

    ssize_t total = 0;
    for (int i = 0; i < iov_size; i++) {
        const iovec& pi = iov[i];

        ssize_t writen = 0;
        if ((err = write(pi.iov_base, pi.iov_len, &writen)) != srs_success) {
            return err;
        }
        total += writen;
    }

    if (nwrite) {
        *nwrite = total;
    }
    return err;
}

srs_error_t MockBufferIO::read(void* buf, size_t size, ssize_t* nread)
{
    if (in_err != srs_success) {
        return srs_error_copy(in_err);
    }

    if (in_buffer.length() <= 0) {
        return srs_error_new(ERROR_SOCKET_READ, "read");
    }

    int available = in_buffer.length() < (int)size ? in_buffer.length() : (int)size;
    memcpy(buf, in_buffer.bytes(), available);

    rbytes += available;
    if (nread) {
        *nread = available;
    }
    in_buffer.erase(available);

    return srs_success;
}

VOID TEST(ProtocolStreamTest, SimpleStream)
{
    SrsSimpleStream s;
    EXPECT_EQ(0, s.length());
    EXPECT_TRUE(s.bytes() == NULL);

    s.append("Hello", 5);
    s.append(", world!", 8);
    EXPECT_EQ(13, s.length());
    EXPECT_EQ(0, memcmp(s.bytes(), "Hello, world!", 13));

    s.erase(7);
    EXPECT_EQ(6, s.length());
    EXPECT_EQ(0, memcmp(s.bytes(), "world!", 6));

    // ignore non-positive, clear if greater than length.
    s.erase(0);
    EXPECT_EQ(6, s.length());
    s.erase(100);
    EXPECT_EQ(0, s.length());
}

VOID TEST(ProtocolStreamTest, MockBufferIO)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    io.append("Hello");

    char buf[16];
    HELPER_EXPECT_SUCCESS(io.read_fully(buf, 5, NULL));
    EXPECT_EQ(0, memcmp(buf, "Hello", 5));
    EXPECT_EQ(5, (int)io.get_recv_bytes());

    // read_fully fails if not enough bytes.
    io.append("Hi");
    HELPER_EXPECT_FAILED(io.read_fully(buf, 5, NULL));

    HELPER_EXPECT_SUCCESS(io.write((void*)"World", 5, NULL));
    EXPECT_EQ(5, io.out_length());
    EXPECT_EQ(0, memcmp(io.out_buffer.bytes(), "World", 5));

    iovec iovs[2];
    iovs[0].iov_base = (void*)"AB";
    iovs[0].iov_len = 2;
    iovs[1].iov_base = (void*)"CD";
    iovs[1].iov_len = 2;
    ssize_t nn = 0;
    HELPER_EXPECT_SUCCESS(io.writev(iovs, 2, &nn));
    EXPECT_EQ(4, (int)nn);
    EXPECT_EQ(9, io.out_length());
    EXPECT_EQ(0, memcmp(io.out_buffer.bytes(), "WorldABCD", 9));
}

// 완료 조건: MockBufferIO로 FastStream grow/slice 테스트 통과.
VOID TEST(ProtocolStreamTest, FastStreamGrowAndSlice)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    io.append("Hello, world!");

    SrsFastStream fs;
    EXPECT_EQ(0, fs.size());

    // grow(5)지만 reader에 있는 만큼(13) 다 읽어들인다.
    HELPER_EXPECT_SUCCESS(fs.grow(&io, 5));
    EXPECT_EQ(13, fs.size());
    EXPECT_EQ(0, memcmp(fs.bytes(), "Hello, world!", 13));

    // read_slice는 내부 버퍼 포인터를 그대로 돌려준다 (제로카피).
    char* p = fs.read_slice(5);
    EXPECT_EQ(0, memcmp(p, "Hello", 5));
    EXPECT_EQ(8, fs.size());

    EXPECT_EQ(',', fs.read_1byte());
    fs.skip(1);
    EXPECT_EQ(6, fs.size());

    // 이미 충분하면 reader를 건드리지 않는다.
    HELPER_EXPECT_SUCCESS(fs.grow(&io, 6));
    EXPECT_EQ(0, memcmp(fs.read_slice(6), "world!", 6));

    // skip은 음수로 되돌릴 수도 있다.
    fs.skip(-6);
    EXPECT_EQ(0, memcmp(fs.read_slice(6), "world!", 6));
}

VOID TEST(ProtocolStreamTest, FastStreamGrowEOF)
{
    srs_error_t err = srs_success;

    // reader가 빈 경우 grow는 read 에러를 래핑해 돌려준다.
    MockBufferIO io;
    SrsFastStream fs;
    HELPER_EXPECT_FAILED(fs.grow(&io, 1));

    // 주입된 에러가 그대로 전파된다.
    MockBufferIO io2;
    io2.in_err = srs_error_new(ERROR_SOCKET_TIMEOUT, "mock timeout");
    err = fs.grow(&io2, 1);
    EXPECT_EQ(ERROR_SOCKET_TIMEOUT, srs_error_code(err));
    srs_freep(err);
}

VOID TEST(ProtocolStreamTest, FastStreamGrowOverflow)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    io.append("0123456789");

    // 요구 크기가 버퍼 자체보다 크면 ERROR_READER_BUFFER_OVERFLOW.
    SrsFastStream fs(8);
    err = fs.grow(&io, 9);
    EXPECT_EQ(ERROR_READER_BUFFER_OVERFLOW, srs_error_code(err));
    srs_freep(err);

    HELPER_EXPECT_SUCCESS(fs.grow(&io, 8));
    EXPECT_EQ(0, memcmp(fs.read_slice(8), "01234567", 8));
}

VOID TEST(ProtocolStreamTest, FastStreamGrowMemmove)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    io.append("0123456789");

    // 8바이트 버퍼: 8을 읽고 6을 소비하면 남은 2바이트("67")가 버퍼 끝에 있다.
    SrsFastStream fs(8);
    HELPER_EXPECT_SUCCESS(fs.grow(&io, 8));
    fs.read_slice(6);
    EXPECT_EQ(2, fs.size());

    // grow(4)는 남은 2바이트를 버퍼 앞으로 옮기고(memmove) 이어서 읽는다.
    HELPER_EXPECT_SUCCESS(fs.grow(&io, 4));
    EXPECT_EQ(0, memcmp(fs.read_slice(4), "6789", 4));
}

// S3 — 심플 핸드셰이크 (srs_protocol_rtmp_handshake)

// 완료 조건: 1537바이트(C0C1) + 1536바이트(C2) 주입 → 3073바이트(S0S1S2) 응답.
VOID TEST(ProtocolHandshakeTest, SimpleHandshakeWithClient)
{
    srs_error_t err = srs_success;

    MockBufferIO io;

    // C0 = 0x03(plain), C1 = time(4)|version(4)|random(1528).
    uint8_t c0c1[1537];
    c0c1[0] = 0x03;
    for (int i = 1; i < 1537; i++) {
        c0c1[i] = (uint8_t)(i % 256);
    }
    io.append(c0c1, 1537);

    // C2 — 서버는 읽고 폐기한다.
    uint8_t c2[1536];
    HELPER_ARRAY_INIT(c2, 1536, 0x05);
    io.append(c2, 1536);

    SrsHandshakeBytes hs_bytes;
    SrsSimpleHandshake hs;
    HELPER_ASSERT_SUCCESS(hs.handshake_with_client(&hs_bytes, &io));

    // S0(1) + S1(1536) + S2(1536) = 3073바이트 응답.
    ASSERT_EQ(3073, io.out_length());
    char* out = io.out_buffer.bytes();
    // S0 = 0x03.
    EXPECT_EQ(0x03, out[0]);
    // S2 = 받은 C1의 1536바이트 복사.
    EXPECT_EQ(0, memcmp(out + 1537, c0c1 + 1, 1536));

    // C0C1과 C2를 모두 소비했다.
    EXPECT_EQ(0, io.length());
    EXPECT_EQ(0, memcmp(hs_bytes.c2, c2, 1536));
}

VOID TEST(ProtocolHandshakeTest, PlainTextRequired)
{
    srs_error_t err = srs_success;

    MockBufferIO io;

    // C0가 0x03이 아니면(예: 복잡 핸드셰이크/RTMPS 시도) 거부.
    uint8_t c0c1[1537];
    HELPER_ARRAY_INIT(c0c1, 1537, 0x00);
    c0c1[0] = 0x06;
    io.append(c0c1, 1537);

    SrsHandshakeBytes hs_bytes;
    SrsSimpleHandshake hs;
    err = hs.handshake_with_client(&hs_bytes, &io);
    EXPECT_EQ(ERROR_RTMP_PLAIN_REQUIRED, srs_error_code(err));
    srs_freep(err);

    // 응답을 보내지 않는다.
    EXPECT_EQ(0, io.out_length());
}

VOID TEST(ProtocolHandshakeTest, HandshakeIOFailures)
{
    srs_error_t err = srs_success;

    // C0C1이 모자라면 read 에러가 전파된다.
    if (true) {
        MockBufferIO io;
        uint8_t partial[100];
        HELPER_ARRAY_INIT(partial, 100, 0x03);
        io.append(partial, 100);

        SrsHandshakeBytes hs_bytes;
        SrsSimpleHandshake hs;
        HELPER_EXPECT_FAILED(hs.handshake_with_client(&hs_bytes, &io));
    }

    // C0C1만 오고 C2가 없으면 실패하되, S0S1S2는 이미 나갔다.
    if (true) {
        MockBufferIO io;
        uint8_t c0c1[1537];
        HELPER_ARRAY_INIT(c0c1, 1537, 0x00);
        c0c1[0] = 0x03;
        io.append(c0c1, 1537);

        SrsHandshakeBytes hs_bytes;
        SrsSimpleHandshake hs;
        HELPER_EXPECT_FAILED(hs.handshake_with_client(&hs_bytes, &io));
        EXPECT_EQ(3073, io.out_length());
    }
}

// S5 — 청크 스트림 스택 (srs_protocol_rtmp_stack)

// fmt=0 단일 청크: 오디오 메시지 하나가 그대로 파싱된다.
VOID TEST(ProtocolStackTest, RecvSingleChunkMessage)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    uint8_t data[] = {
        0x04,                                     // fmt=0, cid=4
        0x00, 0x00, 0x1a,                         // timestamp=26
        0x00, 0x00, 0x10,                         // payload_length=16
        0x08,                                     // message_type=8(audio)
        0x01, 0x00, 0x00, 0x00,                   // stream_id=1 (little-endian)
        // payload 16B
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    io.append(data, sizeof(data));

    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(8, msg->header.message_type);
    EXPECT_EQ(16, msg->size);
    EXPECT_EQ(26, (int)msg->header.timestamp);
    EXPECT_EQ(1, msg->header.stream_id);
    EXPECT_EQ(4, msg->header.prefer_cid);
    EXPECT_EQ(0, memcmp(msg->payload, data + 12, 16));
    srs_freep(msg);
}

// in_chunk_size(128) 단위 분할: 200바이트 메시지 = 128B 청크 + 0xC4 연속 청크 72B로 재조립.
VOID TEST(ProtocolStackTest, RecvSplitChunksReassembly)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    uint8_t header[] = {
        0x04,                                     // fmt=0, cid=4
        0x00, 0x00, 0x1a,                         // timestamp=26
        0x00, 0x00, 0xc8,                         // payload_length=200
        0x09,                                     // message_type=9(video)
        0x01, 0x00, 0x00, 0x00,                   // stream_id=1
    };
    io.append(header, sizeof(header));

    uint8_t payload[200];
    for (int i = 0; i < 200; i++) {
        payload[i] = (uint8_t)i;
    }
    // 첫 청크는 in_chunk_size(128)까지만.
    io.append(payload, 128);

    // 연속 청크: fmt=3, cid=4.
    uint8_t c3 = 0xC4;
    io.append(&c3, 1);
    io.append(payload + 128, 72);

    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(9, msg->header.message_type);
    EXPECT_EQ(200, msg->size);
    EXPECT_EQ(200, msg->header.payload_length);
    EXPECT_EQ(0, memcmp(msg->payload, payload, 200));
    srs_freep(msg);
}

// FMLE의 0xC4 신규 메시지: fmt=3이라도 새 메시지면 delta(=이전 ts 필드값)를 누적한다.
VOID TEST(ProtocolStackTest, RecvFmt3FreshMessageAppliesDelta)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    uint8_t first[] = {
        0x04,                                     // fmt=0, cid=4
        0x00, 0x00, 0x1a,                         // timestamp=26
        0x00, 0x00, 0x02,                         // payload_length=2
        0x08,                                     // audio
        0x01, 0x00, 0x00, 0x00,                   // stream_id=1
        0xaa, 0xbb,
    };
    io.append(first, sizeof(first));

    // 두 번째 메시지: 0xC4 (fmt=3, 헤더 전부 상속) — timestamp = 26+26 = 52.
    uint8_t second[] = { 0xC4, 0xcc, 0xdd };
    io.append(second, sizeof(second));

    // 세 번째 메시지도 동일 — timestamp = 52+26 = 78.
    uint8_t third[] = { 0xC4, 0xee, 0xff };
    io.append(third, sizeof(third));

    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(26, (int)msg->header.timestamp);
    srs_freep(msg);

    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(52, (int)msg->header.timestamp);
    EXPECT_EQ(2, msg->size);
    EXPECT_EQ(8, msg->header.message_type);
    srs_freep(msg);

    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(78, (int)msg->header.timestamp);
    srs_freep(msg);
}

// fmt=1(delta+len+type)과 fmt=2(delta만) 파싱.
VOID TEST(ProtocolStackTest, RecvFmt1Fmt2Headers)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    uint8_t first[] = {
        0x04,                                     // fmt=0, cid=4
        0x00, 0x00, 0x64,                         // timestamp=100
        0x00, 0x00, 0x04,                         // payload_length=4
        0x09,                                     // video
        0x01, 0x00, 0x00, 0x00,                   // stream_id=1
        0x01, 0x02, 0x03, 0x04,
    };
    io.append(first, sizeof(first));

    // fmt=1: delta=10, len=4, type=9 — timestamp = 100+10 = 110.
    uint8_t second[] = {
        0x44,                                     // fmt=1, cid=4
        0x00, 0x00, 0x0a,                         // delta=10
        0x00, 0x00, 0x04,                         // payload_length=4
        0x09,                                     // video
        0x05, 0x06, 0x07, 0x08,
    };
    io.append(second, sizeof(second));

    // fmt=2: delta=5만 — timestamp = 110+5 = 115.
    uint8_t third[] = {
        0x84,                                     // fmt=2, cid=4
        0x00, 0x00, 0x05,                         // delta=5
        0x09, 0x0a, 0x0b, 0x0c,
    };
    io.append(third, sizeof(third));

    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    EXPECT_EQ(100, (int)msg->header.timestamp);
    srs_freep(msg);

    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    EXPECT_EQ(110, (int)msg->header.timestamp);
    EXPECT_EQ(9, msg->header.message_type);
    srs_freep(msg);

    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    EXPECT_EQ(115, (int)msg->header.timestamp);
    EXPECT_EQ(4, msg->size);
    srs_freep(msg);
}

// extended timestamp: ts 필드가 0xFFFFFF면 4바이트 확장 타임스탬프가 뒤따른다.
VOID TEST(ProtocolStackTest, RecvExtendedTimestamp)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    // timestamp = 0x01234567 (>= 0xFFFFFF).
    uint8_t data[] = {
        0x04,                                     // fmt=0, cid=4
        0xff, 0xff, 0xff,                         // marker
        0x00, 0x00, 0x02,                         // payload_length=2
        0x08,                                     // audio
        0x01, 0x00, 0x00, 0x00,                   // stream_id=1
        0x01, 0x23, 0x45, 0x67,                   // extended timestamp
        0xaa, 0xbb,
    };
    io.append(data, sizeof(data));

    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(0x01234567, (int)msg->header.timestamp);
    EXPECT_EQ(2, msg->size);
    srs_freep(msg);
}

// adobe 스타일: 연속 청크(0xC3)에도 extended timestamp를 반복 전송 — 값이 같으면 소비한다.
VOID TEST(ProtocolStackTest, RecvExtendedTimestampRepeatedInC3)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    uint8_t header[] = {
        0x04,                                     // fmt=0, cid=4
        0xff, 0xff, 0xff,                         // marker
        0x00, 0x00, 0x82,                         // payload_length=130
        0x09,                                     // video
        0x01, 0x00, 0x00, 0x00,                   // stream_id=1
        0x01, 0x23, 0x45, 0x67,                   // extended timestamp
    };
    io.append(header, sizeof(header));

    uint8_t payload[130];
    for (int i = 0; i < 130; i++) {
        payload[i] = (uint8_t)(i + 1);
    }
    io.append(payload, 128);

    // 연속 청크가 extended timestamp를 다시 보낸다 (srs/adobe 스타일).
    uint8_t c3[] = { 0xC4, 0x01, 0x23, 0x45, 0x67 };
    io.append(c3, sizeof(c3));
    io.append(payload + 128, 2);

    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(0x01234567, (int)msg->header.timestamp);
    EXPECT_EQ(130, msg->size);
    EXPECT_EQ(0, memcmp(msg->payload, payload, 130));
    srs_freep(msg);
}

// ffmpeg/librtmp 스타일: 연속 청크가 extended timestamp를 반복하지 않는다 —
// 4바이트를 읽어보고 값이 다르면 페이로드로 판단해 되돌린다(read_message_header의 감지 로직).
VOID TEST(ProtocolStackTest, RecvExtendedTimestampNotRepeatedInC3)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    uint8_t header[] = {
        0x04,                                     // fmt=0, cid=4
        0xff, 0xff, 0xff,                         // marker
        0x00, 0x00, 0x82,                         // payload_length=130
        0x09,                                     // video
        0x01, 0x00, 0x00, 0x00,                   // stream_id=1
        0x01, 0x23, 0x45, 0x67,                   // extended timestamp
    };
    io.append(header, sizeof(header));

    uint8_t payload[130];
    for (int i = 0; i < 130; i++) {
        payload[i] = (uint8_t)(i + 1);
    }
    io.append(payload, 128);

    // 연속 청크에 extended timestamp 없음 — 바로 페이로드.
    uint8_t c3 = 0xC4;
    io.append(&c3, 1);
    io.append(payload + 128, 2);

    // 감지 로직은 4바이트를 미리 읽어 비교하므로, 실제 라이브 스트림처럼
    // 뒤따르는 메시지가 있어야 한다 (여기서 2바이트를 빌려 본 뒤 skip(-4)로 되돌림).
    uint8_t next[] = {
        0x05,                                     // fmt=0, cid=5
        0x00, 0x00, 0x0a,                         // timestamp=10
        0x00, 0x00, 0x02,                         // payload_length=2
        0x08,                                     // audio
        0x01, 0x00, 0x00, 0x00,
        0xee, 0xff,
    };
    io.append(next, sizeof(next));

    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(0x01234567, (int)msg->header.timestamp);
    EXPECT_EQ(130, msg->size);
    EXPECT_EQ(0, memcmp(msg->payload, payload, 130));
    srs_freep(msg);

    // 되돌린 4바이트 중 후속 메시지 부분이 온전히 파싱된다.
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(8, msg->header.message_type);
    EXPECT_EQ(10, (int)msg->header.timestamp);
    EXPECT_EQ(2, msg->size);
    srs_freep(msg);
}

// 청크 인터리빙: 두 csid의 청크가 교차해도 각 메시지가 올바르게 조립된다.
VOID TEST(ProtocolStackTest, RecvInterlacedChunkStreams)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    uint8_t video[200];
    for (int i = 0; i < 200; i++) {
        video[i] = (uint8_t)i;
    }

    // cid=6 비디오 200B: 첫 128B만 먼저.
    uint8_t vheader[] = {
        0x06,                                     // fmt=0, cid=6
        0x00, 0x00, 0x14,                         // timestamp=20
        0x00, 0x00, 0xc8,                         // payload_length=200
        0x09,                                     // video
        0x01, 0x00, 0x00, 0x00,
    };
    io.append(vheader, sizeof(vheader));
    io.append(video, 128);

    // 사이에 cid=7 오디오 4B가 끼어든다.
    uint8_t audio[] = {
        0x07,                                     // fmt=0, cid=7
        0x00, 0x00, 0x15,                         // timestamp=21
        0x00, 0x00, 0x04,                         // payload_length=4
        0x08,                                     // audio
        0x01, 0x00, 0x00, 0x00,
        0xd1, 0xd2, 0xd3, 0xd4,
    };
    io.append(audio, sizeof(audio));

    // cid=6의 나머지 72B.
    uint8_t c3 = 0xC6;
    io.append(&c3, 1);
    io.append(video + 128, 72);

    // 오디오가 먼저 완성된다.
    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(8, msg->header.message_type);
    EXPECT_EQ(4, msg->size);
    srs_freep(msg);

    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(9, msg->header.message_type);
    EXPECT_EQ(200, msg->size);
    EXPECT_EQ(0, memcmp(msg->payload, video, 200));
    srs_freep(msg);
}

// 2바이트/3바이트 basic header: cid 0/1 마커로 큰 csid를 인코딩한다.
VOID TEST(ProtocolStackTest, RecvLargeCidBasicHeader)
{
    srs_error_t err = srs_success;

    // 2바이트 형식: cid = 64 + b1 = 64 + 100 = 164.
    if (true) {
        MockBufferIO io;
        SrsProtocol proto(&io);

        uint8_t data[] = {
            0x00, 0x64,                           // fmt=0, cid=164
            0x00, 0x00, 0x0a,
            0x00, 0x00, 0x02,
            0x08,
            0x01, 0x00, 0x00, 0x00,
            0xaa, 0xbb,
        };
        io.append(data, sizeof(data));

        SrsCommonMessage* msg = NULL;
        HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
        ASSERT_TRUE(msg != NULL);
        EXPECT_EQ(164, msg->header.prefer_cid);
        srs_freep(msg);
    }

    // 3바이트 형식: cid = 64 + b1 + b2*256 = 64 + 10 + 2*256 = 586.
    if (true) {
        MockBufferIO io;
        SrsProtocol proto(&io);

        uint8_t data[] = {
            0x01, 0x0a, 0x02,                     // fmt=0, cid=586
            0x00, 0x00, 0x0a,
            0x00, 0x00, 0x02,
            0x08,
            0x01, 0x00, 0x00, 0x00,
            0xaa, 0xbb,
        };
        io.append(data, sizeof(data));

        SrsCommonMessage* msg = NULL;
        HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
        ASSERT_TRUE(msg != NULL);
        EXPECT_EQ(586, msg->header.prefer_cid);
        srs_freep(msg);
    }
}

// 프로토콜 위반 감지: 새 청크 스트림은 fmt=0으로 시작해야 하고(fmt=1은 librtmp 관용으로 허용),
// 조립 중 메시지의 payload_length는 변경할 수 없다.
VOID TEST(ProtocolStackTest, RecvProtocolErrors)
{
    srs_error_t err = srs_success;

    // 신규 청크 스트림이 fmt=3로 시작 → ERROR_RTMP_CHUNK_START.
    if (true) {
        MockBufferIO io;
        SrsProtocol proto(&io);

        uint8_t data[] = { 0xC4, 0x00, 0x00 };
        io.append(data, sizeof(data));

        SrsCommonMessage* msg = NULL;
        err = proto.recv_message(&msg);
        EXPECT_EQ(ERROR_RTMP_CHUNK_START, srs_error_code(err));
        srs_freep(err);
        EXPECT_TRUE(msg == NULL);
    }

    // librtmp 관용: fmt=1로 시작하는 신규 스트림은 경고만 하고 수용한다.
    if (true) {
        MockBufferIO io;
        SrsProtocol proto(&io);

        uint8_t data[] = {
            0x42,                                 // fmt=1, cid=2
            0x00, 0x00, 0x00,                     // delta=0
            0x00, 0x00, 0x06,                     // payload_length=6
            0x04,                                 // user control
            0x00, 0x06,                           // PingRequest
            0x00, 0x00, 0x0d, 0x0f,               // timestamp
        };
        io.append(data, sizeof(data));

        SrsCommonMessage* msg = NULL;
        HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
        ASSERT_TRUE(msg != NULL);
        EXPECT_EQ(4, msg->header.message_type);
        srs_freep(msg);

        // PingRequest는 자동으로 PingResponse로 에코된다.
        ASSERT_EQ(12 + 6, io.out_length());
        char* out = io.out_buffer.bytes();
        EXPECT_EQ(0x04, out[7]);                  // user control
        EXPECT_EQ(0x00, out[12]);
        EXPECT_EQ(0x07, out[13]);                 // PingResponse
        EXPECT_EQ(0, memcmp(out + 14, data + 10, 4)); // 같은 timestamp 에코
    }

    // 조립 중(fmt=1) payload_length 변경 → ERROR_RTMP_PACKET_SIZE.
    if (true) {
        MockBufferIO io;
        SrsProtocol proto(&io);

        uint8_t header[] = {
            0x04,
            0x00, 0x00, 0x0a,
            0x00, 0x01, 0x2c,                     // payload_length=300 (2청크 필요)
            0x09,
            0x01, 0x00, 0x00, 0x00,
        };
        io.append(header, sizeof(header));
        uint8_t chunk1[128] = {0};
        io.append(chunk1, 128);

        // 청크가 다 오기 전에 fmt=1로 길이 변경 시도.
        uint8_t bad[] = {
            0x44,
            0x00, 0x00, 0x05,
            0x00, 0x00, 0x10,                     // payload_length=16 (변경!)
            0x09,
        };
        io.append(bad, sizeof(bad));

        SrsCommonMessage* msg = NULL;
        err = proto.recv_message(&msg);
        EXPECT_EQ(ERROR_RTMP_PACKET_SIZE, srs_error_code(err));
        srs_freep(err);
    }
}

// 인바운드 SetChunkSize 반영: in_chunk_size가 커지면 큰 청크를 한 번에 수신한다.
VOID TEST(ProtocolStackTest, OnRecvSetChunkSize)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);
    EXPECT_EQ(128, proto.in_chunk_size);

    uint8_t set_chunk[] = {
        0x02,                                     // fmt=0, cid=2 (protocol control)
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x04,
        0x01,                                     // SetChunkSize
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00,                   // chunk_size=256
    };
    io.append(set_chunk, sizeof(set_chunk));

    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    EXPECT_EQ(1, msg->header.message_type);
    srs_freep(msg);
    EXPECT_EQ(256, proto.in_chunk_size);

    // 200바이트 페이로드가 단일 청크로 온다 (128이었다면 에러/분할).
    uint8_t header[] = {
        0x04,
        0x00, 0x00, 0x0a,
        0x00, 0x00, 0xc8,                         // payload_length=200
        0x09,
        0x01, 0x00, 0x00, 0x00,
    };
    io.append(header, sizeof(header));
    uint8_t payload[200];
    for (int i = 0; i < 200; i++) {
        payload[i] = (uint8_t)i;
    }
    io.append(payload, 200);

    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    ASSERT_TRUE(msg != NULL);
    EXPECT_EQ(200, msg->size);
    EXPECT_EQ(0, memcmp(msg->payload, payload, 200));
    srs_freep(msg);

    // 최소값(128) 미만의 SetChunkSize는 거부 (#541).
    uint8_t bad[] = {
        0x02,
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x04,
        0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x02,                   // chunk_size=2
    };
    io.append(bad, sizeof(bad));

    err = proto.recv_message(&msg);
    EXPECT_EQ(ERROR_RTMP_CHUNK_SIZE, srs_error_code(err));
    srs_freep(err);
}

// 인바운드 WindowAckSize 반영 + 수신 절반-윈도우 자동 Acknowledgement.
VOID TEST(ProtocolStackTest, OnRecvWindowAckSizeAndAutoAck)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    uint8_t ack_size[] = {
        0x02,                                     // fmt=0, cid=2
        0x00, 0x00, 0x00,
        0x00, 0x00, 0x04,
        0x05,                                     // WindowAckSize
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x10,                   // window=16
    };
    io.append(ack_size, sizeof(ack_size));

    SrsCommonMessage* msg = NULL;
    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    srs_freep(msg);
    EXPECT_EQ(16u, proto.in_ack_size.window);
    EXPECT_EQ(0, io.out_length());                // 첫 메시지 수신 시점엔 window=0이라 ack 없음.

    // 다음 메시지 수신에서 delta >= window/2 → Acknowledgement 자동 송신.
    uint8_t audio[] = {
        0x04,
        0x00, 0x00, 0x0a,
        0x00, 0x00, 0x02,
        0x08,
        0x01, 0x00, 0x00, 0x00,
        0xaa, 0xbb,
    };
    io.append(audio, sizeof(audio));

    HELPER_ASSERT_SUCCESS(proto.recv_message(&msg));
    srs_freep(msg);

    // ack = fmt0 cid2 헤더(12B) + type3 페이로드 4B(수신 바이트 수).
    ASSERT_EQ(16, io.out_length());
    char* out = io.out_buffer.bytes();
    EXPECT_EQ(0x02, out[0]);
    EXPECT_EQ(0x03, out[7]);                      // Acknowledgement
    SrsBuffer seq(out + 12, 4);
    EXPECT_EQ((uint32_t)io.get_recv_bytes(), (uint32_t)seq.read_4bytes());
}

// 송신 경로: fmt=0 첫 청크 + fmt=3 연속 청크로 슬라이싱된 출력을
// 다시 수신 파서에 넣어 원본 메시지가 복원되는지 확인한다 (라운드트립).
VOID TEST(ProtocolStackTest, SendMessageRoundTrip)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    // 300바이트 오디오 — out_chunk_size(128) 기준 3청크.
    int size = 300;
    char* payload = new char[size];
    for (int i = 0; i < size; i++) {
        payload[i] = (char)(i % 256);
    }

    SrsMessageHeader header;
    header.initialize_audio(size, 90, 1);

    SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
    HELPER_ASSERT_SUCCESS(msg->create(&header, payload, size));
    HELPER_ASSERT_SUCCESS(proto.send_and_free_message(msg, 1));

    // 출력: 12B(c0) + 128B + 1B(c3) + 128B + 1B(c3) + 44B.
    ASSERT_EQ(12 + 128 + 1 + 128 + 1 + 44, io.out_length());

    // 송신 바이트를 다른 프로토콜 스택의 입력으로 재주입.
    MockBufferIO io2;
    io2.append((uint8_t*)io.out_buffer.bytes(), io.out_length());
    SrsProtocol proto2(&io2);

    SrsCommonMessage* recv = NULL;
    HELPER_ASSERT_SUCCESS(proto2.recv_message(&recv));
    ASSERT_TRUE(recv != NULL);
    EXPECT_EQ(8, recv->header.message_type);
    EXPECT_EQ(300, recv->size);
    EXPECT_EQ(90, (int)recv->header.timestamp);
    EXPECT_EQ(1, recv->header.stream_id);
    for (int i = 0; i < size; i++) {
        ASSERT_EQ((char)(i % 256), recv->payload[i]);
    }
    srs_freep(recv);
}

// extended timestamp 송신 라운드트립: ts >= 0xFFFFFF면 c0/c3 모두 확장 타임스탬프를 붙인다.
VOID TEST(ProtocolStackTest, SendExtendedTimestampRoundTrip)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);

    int size = 200;
    char* payload = new char[size];
    for (int i = 0; i < size; i++) {
        payload[i] = (char)(i % 256);
    }

    SrsMessageHeader header;
    header.initialize_video(size, 0x01234567, 1);

    SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
    HELPER_ASSERT_SUCCESS(msg->create(&header, payload, size));
    HELPER_ASSERT_SUCCESS(proto.send_and_free_message(msg, 1));

    // 출력: 16B(c0+ext) + 128B + 5B(c3+ext) + 72B.
    ASSERT_EQ(16 + 128 + 5 + 72, io.out_length());

    MockBufferIO io2;
    io2.append((uint8_t*)io.out_buffer.bytes(), io.out_length());
    SrsProtocol proto2(&io2);

    SrsCommonMessage* recv = NULL;
    HELPER_ASSERT_SUCCESS(proto2.recv_message(&recv));
    ASSERT_TRUE(recv != NULL);
    EXPECT_EQ(0x01234567, (int)recv->header.timestamp);
    EXPECT_EQ(200, recv->size);
    // payload는 send_and_free_message가 소유권을 가져가 해제했으므로 값으로 비교한다.
    for (int i = 0; i < size; i++) {
        ASSERT_EQ((char)(i % 256), recv->payload[i]);
    }
    srs_freep(recv);
}

// 아웃바운드 SetChunkSize를 보내면 out_chunk_size가 갱신되어 이후 송신이 큰 청크를 쓴다.
VOID TEST(ProtocolStackTest, SendSetChunkSizePacket)
{
    srs_error_t err = srs_success;

    MockBufferIO io;
    SrsProtocol proto(&io);
    EXPECT_EQ(128, proto.out_chunk_size);

    SrsSetChunkSizePacket* pkt = new SrsSetChunkSizePacket();
    pkt->chunk_size = 60000;
    HELPER_ASSERT_SUCCESS(proto.send_and_free_packet(pkt, 0));
    EXPECT_EQ(60000, proto.out_chunk_size);

    // 송신된 바이트: fmt0 cid2 + type1 + 4바이트 60000.
    ASSERT_EQ(16, io.out_length());
    char* out = io.out_buffer.bytes();
    EXPECT_EQ(0x02, out[0]);
    EXPECT_EQ(0x01, out[7]);
    SrsBuffer b(out + 12, 4);
    EXPECT_EQ(60000, b.read_4bytes());

    // 이후 300바이트 메시지는 단일 청크로 나간다.
    int size = 300;
    char* payload = new char[size];
    memset(payload, 0xab, size);

    SrsMessageHeader header;
    header.initialize_audio(size, 10, 1);

    SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
    HELPER_ASSERT_SUCCESS(msg->create(&header, payload, size));
    HELPER_ASSERT_SUCCESS(proto.send_and_free_message(msg, 1));
    EXPECT_EQ(16 + 12 + 300, io.out_length());
}

// 컨트롤 패킷 5종: encode → decode 라운드트립.
VOID TEST(ProtocolStackTest, ControlPacketsRoundTrip)
{
    srs_error_t err = srs_success;

    // SetChunkSize
    if (true) {
        SrsSetChunkSizePacket enc;
        enc.chunk_size = 60000;
        int size = 0;
        char* payload = NULL;
        HELPER_ASSERT_SUCCESS(enc.encode(size, payload));
        ASSERT_EQ(4, size);

        SrsBuffer b(payload, size);
        SrsSetChunkSizePacket dec;
        HELPER_ASSERT_SUCCESS(dec.decode(&b));
        EXPECT_EQ(60000, dec.chunk_size);
        srs_freepa(payload);
    }

    // WindowAckSize
    if (true) {
        SrsSetWindowAckSizePacket enc;
        enc.ackowledgement_window_size = 2500000;
        int size = 0;
        char* payload = NULL;
        HELPER_ASSERT_SUCCESS(enc.encode(size, payload));
        ASSERT_EQ(4, size);

        SrsBuffer b(payload, size);
        SrsSetWindowAckSizePacket dec;
        HELPER_ASSERT_SUCCESS(dec.decode(&b));
        EXPECT_EQ(2500000, dec.ackowledgement_window_size);
        srs_freepa(payload);
    }

    // Acknowledgement
    if (true) {
        SrsAcknowledgementPacket enc;
        enc.sequence_number = 0x12345678u;
        int size = 0;
        char* payload = NULL;
        HELPER_ASSERT_SUCCESS(enc.encode(size, payload));
        ASSERT_EQ(4, size);

        SrsBuffer b(payload, size);
        SrsAcknowledgementPacket dec;
        HELPER_ASSERT_SUCCESS(dec.decode(&b));
        EXPECT_EQ(0x12345678u, dec.sequence_number);
        srs_freepa(payload);
    }

    // SetPeerBandwidth (송신 전용 — 인코딩만 확인)
    if (true) {
        SrsSetPeerBandwidthPacket enc;
        enc.bandwidth = 2500000;
        enc.type = SrsPeerBandwidthDynamic;
        int size = 0;
        char* payload = NULL;
        HELPER_ASSERT_SUCCESS(enc.encode(size, payload));
        ASSERT_EQ(5, size);

        SrsBuffer b(payload, size);
        EXPECT_EQ(2500000, b.read_4bytes());
        EXPECT_EQ(SrsPeerBandwidthDynamic, b.read_1bytes());
        srs_freepa(payload);
    }

    // UserControl: StreamBegin과 SetBufferLength(8바이트 event data).
    if (true) {
        SrsUserControlPacket enc;
        enc.event_type = SrcPCUCStreamBegin;
        enc.event_data = 1;
        int size = 0;
        char* payload = NULL;
        HELPER_ASSERT_SUCCESS(enc.encode(size, payload));
        ASSERT_EQ(6, size);

        SrsBuffer b(payload, size);
        SrsUserControlPacket dec;
        HELPER_ASSERT_SUCCESS(dec.decode(&b));
        EXPECT_EQ(SrcPCUCStreamBegin, dec.event_type);
        EXPECT_EQ(1, dec.event_data);
        srs_freepa(payload);
    }

    if (true) {
        SrsUserControlPacket enc;
        enc.event_type = SrcPCUCSetBufferLength;
        enc.event_data = 1;
        enc.extra_data = 3000;
        int size = 0;
        char* payload = NULL;
        HELPER_ASSERT_SUCCESS(enc.encode(size, payload));
        ASSERT_EQ(10, size);

        SrsBuffer b(payload, size);
        SrsUserControlPacket dec;
        HELPER_ASSERT_SUCCESS(dec.decode(&b));
        EXPECT_EQ(SrcPCUCSetBufferLength, dec.event_type);
        EXPECT_EQ(1, dec.event_data);
        EXPECT_EQ(3000, dec.extra_data);
        srs_freepa(payload);
    }
}

// ---------------------------------------------------------------------------
// S6 — 커맨드 패킷 + SrsRtmpServer (커맨드 흐름)
// ---------------------------------------------------------------------------

// 한쪽 프로토콜이 송신한 바이트를 다른 쪽 io의 입력으로 옮긴다 (소켓 배선 흉내).
static void mock_pipe(MockBufferIO* from, MockBufferIO* to)
{
    to->append((uint8_t*)from->out_buffer.bytes(), from->out_length());
    from->out_buffer.erase(from->out_length());
}

// tcUrl 파싱: connect 시점(스트림 없음)과 identify 이후(스트림/파라미터 재파싱) 모두.
VOID TEST(ProtocolRtmpServerTest, DiscoveryTcUrl)
{
    // 기본형: rtmp://ip:port/app
    if (true) {
        std::string schema, host, vhost, app, stream, param; int port = 0;
        srs_discovery_tc_url("rtmp://127.0.0.1:19350/live", schema, host, vhost, app, stream, port, param);
        EXPECT_STREQ("rtmp", schema.c_str());
        EXPECT_STREQ("127.0.0.1", host.c_str());
        EXPECT_STREQ("127.0.0.1", vhost.c_str());
        EXPECT_STREQ("live", app.c_str());
        EXPECT_EQ(19350, port);
        EXPECT_STREQ("", stream.c_str());
        EXPECT_STREQ("", param.c_str());
    }

    // vhost 쿼리: rtmp://ip/app?vhost=xxx (포트 생략 → 1935)
    if (true) {
        std::string schema, host, vhost, app, stream, param; int port = 0;
        srs_discovery_tc_url("rtmp://ossrs.net/live?vhost=demo.srs.com", schema, host, vhost, app, stream, port, param);
        EXPECT_STREQ("ossrs.net", host.c_str());
        EXPECT_STREQ("demo.srs.com", vhost.c_str());
        EXPECT_STREQ("live", app.c_str());
        EXPECT_EQ(1935, port);
    }

    // identify 이후 재파싱: stream에 붙은 쿼리를 param으로 분리
    if (true) {
        std::string schema, host, vhost, app, param; int port = 0;
        std::string stream = "livestream?key=abc";
        srs_discovery_tc_url("rtmp://127.0.0.1/live", schema, host, vhost, app, stream, port, param);
        EXPECT_STREQ("live", app.c_str());
        EXPECT_STREQ("livestream", stream.c_str());
        EXPECT_STREQ("?key=abc", param.c_str());
    }

    // 기본 vhost만 쿼리에 있으면 param을 비운다
    if (true) {
        std::string schema, host, vhost, app, stream, param; int port = 0;
        srs_discovery_tc_url("rtmp://127.0.0.1/live?vhost=__defaultVhost__", schema, host, vhost, app, stream, port, param);
        EXPECT_STREQ("127.0.0.1", vhost.c_str());
        EXPECT_STREQ("", param.c_str());
    }
}

// connect(app) → 부트스트랩(WindowAck/PeerBW/ChunkSize) → _result.
// 클라이언트 역할 SrsProtocol이 요청을 만들고(requests[tid] 기록),
// 서버 응답 바이트를 되받아 _result(connect)를 디코드한다.
VOID TEST(ProtocolRtmpServerTest, ConnectAppAndResponse)
{
    srs_error_t err = srs_success;

    MockBufferIO sio;
    SrsRtmpServer rtmp(&sio);

    MockBufferIO cio;
    SrsProtocol client(&cio);

    // 클라이언트: connect(tid=1) 송신
    if (true) {
        SrsConnectAppPacket* pkt = new SrsConnectAppPacket();
        pkt->command_object->set("app", SrsAmf0Any::str("live"));
        pkt->command_object->set("tcUrl", SrsAmf0Any::str("rtmp://127.0.0.1:1935/live"));
        pkt->command_object->set("objectEncoding", SrsAmf0Any::number(0));
        HELPER_ASSERT_SUCCESS(client.send_and_free_packet(pkt, 0));
    }
    mock_pipe(&cio, &sio);

    // 서버: connect_app → tcUrl 파싱
    SrsRequest req;
    HELPER_ASSERT_SUCCESS(rtmp.connect_app(&req));
    EXPECT_STREQ("rtmp", req.schema.c_str());
    EXPECT_STREQ("127.0.0.1", req.host.c_str());
    EXPECT_STREQ("127.0.0.1", req.vhost.c_str());
    EXPECT_STREQ("live", req.app.c_str());
    EXPECT_EQ(1935, req.port);
    EXPECT_STREQ("rtmp://127.0.0.1:1935/live", req.tcUrl.c_str());

    // 서버 부트스트랩: SetChunkSize는 128바이트를 넘는 connect 응답보다 먼저 — 이슈 #454, CLAUDE.md §5.3
    HELPER_ASSERT_SUCCESS(rtmp.set_window_ack_size(2500000));
    HELPER_ASSERT_SUCCESS(rtmp.set_peer_bandwidth(2500000, (int)SrsPeerBandwidthDynamic));
    HELPER_ASSERT_SUCCESS(rtmp.set_chunk_size(60000));
    HELPER_ASSERT_SUCCESS(rtmp.response_connect_app(&req, "127.0.0.1"));

    // 클라이언트: 컨트롤 3종을 지나 _result(connect) 디코드 (requests[1]="connect" 덕분)
    mock_pipe(&sio, &cio);
    SrsCommonMessage* msg = NULL;
    SrsConnectAppResPacket* res = NULL;
    HELPER_ASSERT_SUCCESS(client.expect_message<SrsConnectAppResPacket>(&msg, &res));
    EXPECT_EQ(1, (int)res->transaction_id);
    if (true) {
        SrsAmf0Any* prop = res->props->ensure_property_string("fmsVer");
        ASSERT_TRUE(prop != NULL);
        EXPECT_STREQ("FMS/" RTMP_SIG_FMS_VER, prop->to_str().c_str());
    }
    if (true) {
        SrsAmf0Any* prop = res->info->ensure_property_string(StatusCode);
        ASSERT_TRUE(prop != NULL);
        EXPECT_STREQ(StatusCodeConnectSuccess, prop->to_str().c_str());
    }
    // SetChunkSize(60000)가 반영되어 클라이언트 수신 청크 크기가 갱신됐는지
    EXPECT_EQ(60000, client.in_chunk_size);
    srs_freep(msg);
    srs_freep(res);
}

// FMLE(OBS/ffmpeg) publisher: releaseStream → identify → FMLEPublish 판별 + _result 응답.
VOID TEST(ProtocolRtmpServerTest, IdentifyFmlePublish)
{
    srs_error_t err = srs_success;

    MockBufferIO sio;
    SrsRtmpServer rtmp(&sio);

    MockBufferIO cio;
    SrsProtocol client(&cio);

    // 클라이언트: releaseStream("livestream") tid=2
    HELPER_ASSERT_SUCCESS(client.send_and_free_packet(SrsFMLEStartPacket::create_release_stream("livestream"), 0));
    mock_pipe(&cio, &sio);

    SrsRtmpConnType type = SrsRtmpConnUnknown;
    std::string stream_name;
    srs_utime_t duration = 0;
    HELPER_ASSERT_SUCCESS(rtmp.identify_client(1, type, stream_name, duration));
    EXPECT_EQ(SrsRtmpConnFMLEPublish, type);
    EXPECT_STREQ("livestream", stream_name.c_str());

    // 서버의 _result(releaseStream)를 클라이언트가 디코드 (requests[2]="releaseStream")
    mock_pipe(&sio, &cio);
    SrsCommonMessage* msg = NULL;
    SrsFMLEStartResPacket* res = NULL;
    HELPER_ASSERT_SUCCESS(client.expect_message<SrsFMLEStartResPacket>(&msg, &res));
    EXPECT_EQ(2, (int)res->transaction_id);
    srs_freep(msg);
    srs_freep(res);
}

// player(ffplay): createStream → _result(streamId=1) → play → Play 판별.
VOID TEST(ProtocolRtmpServerTest, IdentifyPlayViaCreateStream)
{
    srs_error_t err = srs_success;

    MockBufferIO sio;
    SrsRtmpServer rtmp(&sio);

    MockBufferIO cio;
    SrsProtocol client(&cio);

    // 클라이언트: createStream(tid=2) → play("livestream")
    if (true) {
        SrsCreateStreamPacket* pkt = new SrsCreateStreamPacket();
        HELPER_ASSERT_SUCCESS(client.send_and_free_packet(pkt, 0));
    }
    if (true) {
        SrsPlayPacket* pkt = new SrsPlayPacket();
        pkt->stream_name = "livestream";
        HELPER_ASSERT_SUCCESS(client.send_and_free_packet(pkt, 1));
    }
    mock_pipe(&cio, &sio);

    SrsRtmpConnType type = SrsRtmpConnUnknown;
    std::string stream_name;
    srs_utime_t duration = 0;
    HELPER_ASSERT_SUCCESS(rtmp.identify_client(1, type, stream_name, duration));
    EXPECT_EQ(SrsRtmpConnPlay, type);
    EXPECT_STREQ("livestream", stream_name.c_str());
    EXPECT_EQ(-1 * SRS_UTIME_MILLISECONDS, duration);

    // 서버의 _result(createStream)를 클라이언트가 디코드: 응답 streamId는 1
    mock_pipe(&sio, &cio);
    SrsCommonMessage* msg = NULL;
    SrsCreateStreamResPacket* res = NULL;
    HELPER_ASSERT_SUCCESS(client.expect_message<SrsCreateStreamResPacket>(&msg, &res));
    EXPECT_EQ(2, (int)res->transaction_id);
    EXPECT_EQ(1, (int)res->stream_id);
    srs_freep(msg);
    srs_freep(res);
}

// FMLE publish 시퀀스 2부: FCPublish → createStream → publish에 각각 응답,
// onStatus(Publish.Start)는 인가 후 start_publishing이 분리 송신 — 이슈 #4037, CLAUDE.md §5.3.
VOID TEST(ProtocolRtmpServerTest, StartFmlePublish)
{
    srs_error_t err = srs_success;

    MockBufferIO sio;
    SrsRtmpServer rtmp(&sio);

    MockBufferIO cio;
    SrsProtocol client(&cio);

    // 클라이언트: FCPublish(tid=3) → createStream(tid=4) → publish("livestream", sid=1)
    HELPER_ASSERT_SUCCESS(client.send_and_free_packet(SrsFMLEStartPacket::create_FC_publish("livestream"), 0));
    if (true) {
        SrsCreateStreamPacket* pkt = new SrsCreateStreamPacket();
        pkt->transaction_id = 4;
        HELPER_ASSERT_SUCCESS(client.send_and_free_packet(pkt, 0));
    }
    if (true) {
        SrsPublishPacket* pkt = new SrsPublishPacket();
        pkt->stream_name = "livestream";
        HELPER_ASSERT_SUCCESS(client.send_and_free_packet(pkt, 1));
    }
    mock_pipe(&cio, &sio);

    HELPER_ASSERT_SUCCESS(rtmp.start_fmle_publish(1));
    HELPER_ASSERT_SUCCESS(rtmp.start_publishing(1));
    mock_pipe(&sio, &cio);

    // 클라이언트: _result(FCPublish, tid=3)
    if (true) {
        SrsCommonMessage* msg = NULL;
        SrsFMLEStartResPacket* res = NULL;
        HELPER_ASSERT_SUCCESS(client.expect_message<SrsFMLEStartResPacket>(&msg, &res));
        EXPECT_EQ(3, (int)res->transaction_id);
        srs_freep(msg);
        srs_freep(res);
    }
    // _result(createStream, tid=4, streamId=1)
    if (true) {
        SrsCommonMessage* msg = NULL;
        SrsCreateStreamResPacket* res = NULL;
        HELPER_ASSERT_SUCCESS(client.expect_message<SrsCreateStreamResPacket>(&msg, &res));
        EXPECT_EQ(4, (int)res->transaction_id);
        EXPECT_EQ(1, (int)res->stream_id);
        srs_freep(msg);
        srs_freep(res);
    }
    // onFCPublish — 클라이언트 디스패치에 없는 커맨드이므로 페이로드를 직접 AMF0로 검증
    if (true) {
        SrsCommonMessage* msg = NULL;
        HELPER_ASSERT_SUCCESS(client.recv_message(&msg));
        ASSERT_TRUE(msg->header.is_amf0_command());
        SrsBuffer b(msg->payload, msg->size);
        std::string cmd;
        HELPER_ASSERT_SUCCESS(srs_amf0_read_string(&b, cmd));
        EXPECT_STREQ("onFCPublish", cmd.c_str());
        srs_freep(msg);
    }
    // onStatus(NetStream.Publish.Start), stream_id=1
    if (true) {
        SrsCommonMessage* msg = NULL;
        HELPER_ASSERT_SUCCESS(client.recv_message(&msg));
        ASSERT_TRUE(msg->header.is_amf0_command());
        EXPECT_EQ(1, msg->header.stream_id);

        SrsBuffer b(msg->payload, msg->size);
        std::string cmd;
        double tid = -1;
        HELPER_ASSERT_SUCCESS(srs_amf0_read_string(&b, cmd));
        EXPECT_STREQ("onStatus", cmd.c_str());
        HELPER_ASSERT_SUCCESS(srs_amf0_read_number(&b, tid));
        HELPER_ASSERT_SUCCESS(srs_amf0_read_null(&b));

        SrsAmf0Any* any = NULL;
        HELPER_ASSERT_SUCCESS(srs_amf0_read_any(&b, &any));
        ASSERT_TRUE(any->is_object());
        SrsAmf0Any* prop = any->to_object()->ensure_property_string(StatusCode);
        ASSERT_TRUE(prop != NULL);
        EXPECT_STREQ(StatusCodePublishStart, prop->to_str().c_str());
        srs_freep(any);
        srs_freep(msg);
    }
}

// play 시작: StreamBegin → onStatus(Play.Reset) → onStatus(Play.Start) → |RtmpSampleAccess.
VOID TEST(ProtocolRtmpServerTest, StartPlay)
{
    srs_error_t err = srs_success;

    MockBufferIO sio;
    SrsRtmpServer rtmp(&sio);
    HELPER_ASSERT_SUCCESS(rtmp.start_play(1));

    MockBufferIO cio;
    SrsProtocol client(&cio);
    cio.append((uint8_t*)sio.out_buffer.bytes(), sio.out_length());

    // UserControl StreamBegin(sid=1), stream_id=0으로 전송 — CLAUDE.md §2.5
    if (true) {
        SrsCommonMessage* msg = NULL;
        SrsUserControlPacket* pkt = NULL;
        HELPER_ASSERT_SUCCESS(client.expect_message<SrsUserControlPacket>(&msg, &pkt));
        EXPECT_EQ(SrcPCUCStreamBegin, pkt->event_type);
        EXPECT_EQ(1, pkt->event_data);
        EXPECT_EQ(0, msg->header.stream_id);
        srs_freep(msg);
        srs_freep(pkt);
    }
    // onStatus 2종: Reset → Start, stream_id=1로 전송
    const char* codes[] = { StatusCodeStreamReset, StatusCodeStreamStart };
    for (int i = 0; i < 2; i++) {
        SrsCommonMessage* msg = NULL;
        HELPER_ASSERT_SUCCESS(client.recv_message(&msg));
        ASSERT_TRUE(msg->header.is_amf0_command());
        EXPECT_EQ(1, msg->header.stream_id);

        SrsBuffer b(msg->payload, msg->size);
        std::string cmd;
        double tid = -1;
        HELPER_ASSERT_SUCCESS(srs_amf0_read_string(&b, cmd));
        EXPECT_STREQ("onStatus", cmd.c_str());
        HELPER_ASSERT_SUCCESS(srs_amf0_read_number(&b, tid));
        HELPER_ASSERT_SUCCESS(srs_amf0_read_null(&b));

        SrsAmf0Any* any = NULL;
        HELPER_ASSERT_SUCCESS(srs_amf0_read_any(&b, &any));
        ASSERT_TRUE(any->is_object());
        SrsAmf0Any* prop = any->to_object()->ensure_property_string(StatusCode);
        ASSERT_TRUE(prop != NULL);
        EXPECT_STREQ(codes[i], prop->to_str().c_str());
        srs_freep(any);
        srs_freep(msg);
    }
    // |RtmpSampleAccess(true, true) — AMF0 data 메시지
    if (true) {
        SrsCommonMessage* msg = NULL;
        HELPER_ASSERT_SUCCESS(client.recv_message(&msg));
        ASSERT_TRUE(msg->header.is_amf0_data());

        SrsBuffer b(msg->payload, msg->size);
        std::string cmd;
        bool video_sample_access = false;
        bool audio_sample_access = false;
        HELPER_ASSERT_SUCCESS(srs_amf0_read_string(&b, cmd));
        EXPECT_STREQ(RTMP_AMF0_DATA_SAMPLE_ACCESS, cmd.c_str());
        HELPER_ASSERT_SUCCESS(srs_amf0_read_boolean(&b, video_sample_access));
        HELPER_ASSERT_SUCCESS(srs_amf0_read_boolean(&b, audio_sample_access));
        EXPECT_TRUE(video_sample_access);
        EXPECT_TRUE(audio_sample_access);
        srs_freep(msg);
    }
}

// FMLE unpublish: onFCUnpublish → _result(FCUnpublish) → onStatus(Unpublish.Success).
VOID TEST(ProtocolRtmpServerTest, FmleUnpublish)
{
    srs_error_t err = srs_success;

    MockBufferIO sio;
    SrsRtmpServer rtmp(&sio);

    MockBufferIO cio;
    SrsProtocol client(&cio);

    // 클라이언트가 FCUnpublish(tid=5)를 보냈다고 가정 — requests[5] 기록용
    if (true) {
        SrsFMLEStartPacket* pkt = new SrsFMLEStartPacket();
        pkt->command_name = RTMP_AMF0_COMMAND_UNPUBLISH;
        pkt->transaction_id = 5;
        pkt->stream_name = "livestream";
        HELPER_ASSERT_SUCCESS(client.send_and_free_packet(pkt, 0));
    }

    HELPER_ASSERT_SUCCESS(rtmp.fmle_unpublish(1, 5));
    mock_pipe(&sio, &cio);

    // onFCUnpublish는 expect_message가 건너뛰고, _result(tid=5)를 디코드
    SrsCommonMessage* msg = NULL;
    SrsFMLEStartResPacket* res = NULL;
    HELPER_ASSERT_SUCCESS(client.expect_message<SrsFMLEStartResPacket>(&msg, &res));
    EXPECT_EQ(5, (int)res->transaction_id);
    srs_freep(msg);
    srs_freep(res);

    // 마지막 onStatus(NetStream.Unpublish.Success)
    if (true) {
        SrsCommonMessage* msg2 = NULL;
        HELPER_ASSERT_SUCCESS(client.recv_message(&msg2));
        SrsBuffer b(msg2->payload, msg2->size);
        std::string cmd;
        HELPER_ASSERT_SUCCESS(srs_amf0_read_string(&b, cmd));
        EXPECT_STREQ("onStatus", cmd.c_str());
        srs_freep(msg2);
    }
}

// onMetaData 디코드: FMLE의 @setDataFrame 스트립 + EcmaArray→Object 변환 (ffmpeg 경로).
VOID TEST(ProtocolRtmpServerTest, OnMetaDataPacketDecode)
{
    srs_error_t err = srs_success;

    // @setDataFrame + onMetaData + EcmaArray
    if (true) {
        char data[256];
        SrsBuffer w(data, sizeof(data));
        HELPER_ASSERT_SUCCESS(srs_amf0_write_string(&w, "@setDataFrame"));
        HELPER_ASSERT_SUCCESS(srs_amf0_write_string(&w, "onMetaData"));
        SrsAmf0EcmaArray* arr = SrsAmf0Any::ecma_array();
        arr->set("width", SrsAmf0Any::number(1280));
        arr->set("height", SrsAmf0Any::number(720));
        HELPER_ASSERT_SUCCESS(arr->write(&w));
        srs_freep(arr);

        SrsBuffer r(data, w.pos());
        SrsOnMetaDataPacket pkt;
        HELPER_ASSERT_SUCCESS(pkt.decode(&r));
        EXPECT_STREQ("onMetaData", pkt.name.c_str());
        SrsAmf0Any* prop = pkt.metadata->ensure_property_number("width");
        ASSERT_TRUE(prop != NULL);
        EXPECT_EQ(1280, (int)prop->to_number());
    }

    // onMetaData + Object (OBS 경로)
    if (true) {
        char data[256];
        SrsBuffer w(data, sizeof(data));
        HELPER_ASSERT_SUCCESS(srs_amf0_write_string(&w, "onMetaData"));
        SrsAmf0Object* obj = SrsAmf0Any::object();
        obj->set("duration", SrsAmf0Any::number(0));
        HELPER_ASSERT_SUCCESS(obj->write(&w));
        srs_freep(obj);

        SrsBuffer r(data, w.pos());
        SrsOnMetaDataPacket pkt;
        HELPER_ASSERT_SUCCESS(pkt.decode(&r));
        EXPECT_STREQ("onMetaData", pkt.name.c_str());
        EXPECT_TRUE(pkt.metadata->ensure_property_number("duration") != NULL);
    }
}
