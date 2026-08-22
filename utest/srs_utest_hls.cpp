// srs_simple — S10 HLS 경로 테스트: 코덱 파싱, TS 먹싱, fragment, m3u8.
// 원본 utest의 설계(디코더로 재검증 가능한 바이트 단위 검증)를 따른다.
#include "srs_utest.hpp"

#include <stdlib.h>
#include <unistd.h>

#include <srs_app_config.hpp>
#include <srs_app_fragment.hpp>
#include <srs_app_hls.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_file.hpp>
#include <srs_kernel_stream.hpp>
#include <srs_kernel_ts.hpp>
#include <srs_protocol_rtmp_stack.hpp>

// TS 패킷을 메모리로 받는 mock (ISrsWriter — SrsFileWriter 대신).
class MockTsWriter : public ISrsWriter
{
public:
    SrsSimpleStream data;
public:
    virtual srs_error_t write(void* buf, size_t size, ssize_t* nwrite) {
        data.append((const char*)buf, (int)size);
        if (nwrite) *nwrite = (ssize_t)size;
        return srs_success;
    }
    virtual srs_error_t writev(const iovec* iov, int iov_size, ssize_t* nwrite) {
        ssize_t total = 0;
        for (int i = 0; i < iov_size; i++) {
            data.append((const char*)iov[i].iov_base, (int)iov[i].iov_len);
            total += (ssize_t)iov[i].iov_len;
        }
        if (nwrite) *nwrite = total;
        return srs_success;
    }
};

// ---- 테스트용 FLV 페이로드 빌더 ----

// AVC 시퀀스 헤더 (FLV): 0x17 0x00 cts(3B) + avcC
static string mock_avc_sh()
{
    static const uint8_t sps[] = {0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9};
    static const uint8_t pps[] = {0x68, 0xeb, 0xec, 0xb2, 0x2c};

    string v;
    const uint8_t head[] = {0x17, 0x00, 0x00, 0x00, 0x00};
    v.append((const char*)head, sizeof(head));
    // avcC: ver, profile, compat, level, lengthSizeMinusOne=3(4B prefix)
    const uint8_t avcc[] = {0x01, 0x64, 0x00, 0x1f, 0xff, 0xe1};
    v.append((const char*)avcc, sizeof(avcc));
    v.push_back(0x00); v.push_back((char)sizeof(sps));
    v.append((const char*)sps, sizeof(sps));
    v.push_back(0x01);
    v.push_back(0x00); v.push_back((char)sizeof(pps));
    v.append((const char*)pps, sizeof(pps));
    return v;
}

// AVC NALU 프레임 (FLV): frame_type + 0x01 cts(3B) + [4B len + NALU]
static string mock_avc_frame(bool keyframe, int cts, const string& nalu)
{
    string v;
    v.push_back(keyframe ? 0x17 : 0x27);
    v.push_back(0x01);
    v.push_back((char)((cts >> 16) & 0xff));
    v.push_back((char)((cts >> 8) & 0xff));
    v.push_back((char)(cts & 0xff));
    uint32_t len = (uint32_t)nalu.size();
    v.push_back((char)((len >> 24) & 0xff));
    v.push_back((char)((len >> 16) & 0xff));
    v.push_back((char)((len >> 8) & 0xff));
    v.push_back((char)(len & 0xff));
    v.append(nalu);
    return v;
}

// AAC 시퀀스 헤더 (FLV): 0xaf 0x00 + AudioSpecificConfig(LC, 44.1kHz, stereo)
static string mock_aac_sh()
{
    const uint8_t v[] = {0xaf, 0x00, 0x12, 0x10};
    return string((const char*)v, sizeof(v));
}

// AAC raw 프레임 (FLV): 0xaf 0x01 + raw
static string mock_aac_raw(const string& raw)
{
    string v;
    v.push_back((char)0xaf);
    v.push_back(0x01);
    v.append(raw);
    return v;
}

// ---- CRC32 ----

TEST(KernelTsTest, Crc32MpegTs)
{
    // CRC-32/MPEG-2 표준 검증 벡터.
    EXPECT_EQ(0x0376e6e7u, srs_crc32_mpegts("123456789", 9));
}

