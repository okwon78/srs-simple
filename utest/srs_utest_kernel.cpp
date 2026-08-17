// srs_simple — 원본: trunk/src/utest/srs_utest_kernel.cpp (에러/버퍼 부분)
#include <srs_utest.hpp>

#include <srs_kernel_buffer.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_flv.hpp>

VOID TEST(KernelErrorTest, SuccessIsNull)
{
    EXPECT_TRUE(srs_success == NULL);
    EXPECT_EQ(ERROR_SUCCESS, srs_error_code(srs_success));
    EXPECT_STREQ("Success", srs_error_desc(srs_success).c_str());
    EXPECT_STREQ("Success", srs_error_summary(srs_success).c_str());
}

VOID TEST(KernelErrorTest, ErrorNew)
{
    srs_error_t err = srs_error_new(ERROR_RTMP_HANDSHAKE, "hs failed, nn=%d", 16);
    EXPECT_EQ(ERROR_RTMP_HANDSHAKE, srs_error_code(err));
    EXPECT_STREQ("RtmpHandshake", srs_error_code_str(err).c_str());

    string desc = srs_error_desc(err);
    EXPECT_TRUE(desc.find("code=2016") != string::npos);
    EXPECT_TRUE(desc.find("RtmpHandshake") != string::npos);
    EXPECT_TRUE(desc.find("hs failed, nn=16") != string::npos);

    srs_freep(err);
}

// 완료 조건: wrap 2단 후 desc에 각 단계의 프레임(파일:라인)이 모두 남는다.
VOID TEST(KernelErrorTest, ErrorWrapChain)
{
    srs_error_t err = srs_error_new(ERROR_SOCKET_READ, "read 4 bytes");
    err = srs_error_wrap(err, "recv chunk header");
    err = srs_error_wrap(err, "recv message");

    // The code is inherited from the root cause.
    EXPECT_EQ(ERROR_SOCKET_READ, srs_error_code(err));

    string desc = srs_error_desc(err);
    // All messages are in the chain, ordered from outer to inner.
    size_t p0 = desc.find("recv message");
    size_t p1 = desc.find("recv chunk header");
    size_t p2 = desc.find("read 4 bytes");
    EXPECT_TRUE(p0 != string::npos && p1 != string::npos && p2 != string::npos);
    EXPECT_TRUE(p0 < p1 && p1 < p2);

    // One stack frame per level: new + wrap + wrap = 3 frames.
    int frames = 0;
    for (size_t p = desc.find("thread ["); p != string::npos; p = desc.find("thread [", p + 1)) {
        frames++;
    }
    EXPECT_EQ(3, frames);
    EXPECT_TRUE(desc.find("srs_utest_kernel.cpp") != string::npos);

    srs_freep(err);
}

VOID TEST(KernelErrorTest, ErrorCopy)
{
    srs_error_t err = srs_error_new(ERROR_RTMP_AMF0_DECODE, "amf0 decode");
    err = srs_error_wrap(err, "decode packet");

    srs_error_t cp = srs_error_copy(err);
    EXPECT_TRUE(cp != err);
    EXPECT_EQ(srs_error_code(err), srs_error_code(cp));
    EXPECT_STREQ(srs_error_desc(err).c_str(), srs_error_desc(cp).c_str());

    EXPECT_TRUE(srs_error_copy(srs_success) == srs_success);

    srs_freep(err);
    srs_freep(cp);
}

VOID TEST(KernelErrorTest, ErrorReset)
{
    srs_error_t err = srs_error_new(ERROR_SOCKET_TIMEOUT, "timeout");
    EXPECT_TRUE(err != srs_success);

    srs_error_reset(err);
    EXPECT_TRUE(err == srs_success);
}

VOID TEST(KernelErrorTest, UnknownCodeHasNoName)
{
    // 축소된 enum에 없는 코드는 이름 없이 code=만 나온다.
    srs_error_t err = srs_error_new(12345, "unknown");
    EXPECT_STREQ("", srs_error_code_str(err).c_str());
    srs_freep(err);
}