// ---- SrsFormat: avcC/ASC 파싱 ----

TEST(KernelCodecTest, FormatAvcDemuxSpsPps)
{
    srs_error_t err = srs_success;

    SrsFormat f;
    HELPER_ASSERT_SUCCESS(f.initialize());

    string sh = mock_avc_sh();
    HELPER_ASSERT_SUCCESS(f.on_video(0, (char*)sh.data(), (int)sh.size()));

    ASSERT_TRUE(f.vcodec != NULL);
    EXPECT_EQ(SrsVideoCodecIdAVC, f.vcodec->id);
    EXPECT_EQ(SrsVideoAvcFrameTraitSequenceHeader, f.video->avc_packet_type);
    EXPECT_EQ(3, f.vcodec->NAL_unit_length);
    ASSERT_EQ(6, (int)f.vcodec->sequenceParameterSetNALUnit.size());
    EXPECT_EQ(0x67, (uint8_t)f.vcodec->sequenceParameterSetNALUnit[0]);
    ASSERT_EQ(5, (int)f.vcodec->pictureParameterSetNALUnit.size());
    EXPECT_EQ(0x68, (uint8_t)f.vcodec->pictureParameterSetNALUnit[0]);
    EXPECT_TRUE(f.vcodec->is_avc_codec_ok());
}

TEST(KernelCodecTest, FormatAvcDemuxSpsResolution)
{
    srs_error_t err = srs_success;

    // 실제 x264 SPS (320x240, high profile) — emulation bytes(00 00 03) 포함 (S16).
    static const uint8_t sps[] = {
        0x67, 0x64, 0x00, 0x0d, 0xac, 0xd9, 0x41, 0x41, 0xfb, 0x01, 0x10, 0x00, 0x00,
        0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xc0, 0xf1, 0x42, 0x99, 0x60,
    };
    static const uint8_t pps[] = {0x68, 0xeb, 0xec, 0xb2, 0x2c};

    string sh;
    const uint8_t head[] = {0x17, 0x00, 0x00, 0x00, 0x00};
    sh.append((const char*)head, sizeof(head));
    const uint8_t avcc[] = {0x01, 0x64, 0x00, 0x0d, 0xff, 0xe1};
    sh.append((const char*)avcc, sizeof(avcc));
    sh.push_back(0x00); sh.push_back((char)sizeof(sps));
    sh.append((const char*)sps, sizeof(sps));
    sh.push_back(0x01);
    sh.push_back(0x00); sh.push_back((char)sizeof(pps));
    sh.append((const char*)pps, sizeof(pps));

    SrsFormat f;
    HELPER_ASSERT_SUCCESS(f.initialize());
    HELPER_ASSERT_SUCCESS(f.on_video(0, (char*)sh.data(), (int)sh.size()));

    ASSERT_TRUE(f.vcodec != NULL);
    EXPECT_EQ(320, f.vcodec->width);
    EXPECT_EQ(240, f.vcodec->height);

    // 파싱 불가한(잘린) SPS는 best-effort — 에러 없이 해상도만 0으로 남는다 (§5.6 S16).
    SrsFormat f2;
    HELPER_ASSERT_SUCCESS(f2.initialize());
    string sh2 = mock_avc_sh();
    HELPER_ASSERT_SUCCESS(f2.on_video(0, (char*)sh2.data(), (int)sh2.size()));
    EXPECT_EQ(0, f2.vcodec->width);
    EXPECT_EQ(0, f2.vcodec->height);
}

TEST(KernelCodecTest, FormatAvcDemuxNalus)
{
    srs_error_t err = srs_success;

    SrsFormat f;
    string sh = mock_avc_sh();
    HELPER_ASSERT_SUCCESS(f.on_video(0, (char*)sh.data(), (int)sh.size()));

    // IDR 프레임, cts=40ms
    string idr("\x65\x88\x84\x00\x33\xff", 6);
    string frame = mock_avc_frame(true, 40, idr);
    HELPER_ASSERT_SUCCESS(f.on_video(100, (char*)frame.data(), (int)frame.size()));

    EXPECT_EQ(SrsVideoAvcFrameTypeKeyFrame, f.video->frame_type);
    EXPECT_EQ(SrsVideoAvcFrameTraitNALU, f.video->avc_packet_type);
    EXPECT_EQ(40, f.video->cts);
    EXPECT_TRUE(f.video->has_idr);
    ASSERT_EQ(1, f.video->nb_samples);
    EXPECT_EQ(6, f.video->samples[0].size);
    EXPECT_EQ(0x65, (uint8_t)f.video->samples[0].bytes[0]);
}

TEST(KernelCodecTest, FormatAacDemuxSequenceHeader)
{
    srs_error_t err = srs_success;

    SrsFormat f;
    string sh = mock_aac_sh();
    HELPER_ASSERT_SUCCESS(f.on_audio(0, (char*)sh.data(), (int)sh.size()));

    ASSERT_TRUE(f.acodec != NULL);
    EXPECT_EQ(SrsAudioCodecIdAAC, f.acodec->id);
    EXPECT_EQ(SrsAudioAacFrameTraitSequenceHeader, f.audio->aac_packet_type);
    EXPECT_EQ(SrsAacObjectTypeAacLC, f.acodec->aac_object);
    EXPECT_EQ(4, f.acodec->aac_sample_rate);   // 44.1kHz
    EXPECT_EQ(2, f.acodec->aac_channels);      // stereo
    EXPECT_EQ(44100, srs_aac_srates[f.acodec->aac_sample_rate]);

    // raw 프레임은 샘플 1개.
    string raw = mock_aac_raw(string(64, (char)0xaa));
    HELPER_ASSERT_SUCCESS(f.on_audio(23, (char*)raw.data(), (int)raw.size()));
    EXPECT_EQ(SrsAudioAacFrameTraitRawData, f.audio->aac_packet_type);
    ASSERT_EQ(1, f.audio->nb_samples);
    EXPECT_EQ(64, f.audio->samples[0].size);
}

// ---- SrsTsMessageCache: ADTS/annex-b 변환 ----

TEST(KernelTsTest, CacheAacWithAdts)
{
    srs_error_t err = srs_success;

    SrsFormat f;
    string sh = mock_aac_sh();
    HELPER_ASSERT_SUCCESS(f.on_audio(0, (char*)sh.data(), (int)sh.size()));
    string raw = mock_aac_raw(string(100, (char)0xaa));
    HELPER_ASSERT_SUCCESS(f.on_audio(23, (char*)raw.data(), (int)raw.size()));

    SrsTsMessageCache tsmc;
    HELPER_ASSERT_SUCCESS(tsmc.cache_audio(f.audio, 23 * 90));

    ASSERT_TRUE(tsmc.audio != NULL);
    EXPECT_EQ(23 * 90, tsmc.audio->dts);
    EXPECT_EQ(tsmc.audio->dts, tsmc.audio->pts);
    EXPECT_TRUE(tsmc.audio->is_audio());

    // payload = ADTS(7) + raw(100)
    ASSERT_EQ(107, tsmc.audio->payload->length());
    uint8_t* p = (uint8_t*)tsmc.audio->payload->bytes();
    EXPECT_EQ(0xff, p[0]);
    EXPECT_EQ(0xf9, p[1]);
    // profile LC(1)<<6 | rate_idx(4)<<2 | channels(2)>>2 = 0x50
    EXPECT_EQ(0x50, p[2]);
    // channels(2)<<6 | frame_length(107)>>11 = 0x80
    EXPECT_EQ(0x80, p[3]);
    // frame_length 검증: 13비트 = byte3[1:0] byte4 byte5[7:5]
    int frame_length = ((p[3] & 0x03) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x07);
    EXPECT_EQ(107, frame_length);
}