VOID TEST(KernelBufferTest, PosLeftEmptyRequire)
{
    char data[8];
    SrsBuffer b(data, 8);

    EXPECT_EQ(0, b.pos());
    EXPECT_EQ(8, b.left());
    EXPECT_EQ(8, b.size());
    EXPECT_FALSE(b.empty());
    EXPECT_TRUE(b.require(8));
    EXPECT_FALSE(b.require(9));
    EXPECT_FALSE(b.require(-1));
    EXPECT_TRUE(b.data() == data);
    EXPECT_TRUE(b.head() == data);

    b.read_4bytes();
    EXPECT_EQ(4, b.pos());
    EXPECT_EQ(4, b.left());
    EXPECT_TRUE(b.head() == data + 4);

    b.skip(4);
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(0, b.left());

    // to skip(-pos()) to reset buffer.
    b.skip(-1 * b.pos());
    EXPECT_EQ(0, b.pos());
    EXPECT_EQ(8, b.left());
}

VOID TEST(KernelBufferTest, BigEndianEncoding)
{
    char data[8] = {0};

    // 네트워크 바이트오더: MSB가 먼저.
    if (true) {
        SrsBuffer b(data, 8);
        b.write_2bytes(0x0102);
        EXPECT_EQ(0x01, data[0]);
        EXPECT_EQ(0x02, data[1]);
    }
    if (true) {
        SrsBuffer b(data, 8);
        b.write_3bytes(0x010203);
        EXPECT_EQ(0x01, data[0]);
        EXPECT_EQ(0x02, data[1]);
        EXPECT_EQ(0x03, data[2]);
    }
    if (true) {
        SrsBuffer b(data, 8);
        b.write_4bytes(0x01020304);
        EXPECT_EQ(0x01, data[0]);
        EXPECT_EQ(0x04, data[3]);
    }
    // 청크 헤더의 stream_id만 리틀엔디언.
    if (true) {
        SrsBuffer b(data, 8);
        b.write_le4bytes(0x01020304);
        EXPECT_EQ(0x04, data[0]);
        EXPECT_EQ(0x01, data[3]);
    }
}

VOID TEST(KernelBufferTest, ReadWriteRoundtrip)
{
    char data[64];
    SrsBuffer w(data, 64);

    w.write_1bytes(0x7f);
    w.write_2bytes(0x0102);
    w.write_3bytes(0x123456);
    w.write_4bytes(0x01020304);
    w.write_le4bytes(0x04030201);
    w.write_8bytes(0x0102030405060708LL);
    EXPECT_EQ(1 + 2 + 3 + 4 + 4 + 8, w.pos());

    SrsBuffer r(data, 64);
    EXPECT_EQ(0x7f, r.read_1bytes());
    EXPECT_EQ(0x0102, r.read_2bytes());
    EXPECT_EQ(0x123456, r.read_3bytes());
    EXPECT_EQ(0x01020304, r.read_4bytes());
    EXPECT_EQ(0x04030201, r.read_le4bytes());
    EXPECT_EQ(0x0102030405060708LL, r.read_8bytes());
    EXPECT_EQ(w.pos(), r.pos());
}

VOID TEST(KernelBufferTest, StringBytesRoundtrip)
{
    char data[32];
    SrsBuffer w(data, 32);

    w.write_string("winlin");
    char bytes[] = {0x01, 0x02, 0x03};
    w.write_bytes(bytes, 3);
    EXPECT_EQ(9, w.pos());

    SrsBuffer r(data, 32);
    EXPECT_STREQ("winlin", r.read_string(6).c_str());

    char out[3];
    r.read_bytes(out, 3);
    EXPECT_EQ(0x01, out[0]);
    EXPECT_EQ(0x02, out[1]);
    EXPECT_EQ(0x03, out[2]);
}