TEST(KernelTsTest, CacheAvcAnnexb)
{
    srs_error_t err = srs_success;

    SrsFormat f;
    string sh = mock_avc_sh();
    HELPER_ASSERT_SUCCESS(f.on_video(0, (char*)sh.data(), (int)sh.size()));
    string idr("\x65\x88\x84\x00\x33\xff", 6);
    string frame = mock_avc_frame(true, 40, idr);
    HELPER_ASSERT_SUCCESS(f.on_video(100, (char*)frame.data(), (int)frame.size()));

    SrsTsMessageCache tsmc;
    HELPER_ASSERT_SUCCESS(tsmc.cache_video(f.video, 100 * 90));

    ASSERT_TRUE(tsmc.video != NULL);
    EXPECT_EQ(100 * 90, tsmc.video->dts);
    // pts = dts + cts*90
    EXPECT_EQ(100 * 90 + 40 * 90, tsmc.video->pts);
    EXPECT_TRUE(tsmc.video->write_pcr);
    EXPECT_TRUE(tsmc.video->is_video());

    // payload = [00 00 00 01] 09 f0 (AUD)
    //         + [00 00 01] SPS(6) + [00 00 01] PPS(5) + [00 00 01] IDR(6)
    uint8_t* p = (uint8_t*)tsmc.video->payload->bytes();
    int size = tsmc.video->payload->length();
    ASSERT_EQ(4 + 2 + 3 + 6 + 3 + 5 + 3 + 6, size);
    EXPECT_EQ(0x00, p[0]); EXPECT_EQ(0x00, p[1]); EXPECT_EQ(0x00, p[2]); EXPECT_EQ(0x01, p[3]);
    EXPECT_EQ(0x09, p[4]); EXPECT_EQ(0xf0, p[5]);
    // SPS
    EXPECT_EQ(0x00, p[6]); EXPECT_EQ(0x00, p[7]); EXPECT_EQ(0x01, p[8]);
    EXPECT_EQ(0x67, p[9]);
    // PPS
    EXPECT_EQ(0x01, p[17]);
    EXPECT_EQ(0x68, p[18]);
    // IDR
    EXPECT_EQ(0x01, p[25]);
    EXPECT_EQ(0x65, p[26]);
}

// ---- SrsTsContext: PAT/PMT/PES 직렬화 ----

TEST(KernelTsTest, EncodePatPmtPes)
{
    srs_error_t err = srs_success;

    // 100바이트짜리 오디오 PES 하나를 인코딩.
    SrsTsMessage msg;
    msg.dts = msg.pts = 90000;
    msg.sid = SrsTsPESStreamIdAudioCommon;
    string payload(100, (char)0xab);
    msg.payload->append(payload.data(), (int)payload.size());

    MockTsWriter w;
    SrsTsContext ctx;
    HELPER_ASSERT_SUCCESS(ctx.encode(&w, &msg, SrsVideoCodecIdAVC, SrsAudioCodecIdAAC));

    // PAT + PMT + PES 1개 = 188*3
    ASSERT_EQ(SRS_TS_PACKET_SIZE * 3, w.data.length());
    uint8_t* p = (uint8_t*)w.data.bytes();

    // ---- PAT ----
    EXPECT_EQ(0x47, p[0]);
    EXPECT_EQ(0x40, p[1]);                  // PUSI=1, pid=0
    EXPECT_EQ(0x00, p[2]);
    EXPECT_EQ(0x10, p[3]);                  // payload only, cc=0
    EXPECT_EQ(0x00, p[4]);                  // pointer
    EXPECT_EQ(0x00, p[5]);                  // table_id PAT
    // section: p[5]부터 3+13바이트, CRC 재계산 일치.
    int pat_section_len = ((p[6] & 0x0f) << 8) | p[7];
    EXPECT_EQ(13, pat_section_len);
    uint32_t crc = srs_crc32_mpegts(p + 5, 3 + pat_section_len - 4);
    uint32_t stored = (p[5 + 3 + 13 - 4] << 24) | (p[5 + 3 + 13 - 3] << 16) | (p[5 + 3 + 13 - 2] << 8) | p[5 + 3 + 13 - 1];
    EXPECT_EQ(crc, stored);
    // program → PMT pid
    EXPECT_EQ(TS_PMT_PID, ((p[15] & 0x1f) << 8) | p[16]);

    // ---- PMT ----
    uint8_t* q = p + SRS_TS_PACKET_SIZE;
    EXPECT_EQ(0x47, q[0]);
    EXPECT_EQ(TS_PMT_PID, ((q[1] & 0x1f) << 8) | q[2]);
    EXPECT_EQ(0x02, q[5]);                  // table_id PMT
    int pmt_section_len = ((q[6] & 0x0f) << 8) | q[7];
    EXPECT_EQ(9 + 5 * 2 + 4, pmt_section_len); // ES 2개(video+audio)
    crc = srs_crc32_mpegts(q + 5, 3 + pmt_section_len - 4);
    uint8_t* qc = q + 5 + 3 + pmt_section_len - 4;
    stored = (qc[0] << 24) | (qc[1] << 16) | (qc[2] << 8) | qc[3];
    EXPECT_EQ(crc, stored);
    // PCR_PID = video pid
    EXPECT_EQ(TS_VIDEO_AVC_PID, ((q[13] & 0x1f) << 8) | q[14]);
    // ES: H.264 + AAC
    EXPECT_EQ(SrsTsStreamVideoH264, q[17]);
    EXPECT_EQ(TS_VIDEO_AVC_PID, ((q[18] & 0x1f) << 8) | q[19]);
    EXPECT_EQ(SrsTsStreamAudioAAC, q[22]);
    EXPECT_EQ(TS_AUDIO_AAC_PID, ((q[23] & 0x1f) << 8) | q[24]);

    // ---- PES (오디오, 스터핑 있는 1패킷) ----
    uint8_t* r = p + SRS_TS_PACKET_SIZE * 2;
    EXPECT_EQ(0x47, r[0]);
    EXPECT_EQ(0x40 | ((TS_AUDIO_AAC_PID >> 8) & 0x1f), r[1]); // PUSI=1
    EXPECT_EQ(TS_AUDIO_AAC_PID & 0xff, r[2]);
    EXPECT_EQ(0x30, r[3] & 0xf0);           // AF+payload, cc=0
    // PES 총량 = 9 + 5 + 100 = 114 → AF 크기 = 184-114 = 70 (af_len=69)
    EXPECT_EQ(69, r[4]);
    EXPECT_EQ(0x00, r[5]);                  // flags: 스터핑만
    EXPECT_EQ(0xff, r[6]);                  // stuffing
    // PES 헤더는 AF 뒤에서 시작.
    uint8_t* pes = r + 4 + 70;
    EXPECT_EQ(0x00, pes[0]); EXPECT_EQ(0x00, pes[1]); EXPECT_EQ(0x01, pes[2]);
    EXPECT_EQ(0xc0, pes[3]);                // audio stream id
    // PES_packet_length = 3 + 5 + 100 = 108
    EXPECT_EQ(108, (pes[4] << 8) | pes[5]);
    EXPECT_EQ(0x80, pes[6]);
    EXPECT_EQ(0x80, pes[7]);                // PTS only
    EXPECT_EQ(5, pes[8]);
    // PTS 디코드 = 90000
    int64_t pts = ((int64_t)((pes[9] >> 1) & 0x07) << 30)
        | ((int64_t)pes[10] << 22) | ((int64_t)((pes[11] >> 1) & 0x7f) << 15)
        | ((int64_t)pes[12] << 7) | ((pes[13] >> 1) & 0x7f);
    EXPECT_EQ(90000, pts);
    // payload 시작.
    EXPECT_EQ(0xab, pes[14]);
}