VOID TEST(KernelBufferTest, CopyKeepsPosition)
{
    char data[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    SrsBuffer b(data, 8);
    b.read_4bytes();

    SrsBuffer* cp = b.copy();
    EXPECT_EQ(4, cp->pos());
    EXPECT_EQ(4, cp->left());
    EXPECT_EQ(0x05060708, cp->read_4bytes());
    // The copy shares bytes but owns its position.
    EXPECT_EQ(4, b.pos());
    srs_freep(cp);
}

// S3 — 메시지 모델 (srs_kernel_flv)

VOID TEST(KernelFlvTest, MessageHeaderJudgers)
{
    SrsMessageHeader h;
    // 기본값: 커맨드용 cid를 선호.
    EXPECT_EQ(RTMP_CID_OverConnection, h.prefer_cid);

    h.message_type = RTMP_MSG_AudioMessage;
    EXPECT_TRUE(h.is_audio());
    EXPECT_FALSE(h.is_video());

    h.message_type = RTMP_MSG_VideoMessage;
    EXPECT_TRUE(h.is_video());

    h.message_type = RTMP_MSG_AMF0CommandMessage;
    EXPECT_TRUE(h.is_amf0_command());
    EXPECT_FALSE(h.is_amf0_data());

    h.message_type = RTMP_MSG_AMF0DataMessage;
    EXPECT_TRUE(h.is_amf0_data());

    h.message_type = RTMP_MSG_SetChunkSize;
    EXPECT_TRUE(h.is_set_chunk_size());

    h.message_type = RTMP_MSG_WindowAcknowledgementSize;
    EXPECT_TRUE(h.is_window_ackledgement_size());

    h.message_type = RTMP_MSG_Acknowledgement;
    EXPECT_TRUE(h.is_ackledgement());

    h.message_type = RTMP_MSG_UserControlMessage;
    EXPECT_TRUE(h.is_user_control_message());

    h.message_type = RTMP_MSG_SetPeerBandwidth;
    EXPECT_TRUE(h.is_set_peer_bandwidth());

    h.message_type = RTMP_MSG_AggregateMessage;
    EXPECT_TRUE(h.is_aggregate());
}

VOID TEST(KernelFlvTest, MessageHeaderInitializers)
{
    SrsMessageHeader h;

    h.initialize_audio(100, 3000, 1);
    EXPECT_TRUE(h.is_audio());
    EXPECT_EQ(100, h.payload_length);
    EXPECT_EQ(3000, h.timestamp);
    EXPECT_EQ(1, h.stream_id);
    EXPECT_EQ(RTMP_CID_Audio, h.prefer_cid);

    h.initialize_video(200, 4000, 1);
    EXPECT_TRUE(h.is_video());
    EXPECT_EQ(RTMP_CID_Video, h.prefer_cid);

    // onMetaData 등 amf0 script는 timestamp 0 고정.
    h.initialize_amf0_script(300, 1);
    EXPECT_TRUE(h.is_amf0_data());
    EXPECT_EQ(0, h.timestamp);
    EXPECT_EQ(RTMP_CID_OverConnection2, h.prefer_cid);
}

// 완료 조건: SharedPtrMessage refcount — copy()는 페이로드를 복사하지 않는다.
VOID TEST(KernelFlvTest, SharedPtrMessageRefCount)
{
    srs_error_t err = srs_success;

    // 수신 측 메시지를 만들고,
    SrsCommonMessage cm;
    cm.header.initialize_video(4, 1000, 1);
    cm.create_payload(4);
    cm.size = 4;
    memcpy(cm.payload, "\x17\x00\x00\x00", 4);
    char* raw = cm.payload;

    // 송신용 shared ptr로 소유권 이전.
    SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
    HELPER_ASSERT_SUCCESS(msg->create(&cm));
    // 페이로드 소유권이 넘어가 double-free가 방지된다.
    EXPECT_TRUE(cm.payload == NULL);
    EXPECT_EQ(0, cm.size);
    EXPECT_TRUE(msg->payload == raw);
    EXPECT_EQ(0, msg->count());
    EXPECT_TRUE(msg->is_video());
    EXPECT_TRUE(msg->is_av());
    EXPECT_FALSE(msg->is_audio());

    // copy()는 refcount만 증가 — 페이로드 포인터가 같다.
    SrsSharedPtrMessage* copy = msg->copy();
    EXPECT_EQ(1, msg->count());
    EXPECT_EQ(1, copy->count());
    EXPECT_TRUE(copy->payload == raw);
    EXPECT_EQ(msg->timestamp, copy->timestamp);
    EXPECT_EQ(msg->stream_id, copy->stream_id);

    SrsSharedPtrMessage* copy2 = copy->copy();
    EXPECT_EQ(2, msg->count());

    // 복사본을 지우면 count만 준다. 마지막 하나가 페이로드를 해제한다.
    srs_freep(copy);
    EXPECT_EQ(1, msg->count());
    srs_freep(copy2);
    EXPECT_EQ(0, msg->count());
    srs_freep(msg);
}

VOID TEST(KernelFlvTest, SharedPtrMessageCheck)
{
    srs_error_t err = srs_success;

    SrsMessageHeader h;
    h.initialize_video(1, 0, 1);

    SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
    HELPER_ASSERT_SUCCESS(msg->create(&h, new char[1], 1));

    // stream_id가 같으면 true — 청크 헤더를 다시 만들 필요 없음.
    EXPECT_TRUE(msg->check(1));
    // 다르면 갱신하고 false.
    EXPECT_FALSE(msg->check(2));
    EXPECT_EQ(2, msg->stream_id);
    EXPECT_TRUE(msg->check(2));

    srs_freep(msg);
}

VOID TEST(KernelFlvTest, SharedPtrMessageInvalidSize)
{
    srs_error_t err = srs_success;

    SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
    HELPER_EXPECT_FAILED(msg->create(NULL, NULL, -1));
    srs_freep(msg);
}

// 완료 조건: c0/c3 청크 헤더 직렬화 (fmt=0/3만 — CLAUDE.md §2.2)
VOID TEST(KernelFlvTest, ChunkHeaderC0)
{
    char cache[SRS_CONSTS_RTMP_MAX_FMT0_HEADER_SIZE];

    // 일반 타임스탬프: basic(1) + message header(11) = 12바이트.
    int nb = srs_chunk_header_c0(RTMP_CID_Video, 0x123456, 0x000abc, RTMP_MSG_VideoMessage, 1, cache, sizeof(cache));
    ASSERT_EQ(12, nb);
    // fmt=0 + csid
    EXPECT_EQ(0x06, (uint8_t)cache[0]);
    // timestamp 3바이트 BE
    EXPECT_EQ(0x12, (uint8_t)cache[1]);
    EXPECT_EQ(0x34, (uint8_t)cache[2]);
    EXPECT_EQ(0x56, (uint8_t)cache[3]);
    // payload_length 3바이트 BE
    EXPECT_EQ(0x00, (uint8_t)cache[4]);
    EXPECT_EQ(0x0a, (uint8_t)cache[5]);
    EXPECT_EQ(0xbc, (uint8_t)cache[6]);
    // message_type
    EXPECT_EQ(RTMP_MSG_VideoMessage, (uint8_t)cache[7]);
    // stream_id 4바이트 LE
    EXPECT_EQ(0x01, (uint8_t)cache[8]);
    EXPECT_EQ(0x00, (uint8_t)cache[9]);
    EXPECT_EQ(0x00, (uint8_t)cache[10]);
    EXPECT_EQ(0x00, (uint8_t)cache[11]);

    // extended timestamp: 3바이트 필드는 0xFFFFFF, 뒤에 4바이트 실값 = 16바이트.
    nb = srs_chunk_header_c0(RTMP_CID_Video, 0x01234567, 10, RTMP_MSG_VideoMessage, 1, cache, sizeof(cache));
    ASSERT_EQ(16, nb);
    EXPECT_EQ(0xFF, (uint8_t)cache[1]);
    EXPECT_EQ(0xFF, (uint8_t)cache[2]);
    EXPECT_EQ(0xFF, (uint8_t)cache[3]);
    EXPECT_EQ(0x01, (uint8_t)cache[12]);
    EXPECT_EQ(0x23, (uint8_t)cache[13]);
    EXPECT_EQ(0x45, (uint8_t)cache[14]);
    EXPECT_EQ(0x67, (uint8_t)cache[15]);

    // 캐시가 모자라면 0.
    EXPECT_EQ(0, srs_chunk_header_c0(RTMP_CID_Video, 0, 10, RTMP_MSG_VideoMessage, 1, cache, 11));
}

VOID TEST(KernelFlvTest, ChunkHeaderC3)
{
    char cache[SRS_CONSTS_RTMP_MAX_FMT3_HEADER_SIZE];

    // fmt=3은 basic header 1바이트뿐.
    int nb = srs_chunk_header_c3(RTMP_CID_Audio, 1000, cache, sizeof(cache));
    ASSERT_EQ(1, nb);
    EXPECT_EQ(0xC7, (uint8_t)cache[0]);

    // adobe 확장: fmt=3에도 extended timestamp를 붙인다 (스펙과 다름 — 원본 주석 참조).
    nb = srs_chunk_header_c3(RTMP_CID_Audio, 0x01234567, cache, sizeof(cache));
    ASSERT_EQ(5, nb);
    EXPECT_EQ(0xC7, (uint8_t)cache[0]);
    EXPECT_EQ(0x01, (uint8_t)cache[1]);
    EXPECT_EQ(0x23, (uint8_t)cache[2]);
    EXPECT_EQ(0x45, (uint8_t)cache[3]);
    EXPECT_EQ(0x67, (uint8_t)cache[4]);

    EXPECT_EQ(0, srs_chunk_header_c3(RTMP_CID_Audio, 0, cache, 0));
}

VOID TEST(KernelFlvTest, SharedPtrMessageChunkHeader)
{
    srs_error_t err = srs_success;

    SrsMessageHeader h;
    h.initialize_audio(16, 2000, 1);

    SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
    HELPER_ASSERT_SUCCESS(msg->create(&h, new char[16], 16));

    char cache[SRS_CONSTS_RTMP_MAX_FMT0_HEADER_SIZE];
    EXPECT_EQ(12, msg->chunk_header(cache, sizeof(cache), true));
    EXPECT_EQ(0x07, (uint8_t)cache[0]); // fmt=0, csid=RTMP_CID_Audio
    EXPECT_EQ(1, msg->chunk_header(cache, sizeof(cache), false));
    EXPECT_EQ(0xC7, (uint8_t)cache[0]); // fmt=3, csid=RTMP_CID_Audio

    srs_freep(msg);
}

// S3 — 시퀀스 헤더/키프레임 판별 (srs_kernel_codec)

// 완료 조건: 시퀀스 헤더 판별 테스트.
VOID TEST(KernelCodecTest, FlvVideoJudgers)
{
    // 0x17 = keyframe(1) | AVC(7), 0x00 = sequence header
    char sh[] = {0x17, 0x00, 0x00, 0x00, 0x00};
    EXPECT_TRUE(SrsFlvVideo::h264(sh, sizeof(sh)));
    EXPECT_TRUE(SrsFlvVideo::keyframe(sh, sizeof(sh)));
    EXPECT_TRUE(SrsFlvVideo::sh(sh, sizeof(sh)));

    // 0x17 0x01 = keyframe NALU — 키프레임이지만 시퀀스 헤더는 아님.
    char kf[] = {0x17, 0x01, 0x00, 0x00, 0x00};
    EXPECT_TRUE(SrsFlvVideo::keyframe(kf, sizeof(kf)));
    EXPECT_FALSE(SrsFlvVideo::sh(kf, sizeof(kf)));

    // 0x27 = inter frame(2) | AVC(7)
    char inter[] = {0x27, 0x01, 0x00, 0x00, 0x00};
    EXPECT_TRUE(SrsFlvVideo::h264(inter, sizeof(inter)));
    EXPECT_FALSE(SrsFlvVideo::keyframe(inter, sizeof(inter)));
    EXPECT_FALSE(SrsFlvVideo::sh(inter, sizeof(inter)));

    // 0x12 = keyframe(1) | H.263(2) — AVC가 아니면 시퀀스 헤더도 아님.
    char h263[] = {0x12, 0x00};
    EXPECT_FALSE(SrsFlvVideo::h264(h263, sizeof(h263)));
    EXPECT_FALSE(SrsFlvVideo::sh(h263, sizeof(h263)));

    // 빈/짧은 데이터.
    EXPECT_FALSE(SrsFlvVideo::keyframe(NULL, 0));
    char one[] = {0x17};
    EXPECT_FALSE(SrsFlvVideo::sh(one, 1));
}

VOID TEST(KernelCodecTest, FlvAudioJudgers)
{
    // 0xAF = AAC(10) | 44kHz stereo 16bit, 0x00 = sequence header(AudioSpecificConfig)
    char sh[] = {(char)0xAF, 0x00, 0x12, 0x10};
    EXPECT_TRUE(SrsFlvAudio::aac(sh, sizeof(sh)));
    EXPECT_TRUE(SrsFlvAudio::sh(sh, sizeof(sh)));

    // 0xAF 0x01 = AAC raw data.
    char raw[] = {(char)0xAF, 0x01, 0x00};
    EXPECT_TRUE(SrsFlvAudio::aac(raw, sizeof(raw)));
    EXPECT_FALSE(SrsFlvAudio::sh(raw, sizeof(raw)));

    // 0x2F = MP3(2) — AAC가 아님.
    char mp3[] = {0x2F, 0x00};
    EXPECT_FALSE(SrsFlvAudio::aac(mp3, sizeof(mp3)));
    EXPECT_FALSE(SrsFlvAudio::sh(mp3, sizeof(mp3)));

    EXPECT_FALSE(SrsFlvAudio::aac(NULL, 0));
    char one[] = {(char)0xAF};
    EXPECT_FALSE(SrsFlvAudio::sh(one, 1));
}