TEST(KernelTsTest, EncodePesLargeVideoWithPcr)
{
    srs_error_t err = srs_success;

    SrsTsMessage msg;
    msg.dts = 90000;
    msg.pts = 90000 + 3600;                 // cts 40ms
    msg.sid = SrsTsPESStreamIdVideoCommon;
    msg.write_pcr = true;
    string payload(1000, (char)0xcd);
    msg.payload->append(payload.data(), (int)payload.size());

    MockTsWriter w;
    SrsTsContext ctx;
    HELPER_ASSERT_SUCCESS(ctx.encode(&w, &msg, SrsVideoCodecIdAVC, SrsAudioCodecIdAAC));

    ASSERT_EQ(0, w.data.length() % SRS_TS_PACKET_SIZE);
    int nb_pkts = w.data.length() / SRS_TS_PACKET_SIZE;
    ASSERT_GE(nb_pkts, 3 + 2);              // PAT+PMT + PES 여러 개
    uint8_t* p = (uint8_t*)w.data.bytes();

    // 첫 PES 패킷: PCR 있는 AF.
    uint8_t* r = p + SRS_TS_PACKET_SIZE * 2;
    EXPECT_EQ(0x47, r[0]);
    EXPECT_EQ(0x40 | ((TS_VIDEO_AVC_PID >> 8) & 0x1f), r[1]);
    EXPECT_EQ(0x30, r[3] & 0xf0);           // AF+payload
    EXPECT_EQ(7, r[4]);                     // af_len = flags(1)+PCR(6)
    EXPECT_EQ(0x50, r[5]);                  // random_access + PCR_flag
    // PCR base 디코드 = dts.
    int64_t pcr_base = ((int64_t)r[6] << 25) | ((int64_t)r[7] << 17)
        | ((int64_t)r[8] << 9) | ((int64_t)r[9] << 1) | ((r[10] >> 7) & 0x01);
    EXPECT_EQ(90000, pcr_base);
    // PES: PTS+DTS.
    uint8_t* pes = r + 4 + 8;
    EXPECT_EQ(0x01, pes[2]);
    EXPECT_EQ(0xe0, pes[3]);                // video stream id
    EXPECT_EQ(0xc0, pes[7]);                // PTS+DTS
    EXPECT_EQ(10, pes[8]);

    // 이어지는 패킷들: PUSI=0, continuity counter 증가.
    uint8_t* r2 = p + SRS_TS_PACKET_SIZE * 3;
    EXPECT_EQ(0x47, r2[0]);
    EXPECT_EQ(0x00 | ((TS_VIDEO_AVC_PID >> 8) & 0x1f), r2[1]);
    EXPECT_EQ((r[3] & 0x0f) + 1, r2[3] & 0x0f);

    // 모든 패킷은 0x47로 시작.
    for (int i = 0; i < nb_pkts; i++) {
        EXPECT_EQ(0x47, p[i * SRS_TS_PACKET_SIZE]);
    }
}

// ---- SrsFragment / SrsFragmentWindow ----

TEST(AppFragmentTest, FragmentDuration)
{
    SrsFragment f;
    EXPECT_EQ(0, f.duration());

    f.append(0);
    f.append(100);
    f.append(200);
    EXPECT_EQ(200 * SRS_UTIME_MILLISECONDS, f.duration());

    // dts가 흔들려도 최소 dts 기준.
    f.append(150);
    EXPECT_EQ(150 * SRS_UTIME_MILLISECONDS, f.duration());
}

TEST(AppFragmentTest, WindowShrink)
{
    SrsFragmentWindow w;
    for (int i = 0; i < 5; i++) {
        SrsFragment* f = new SrsFragment();
        f->append(0);
        f->append(10000); // 10s
        w.append(f);
    }
    EXPECT_EQ(5, w.size());
    EXPECT_EQ(10 * SRS_UTIME_SECONDS, w.max_duration());

    // 윈도우 30초 → 뒤에서 4개(40s>30s)째에서 멈추고 그 앞 1개 만료.
    w.shrink(30 * SRS_UTIME_SECONDS);
    EXPECT_EQ(4, w.size());
    EXPECT_EQ(1, (int)w.expired_fragments.size());

    w.clear_expired(false);
    EXPECT_EQ(0, (int)w.expired_fragments.size());
}

// ---- SrsHlsMuxer/Controller: 세그먼트와 m3u8 (실제 파일) ----

TEST(AppHlsTest, MuxerSegmentAndM3u8)
{
    srs_error_t err = srs_success;

    // 임시 디렉터리로 hls_path를 돌려놓는다.
    char tmpl[] = "/tmp/srs_utest_hls_XXXXXX";
    char* tmpdir = ::mkdtemp(tmpl);
    ASSERT_TRUE(tmpdir != NULL);

    SrsSimpleConfig saved = *_srs_config;
    _srs_config->hls_path = tmpdir;
    _srs_config->hls_fragment = 500 * SRS_UTIME_MILLISECONDS;
    _srs_config->hls_window = 60 * SRS_UTIME_SECONDS;

    SrsRequest req;
    req.vhost = "__defaultVhost__";
    req.app = "live";
    req.stream = "livestream";

    SrsFormat f;
    string sh = mock_avc_sh();
    HELPER_ASSERT_SUCCESS(f.on_video(0, (char*)sh.data(), (int)sh.size()));

    SrsHlsController controller;
    HELPER_ASSERT_SUCCESS(controller.initialize());
    HELPER_ASSERT_SUCCESS(controller.on_publish(&req));

    // 키프레임 0/600/1200ms, 사이 40ms 간격 인터프레임 → fragment 500ms에서 컷 2회.
    // 1200ms 이후 1600ms까지 이어서, 마지막 세그먼트가 최소 duration(100ms)을 넘게 한다
    // (원본 do_segment_close는 100ms 미만 세그먼트를 드롭한다).
    string idr("\x65\x88\x84\x00\x33\xff", 6);
    string p_nalu("\x41\x9a\x00\x33", 4);
    for (int dts = 0; dts <= 1600; dts += 40) {
        bool keyframe = (dts % 600) == 0;
        string frame = mock_avc_frame(keyframe, 0, keyframe ? idr : p_nalu);
        HELPER_ASSERT_SUCCESS(f.on_video(dts, (char*)frame.data(), (int)frame.size()));
        HELPER_ASSERT_SUCCESS(controller.write_video(f.video, (int64_t)dts * 90));
    }
    HELPER_ASSERT_SUCCESS(controller.on_unpublish());

    // 세그먼트 파일: 0/1/2번이 만들어져야 한다 (600ms 컷 2회 + 마지막 close).
    string dir = string(tmpdir) + "/live";
    EXPECT_TRUE(srs_path_exists(dir + "/livestream-0.ts"));
    EXPECT_TRUE(srs_path_exists(dir + "/livestream-1.ts"));
    EXPECT_TRUE(srs_path_exists(dir + "/livestream-2.ts"));
    EXPECT_TRUE(srs_path_exists(dir + "/livestream.m3u8"));

    // ts 파일은 188 정렬 + 0x47 sync + PAT로 시작.
    SrsFileReader fr;
    HELPER_ASSERT_SUCCESS(fr.open(dir + "/livestream-0.ts"));
    int64_t size = fr.filesize();
    EXPECT_GT(size, 0);
    EXPECT_EQ(0, size % SRS_TS_PACKET_SIZE);
    char pkt[SRS_TS_PACKET_SIZE];
    ssize_t nread = 0;
    HELPER_ASSERT_SUCCESS(fr.read(pkt, sizeof(pkt), &nread));
    ASSERT_EQ(SRS_TS_PACKET_SIZE, (int)nread);
    EXPECT_EQ(0x47, (uint8_t)pkt[0]);
    EXPECT_EQ(0x40, (uint8_t)pkt[1]);       // PAT (pid 0, PUSI)
    fr.close();

    // m3u8 내용 검증.
    SrsFileReader mr;
    HELPER_ASSERT_SUCCESS(mr.open(dir + "/livestream.m3u8"));
    char buf[4096];
    nread = 0;
    HELPER_ASSERT_SUCCESS(mr.read(buf, sizeof(buf) - 1, &nread));
    buf[nread] = '\0';
    string m3u8(buf);
    EXPECT_NE(string::npos, m3u8.find("#EXTM3U"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-VERSION:3"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-MEDIA-SEQUENCE:0"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-TARGETDURATION:"));
    EXPECT_NE(string::npos, m3u8.find("#EXTINF:0.600"));
    EXPECT_NE(string::npos, m3u8.find("livestream-0.ts"));
    EXPECT_NE(string::npos, m3u8.find("livestream-2.ts"));

    *_srs_config = saved;
}
