// srs_simple — S12 fMP4 커널 먹서 테스트: 박스 구조 라운드트립, init.mp4, m4s(moof+mdat).
// 원본 utest의 설계(바이트 단위 검증)를 따른다. 박스 워커로 size 체인이 실제 바이트 수와
// 일치하는지(=라운드트립)와 필드 값을 함께 검증한다.
#include "srs_utest.hpp"

#include <srs_kernel_codec.hpp>
#include <srs_kernel_io.hpp>
#include <srs_kernel_mp4.hpp>

// fMP4 바이트를 메모리로 받는 mock (ISrsWriter).
class MockMp4Writer : public ISrsWriter
{
public:
    string data;
public:
    virtual srs_error_t write(void* buf, size_t size, ssize_t* nwrite) {
        data.append((const char*)buf, size);
        if (nwrite) *nwrite = (ssize_t)size;
        return srs_success;
    }
    virtual srs_error_t writev(const iovec* iov, int iov_size, ssize_t* nwrite) {
        ssize_t total = 0;
        for (int i = 0; i < iov_size; i++) {
            data.append((const char*)iov[i].iov_base, iov[i].iov_len);
            total += (ssize_t)iov[i].iov_len;
        }
        if (nwrite) *nwrite = total;
        return srs_success;
    }
};

// ---- 테스트용 FLV 페이로드 빌더 (srs_utest_hls.cpp와 동일) ----

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

// AAC 시퀀스 헤더 (FLV): 0xaf 0x00 + AudioSpecificConfig(LC, 44.1kHz, stereo)
static string mock_aac_sh()
{
    const uint8_t v[] = {0xaf, 0x00, 0x12, 0x10};
    return string((const char*)v, sizeof(v));
}

// ---- 박스 워커: size 체인을 따라가며 [ofs, limit) 형제 나열에서 nth번째 type을 찾는다 ----

static uint32_t mp4_u32(const string& d, int ofs)
{
    return ((uint32_t)(uint8_t)d[ofs] << 24) | ((uint32_t)(uint8_t)d[ofs + 1] << 16)
        | ((uint32_t)(uint8_t)d[ofs + 2] << 8) | (uint32_t)(uint8_t)d[ofs + 3];
}

static uint64_t mp4_u64(const string& d, int ofs)
{
    return ((uint64_t)mp4_u32(d, ofs) << 32) | mp4_u32(d, ofs + 4);
}

// 실패 시 -1. 성공 시 박스 시작 오프셋을 반환하고 *psize에 박스 전체 크기.
// size 필드가 깨졌으면(0이거나 범위 초과) -1 — 이것이 라운드트립 검증을 겸한다.
static int mp4_find_box(const string& d, int ofs, int limit, const string& type, int nth, uint32_t* psize)
{
    int p = ofs;
    while (p + 8 <= limit) {
        uint32_t size = mp4_u32(d, p);
        if (size < 8 || p + (int)size > limit) {
            return -1;
        }
        if (d.compare(p + 4, 4, type) == 0 && nth-- == 0) {
            if (psize) *psize = size;
            return p;
        }
        p += (int)size;
    }
    return -1;
}

// 최상위(파일 전체)에서 탐색.
static int mp4_find_root(const string& d, const string& type, int nth, uint32_t* psize)
{
    return mp4_find_box(d, 0, (int)d.size(), type, nth, psize);
}

// ---- init.mp4 ----

TEST(KernelMp4Test, InitEncoderMuxed)
{
    srs_error_t err = srs_success;

    SrsFormat f;
    string vsh = mock_avc_sh();
    string ash = mock_aac_sh();
    HELPER_ASSERT_SUCCESS(f.on_video(0, (char*)vsh.data(), (int)vsh.size()));
    HELPER_ASSERT_SUCCESS(f.on_audio(0, (char*)ash.data(), (int)ash.size()));

    MockMp4Writer w;
    SrsMp4M2tsInitEncoder enc;
    HELPER_ASSERT_SUCCESS(enc.initialize(&w));
    HELPER_ASSERT_SUCCESS(enc.write(&f));

    const string& d = w.data;

    // ftyp: major iso5.
    uint32_t ftyp_size = 0;
    int ftyp = mp4_find_root(d, "ftyp", 0, &ftyp_size);
    ASSERT_EQ(0, ftyp);
    EXPECT_EQ(0, d.compare(ftyp + 8, 4, "iso5"));

    // 최상위는 ftyp + moov 두 박스로 파일을 정확히 채운다 (size 체인 라운드트립).
    uint32_t moov_size = 0;
    int moov = mp4_find_root(d, "moov", 0, &moov_size);
    ASSERT_EQ((int)ftyp_size, moov);
    EXPECT_EQ((int)d.size(), moov + (int)moov_size);

    // mvhd: timescale 1000, next_track_ID 3 (video 1 + audio 2).
    uint32_t mvhd_size = 0;
    int mvhd = mp4_find_box(d, moov + 8, moov + (int)moov_size, "mvhd", 0, &mvhd_size);
    ASSERT_NE(-1, mvhd);
    EXPECT_EQ(1000u, mp4_u32(d, mvhd + 8 + 4 + 8)); // v/f(4) ctime/mtime(8) 뒤 timescale
    EXPECT_EQ(3u, mp4_u32(d, mvhd + (int)mvhd_size - 4)); // 마지막 4B = next_track_ID

    // trak ×2: video(tid=1, hdlr 'vide', avc1+avcC), audio(tid=2, hdlr 'soun', mp4a+esds).
    uint32_t vtrak_size = 0;
    int vtrak = mp4_find_box(d, moov + 8, moov + (int)moov_size, "trak", 0, &vtrak_size);
    ASSERT_NE(-1, vtrak);
    uint32_t atrak_size = 0;
    int atrak = mp4_find_box(d, moov + 8, moov + (int)moov_size, "trak", 1, &atrak_size);
    ASSERT_NE(-1, atrak);

    // video tkhd: track_ID = 1 (v/f 4B + ctime/mtime 8B 뒤).
    uint32_t tkhd_size = 0;
    int vtkhd = mp4_find_box(d, vtrak + 8, vtrak + (int)vtrak_size, "tkhd", 0, &tkhd_size);
    ASSERT_NE(-1, vtkhd);
    EXPECT_EQ(1u, mp4_u32(d, vtkhd + 8 + 4 + 8));
    int atkhd = mp4_find_box(d, atrak + 8, atrak + (int)atrak_size, "tkhd", 0, &tkhd_size);
    ASSERT_NE(-1, atkhd);
    EXPECT_EQ(2u, mp4_u32(d, atkhd + 8 + 4 + 8));

    // video trak 안의 avcC: 내용이 시퀀스 헤더의 avcC 원문과 일치해야 한다.
    // (stsd > avc1 > avcC 중첩을 따라간다)
    uint32_t mdia_size = 0;
    int mdia = mp4_find_box(d, vtrak + 8, vtrak + (int)vtrak_size, "mdia", 0, &mdia_size);
    ASSERT_NE(-1, mdia);
    uint32_t hdlr_size = 0;
    int hdlr = mp4_find_box(d, mdia + 8, mdia + (int)mdia_size, "hdlr", 0, &hdlr_size);
    ASSERT_NE(-1, hdlr);
    EXPECT_EQ(0, d.compare(hdlr + 8 + 4 + 4, 4, "vide")); // v/f + pre_defined 뒤 handler_type
    uint32_t minf_size = 0;
    int minf = mp4_find_box(d, mdia + 8, mdia + (int)mdia_size, "minf", 0, &minf_size);
    ASSERT_NE(-1, minf);
    uint32_t stbl_size = 0;
    int stbl = mp4_find_box(d, minf + 8, minf + (int)minf_size, "stbl", 0, &stbl_size);
    ASSERT_NE(-1, stbl);
    uint32_t stsd_size = 0;
    int stsd = mp4_find_box(d, stbl + 8, stbl + (int)stbl_size, "stsd", 0, &stsd_size);
    ASSERT_NE(-1, stsd);
    // stsd: full box(v/f 4B) + entry_count(4B) 뒤가 avc1.
    uint32_t avc1_size = 0;
    int avc1 = mp4_find_box(d, stsd + 8 + 4 + 4, stsd + (int)stsd_size, "avc1", 0, &avc1_size);
    ASSERT_NE(-1, avc1);
    // avc1: 프리앰블 8 + SampleEntry 8 + VisualSampleEntry 70 = 86 뒤가 avcC.
    uint32_t avcC_size = 0;
    int avcC = mp4_find_box(d, avc1 + 86, avc1 + (int)avc1_size, "avcC", 0, &avcC_size);
    ASSERT_NE(-1, avcC);
    ASSERT_EQ(f.vcodec->avc_extra_data.size(), (size_t)avcC_size - 8);
    EXPECT_EQ(0, memcmp(&f.vcodec->avc_extra_data[0], d.data() + avcC + 8, avcC_size - 8));

    // audio trak 안의 esds: ASC(0x12 0x10) 포함, mp4a의 channel/samplerate.
    mdia = mp4_find_box(d, atrak + 8, atrak + (int)atrak_size, "mdia", 0, &mdia_size);
    ASSERT_NE(-1, mdia);
    hdlr = mp4_find_box(d, mdia + 8, mdia + (int)mdia_size, "hdlr", 0, &hdlr_size);
    ASSERT_NE(-1, hdlr);
    EXPECT_EQ(0, d.compare(hdlr + 8 + 4 + 4, 4, "soun"));
    minf = mp4_find_box(d, mdia + 8, mdia + (int)mdia_size, "minf", 0, &minf_size);
    ASSERT_NE(-1, minf);
    stbl = mp4_find_box(d, minf + 8, minf + (int)minf_size, "stbl", 0, &stbl_size);
    ASSERT_NE(-1, stbl);
    stsd = mp4_find_box(d, stbl + 8, stbl + (int)stbl_size, "stsd", 0, &stsd_size);
    ASSERT_NE(-1, stsd);
    uint32_t mp4a_size = 0;
    int mp4a = mp4_find_box(d, stsd + 8 + 4 + 4, stsd + (int)stsd_size, "mp4a", 0, &mp4a_size);
    ASSERT_NE(-1, mp4a);
    // mp4a: 프리앰블 8 + SampleEntry 8 + reserved 8 뒤 channelcount(2B), samplesize(2B),
    // pre_defined/reserved(4B), samplerate(4B, 16.16).
    EXPECT_EQ(2, (int)(uint8_t)d[mp4a + 24 + 1]); // channelcount = 2 (stereo)
    EXPECT_EQ(16, (int)(uint8_t)d[mp4a + 26 + 1]); // samplesize = 16
    EXPECT_EQ(44100u, mp4_u32(d, mp4a + 32) >> 16); // samplerate = 44100
    // mp4a: 위 필드까지 36바이트 뒤가 esds.
    uint32_t esds_size = 0;
    int esds = mp4_find_box(d, mp4a + 36, mp4a + (int)mp4a_size, "esds", 0, &esds_size);
    ASSERT_NE(-1, esds);
    // esds 안 어딘가에 DecSpecificInfo(tag 0x05, len 2, ASC 0x12 0x10)가 있다.
    string dsi("\x05\x02\x12\x10", 4);
    EXPECT_NE(string::npos, d.find(dsi, esds));

    // mvex: trex ×2, track_ID 1과 2.
    uint32_t mvex_size = 0;
    int mvex = mp4_find_box(d, moov + 8, moov + (int)moov_size, "mvex", 0, &mvex_size);
    ASSERT_NE(-1, mvex);
    uint32_t trex_size = 0;
    int trex0 = mp4_find_box(d, mvex + 8, mvex + (int)mvex_size, "trex", 0, &trex_size);
    ASSERT_NE(-1, trex0);
    EXPECT_EQ(1u, mp4_u32(d, trex0 + 12)); // track_ID
    EXPECT_EQ(1u, mp4_u32(d, trex0 + 16)); // default_sample_description_index
    int trex1 = mp4_find_box(d, mvex + 8, mvex + (int)mvex_size, "trex", 1, &trex_size);
    ASSERT_NE(-1, trex1);
    EXPECT_EQ(2u, mp4_u32(d, trex1 + 12));

    // 빈 샘플 테이블: stts entry_count = 0.
    uint32_t stts_size = 0;
    int stts = mp4_find_box(d, stbl + 8, stbl + (int)stbl_size, "stts", 0, &stts_size);
    ASSERT_NE(-1, stts);
    EXPECT_EQ(16u, stts_size);
    EXPECT_EQ(0u, mp4_u32(d, stts + 12));
}

TEST(KernelMp4Test, InitEncoderSingleTrack)
{
    srs_error_t err = srs_success;

    SrsFormat f;
    string vsh = mock_avc_sh();
    HELPER_ASSERT_SUCCESS(f.on_video(0, (char*)vsh.data(), (int)vsh.size()));

    MockMp4Writer w;
    SrsMp4M2tsInitEncoder enc;
    HELPER_ASSERT_SUCCESS(enc.initialize(&w));
    // 원본 시그니처: video 트랙 하나, tid=1.
    HELPER_ASSERT_SUCCESS(enc.write(&f, true, 1));

    const string& d = w.data;
    uint32_t moov_size = 0;
    int moov = mp4_find_root(d, "moov", 0, &moov_size);
    ASSERT_NE(-1, moov);

    // trak은 하나뿐, trex도 하나.
    EXPECT_NE(-1, mp4_find_box(d, moov + 8, moov + (int)moov_size, "trak", 0, NULL));
    EXPECT_EQ(-1, mp4_find_box(d, moov + 8, moov + (int)moov_size, "trak", 1, NULL));
    uint32_t mvex_size = 0;
    int mvex = mp4_find_box(d, moov + 8, moov + (int)moov_size, "mvex", 0, &mvex_size);
    ASSERT_NE(-1, mvex);
    EXPECT_NE(-1, mp4_find_box(d, mvex + 8, mvex + (int)mvex_size, "trex", 0, NULL));
    EXPECT_EQ(-1, mp4_find_box(d, mvex + 8, mvex + (int)mvex_size, "trex", 1, NULL));
    // mvhd next_track_ID = 2.
    uint32_t mvhd_size = 0;
    int mvhd = mp4_find_box(d, moov + 8, moov + (int)moov_size, "mvhd", 0, &mvhd_size);
    ASSERT_NE(-1, mvhd);
    EXPECT_EQ(2u, mp4_u32(d, mvhd + (int)mvhd_size - 4));
}

TEST(KernelMp4Test, InitEncoderVideoResolution)
{
    srs_error_t err = srs_success;

    // 실제 x264 SPS(320x240) — Chrome MSE는 avc1/tkhd 해상도 0을 거부하므로(§5.6 S16)
    // init의 avc1과 tkhd에 SPS에서 파싱한 해상도가 실려야 한다.
    static const uint8_t sps[] = {
        0x67, 0x64, 0x00, 0x0d, 0xac, 0xd9, 0x41, 0x41, 0xfb, 0x01, 0x10, 0x00, 0x00,
        0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xc0, 0xf1, 0x42, 0x99, 0x60,
    };
    static const uint8_t pps[] = {0x68, 0xeb, 0xec, 0xb2, 0x2c};

    string vsh;
    const uint8_t head[] = {0x17, 0x00, 0x00, 0x00, 0x00};
    vsh.append((const char*)head, sizeof(head));
    const uint8_t avcc[] = {0x01, 0x64, 0x00, 0x0d, 0xff, 0xe1};
    vsh.append((const char*)avcc, sizeof(avcc));
    vsh.push_back(0x00); vsh.push_back((char)sizeof(sps));
    vsh.append((const char*)sps, sizeof(sps));
    vsh.push_back(0x01);
    vsh.push_back(0x00); vsh.push_back((char)sizeof(pps));
    vsh.append((const char*)pps, sizeof(pps));

    SrsFormat f;
    HELPER_ASSERT_SUCCESS(f.on_video(0, (char*)vsh.data(), (int)vsh.size()));
    ASSERT_EQ(320, f.vcodec->width);
    ASSERT_EQ(240, f.vcodec->height);

    MockMp4Writer w;
    SrsMp4M2tsInitEncoder enc;
    HELPER_ASSERT_SUCCESS(enc.initialize(&w));
    HELPER_ASSERT_SUCCESS(enc.write(&f, true, 1));

    const string& d = w.data;
    uint32_t moov_size = 0;
    int moov = mp4_find_root(d, "moov", 0, &moov_size);
    ASSERT_NE(-1, moov);

    // tkhd 끝 8바이트 = width/height (16.16 fixed).
    uint32_t trak_size = 0;
    int trak = mp4_find_box(d, moov + 8, moov + (int)moov_size, "trak", 0, &trak_size);
    ASSERT_NE(-1, trak);
    uint32_t tkhd_size = 0;
    int tkhd = mp4_find_box(d, trak + 8, trak + (int)trak_size, "tkhd", 0, &tkhd_size);
    ASSERT_NE(-1, tkhd);
    EXPECT_EQ(320u << 16, mp4_u32(d, tkhd + (int)tkhd_size - 8));
    EXPECT_EQ(240u << 16, mp4_u32(d, tkhd + (int)tkhd_size - 4));

    // avc1의 width/height: 프리앰블 8 + SampleEntry 8 + pre_defined/reserved 16 뒤 2B×2.
    uint32_t stsd_size = 0;
    int mdia = mp4_find_box(d, trak + 8, trak + (int)trak_size, "mdia", 0, NULL);
    ASSERT_NE(-1, mdia);
    int minf = -1, stbl = -1;
    {
        uint32_t mdia_size = 0;
        mp4_find_box(d, trak + 8, trak + (int)trak_size, "mdia", 0, &mdia_size);
        uint32_t minf_size = 0;
        minf = mp4_find_box(d, mdia + 8, mdia + (int)mdia_size, "minf", 0, &minf_size);
        ASSERT_NE(-1, minf);
        uint32_t stbl_size = 0;
        stbl = mp4_find_box(d, minf + 8, minf + (int)minf_size, "stbl", 0, &stbl_size);
        ASSERT_NE(-1, stbl);
        int stsd = mp4_find_box(d, stbl + 8, stbl + (int)stbl_size, "stsd", 0, &stsd_size);
        ASSERT_NE(-1, stsd);
        int avc1 = mp4_find_box(d, stsd + 8 + 4 + 4, stsd + (int)stsd_size, "avc1", 0, NULL);
        ASSERT_NE(-1, avc1);
        EXPECT_EQ(320, (int)((uint8_t)d[avc1 + 32] << 8 | (uint8_t)d[avc1 + 33]));
        EXPECT_EQ(240, (int)((uint8_t)d[avc1 + 34] << 8 | (uint8_t)d[avc1 + 35]));
    }
}

TEST(KernelMp4Test, InitEncoderRequiresSequenceHeader)
{
    srs_error_t err = srs_success;

    // 시퀀스 헤더를 파싱하지 않은 format으로는 init.mp4를 만들 수 없다.
    SrsFormat f;
    MockMp4Writer w;
    SrsMp4M2tsInitEncoder enc;
    HELPER_ASSERT_SUCCESS(enc.initialize(&w));
    HELPER_EXPECT_FAILED(enc.write(&f));
    HELPER_EXPECT_FAILED(enc.write(&f, true, 1));
}

// ---- m4s (moof + mdat) ----

TEST(KernelMp4Test, SegmentEncoderMuxed)
{
    srs_error_t err = srs_success;

    MockMp4Writer w;
    SrsMp4M2tsSegmentEncoder enc;
    // basetime은 첫 video dts(1000ms)와 같게 — tfdt 검증의 기준값.
    HELPER_ASSERT_SUCCESS(enc.initialize(&w, 7, 1000 * SRS_UTIME_MILLISECONDS, 1));

    // video 3프레임(33ms 간격, 첫 프레임만 키프레임) + audio 3프레임(23ms 간격).
    string v0("VVVV0"), v1("VVV1"), v2("VV2");
    string a0("AAA0"), a1("AA1"), a2("A2");
    HELPER_ASSERT_SUCCESS(enc.write_sample(SrsMp4HandlerTypeVIDE, SrsVideoAvcFrameTypeKeyFrame,
        1000, 1000, (uint8_t*)v0.data(), (uint32_t)v0.size()));
    HELPER_ASSERT_SUCCESS(enc.write_sample(SrsMp4HandlerTypeSOUN, 0x00,
        1000, 1000, (uint8_t*)a0.data(), (uint32_t)a0.size()));
    HELPER_ASSERT_SUCCESS(enc.write_sample(SrsMp4HandlerTypeVIDE, SrsVideoAvcFrameTypeInterFrame,
        1033, 1073, (uint8_t*)v1.data(), (uint32_t)v1.size()));
    HELPER_ASSERT_SUCCESS(enc.write_sample(SrsMp4HandlerTypeSOUN, 0x00,
        1023, 1023, (uint8_t*)a1.data(), (uint32_t)a1.size()));
    HELPER_ASSERT_SUCCESS(enc.write_sample(SrsMp4HandlerTypeVIDE, SrsVideoAvcFrameTypeInterFrame,
        1066, 1106, (uint8_t*)v2.data(), (uint32_t)v2.size()));
    HELPER_ASSERT_SUCCESS(enc.write_sample(SrsMp4HandlerTypeSOUN, 0x00,
        1046, 1046, (uint8_t*)a2.data(), (uint32_t)a2.size()));

    uint64_t end_dts = 1100;
    HELPER_ASSERT_SUCCESS(enc.flush(end_dts));

    const string& d = w.data;

    // styp + moof + mdat이 파일을 정확히 채운다.
    uint32_t styp_size = 0;
    int styp = mp4_find_root(d, "styp", 0, &styp_size);
    ASSERT_EQ(0, styp);
    EXPECT_EQ(0, d.compare(styp + 8, 4, "msdh"));
    uint32_t moof_size = 0;
    int moof = mp4_find_root(d, "moof", 0, &moof_size);
    ASSERT_EQ((int)styp_size, moof);
    uint32_t mdat_size = 0;
    int mdat = mp4_find_root(d, "mdat", 0, &mdat_size);
    ASSERT_EQ(moof + (int)moof_size, mdat);
    ASSERT_EQ((int)d.size(), mdat + (int)mdat_size);

    // mfhd: sequence_number = 7.
    uint32_t mfhd_size = 0;
    int mfhd = mp4_find_box(d, moof + 8, moof + (int)moof_size, "mfhd", 0, &mfhd_size);
    ASSERT_NE(-1, mfhd);
    EXPECT_EQ(7u, mp4_u32(d, mfhd + 12));

    // traf ×2: video(tid=1) 먼저, audio(tid=2).
    uint32_t vtraf_size = 0;
    int vtraf = mp4_find_box(d, moof + 8, moof + (int)moof_size, "traf", 0, &vtraf_size);
    ASSERT_NE(-1, vtraf);
    uint32_t atraf_size = 0;
    int atraf = mp4_find_box(d, moof + 8, moof + (int)moof_size, "traf", 1, &atraf_size);
    ASSERT_NE(-1, atraf);

    // video tfhd: flags 0x020000(default-base-is-moof), track_ID 1.
    uint32_t tfhd_size = 0;
    int tfhd = mp4_find_box(d, vtraf + 8, vtraf + (int)vtraf_size, "tfhd", 0, &tfhd_size);
    ASSERT_NE(-1, tfhd);
    EXPECT_EQ(0x020000u, mp4_u32(d, tfhd + 8) & 0x00ffffffu);
    EXPECT_EQ(1u, mp4_u32(d, tfhd + 12));

    // video tfdt(v1): basetime = 첫 video dts = 1000.
    uint32_t tfdt_size = 0;
    int tfdt = mp4_find_box(d, vtraf + 8, vtraf + (int)vtraf_size, "tfdt", 0, &tfdt_size);
    ASSERT_NE(-1, tfdt);
    EXPECT_EQ(1, (int)(uint8_t)d[tfdt + 8]); // version 1
    EXPECT_EQ(1000u, (uint32_t)mp4_u64(d, tfdt + 12));

    // video trun: 샘플 3개, duration 33/33/34(end_dts 1100), 키프레임 플래그.
    uint32_t vtrun_size = 0;
    int vtrun = mp4_find_box(d, vtraf + 8, vtraf + (int)vtraf_size, "trun", 0, &vtrun_size);
    ASSERT_NE(-1, vtrun);
    EXPECT_EQ(0x000f01u, mp4_u32(d, vtrun + 8) & 0x00ffffffu); // flags
    ASSERT_EQ(3u, mp4_u32(d, vtrun + 12)); // sample_count
    int ventry = vtrun + 20; // v/f(4) count(4) data_offset(4) 뒤
    EXPECT_EQ(33u, mp4_u32(d, ventry + 0)); // duration v0
    EXPECT_EQ((uint32_t)v0.size(), mp4_u32(d, ventry + 4));
    EXPECT_EQ(0x02000000u, mp4_u32(d, ventry + 8)); // keyframe
    EXPECT_EQ(0u, mp4_u32(d, ventry + 12)); // cts 0
    EXPECT_EQ(33u, mp4_u32(d, ventry + 16 + 0)); // duration v1
    EXPECT_EQ(0x01010000u, mp4_u32(d, ventry + 16 + 8)); // non-sync
    EXPECT_EQ(40u, mp4_u32(d, ventry + 16 + 12)); // cts = pts-dts = 40
    EXPECT_EQ(34u, mp4_u32(d, ventry + 32 + 0)); // duration v2 = 1100-1066
    EXPECT_EQ(0x01010000u, mp4_u32(d, ventry + 32 + 8));

    // data_offset: video = moof 크기 + mdat 헤더(8), audio = + video 페이로드.
    uint32_t video_bytes = (uint32_t)(v0.size() + v1.size() + v2.size());
    uint32_t audio_bytes = (uint32_t)(a0.size() + a1.size() + a2.size());
    EXPECT_EQ(moof_size + 8, mp4_u32(d, vtrun + 16));
    uint32_t atrun_size = 0;
    int atrun = mp4_find_box(d, atraf + 8, atraf + (int)atraf_size, "trun", 0, &atrun_size);
    ASSERT_NE(-1, atrun);
    EXPECT_EQ(moof_size + 8 + video_bytes, mp4_u32(d, atrun + 16));

    // audio tfhd tid=2, tfdt=1000, duration 23/23/54(1100-1046), 전 샘플 sync.
    tfhd = mp4_find_box(d, atraf + 8, atraf + (int)atraf_size, "tfhd", 0, &tfhd_size);
    ASSERT_NE(-1, tfhd);
    EXPECT_EQ(2u, mp4_u32(d, tfhd + 12));
    tfdt = mp4_find_box(d, atraf + 8, atraf + (int)atraf_size, "tfdt", 0, &tfdt_size);
    ASSERT_NE(-1, tfdt);
    EXPECT_EQ(1000u, (uint32_t)mp4_u64(d, tfdt + 12));
    ASSERT_EQ(3u, mp4_u32(d, atrun + 12));
    int aentry = atrun + 20;
    EXPECT_EQ(23u, mp4_u32(d, aentry + 0));
    EXPECT_EQ(0x02000000u, mp4_u32(d, aentry + 8));
    EXPECT_EQ(23u, mp4_u32(d, aentry + 16 + 0));
    EXPECT_EQ(54u, mp4_u32(d, aentry + 32 + 0));

    // mdat: 크기 = 8 + 샘플 합, 페이로드는 video들 뒤 audio들.
    ASSERT_EQ(8 + video_bytes + audio_bytes, mdat_size);
    EXPECT_EQ(v0 + v1 + v2 + a0 + a1 + a2, d.substr(mdat + 8));
}

TEST(KernelMp4Test, SegmentEncoderNegativeCts)
{
    srs_error_t err = srs_success;

    MockMp4Writer w;
    SrsMp4M2tsSegmentEncoder enc;
    HELPER_ASSERT_SUCCESS(enc.initialize(&w, 1, 0, 1));

    // pts < dts (B-frame 재정렬) → trun version 1, cts는 signed.
    string v0("V0");
    HELPER_ASSERT_SUCCESS(enc.write_sample(SrsMp4HandlerTypeVIDE, SrsVideoAvcFrameTypeKeyFrame,
        100, 90, (uint8_t*)v0.data(), (uint32_t)v0.size()));

    uint64_t end_dts = 133;
    HELPER_ASSERT_SUCCESS(enc.flush(end_dts));

    const string& d = w.data;
    uint32_t moof_size = 0;
    int moof = mp4_find_root(d, "moof", 0, &moof_size);
    ASSERT_NE(-1, moof);
    uint32_t traf_size = 0;
    int traf = mp4_find_box(d, moof + 8, moof + (int)moof_size, "traf", 0, &traf_size);
    ASSERT_NE(-1, traf);
    uint32_t trun_size = 0;
    int trun = mp4_find_box(d, traf + 8, traf + (int)traf_size, "trun", 0, &trun_size);
    ASSERT_NE(-1, trun);
    EXPECT_EQ(1, (int)(uint8_t)d[trun + 8]); // version 1
    EXPECT_EQ(-10, (int32_t)mp4_u32(d, trun + 20 + 12)); // cts = -10
}

TEST(KernelMp4Test, SegmentEncoderEmptyFlush)
{
    srs_error_t err = srs_success;

    MockMp4Writer w;
    SrsMp4M2tsSegmentEncoder enc;
    HELPER_ASSERT_SUCCESS(enc.initialize(&w, 1, 0, 1));

    // 샘플 없이 flush는 에러 (원본과 동일 — ERROR_MP4_ILLEGAL_MOOF).
    uint64_t end_dts = 0;
    err = enc.flush(end_dts);
    ASSERT_TRUE(err != srs_success);
    EXPECT_EQ(ERROR_MP4_ILLEGAL_MOOF, srs_error_code(err));
    srs_freep(err);
}

TEST(KernelMp4Test, SegmentEncoderAudioOnly)
{
    srs_error_t err = srs_success;

    MockMp4Writer w;
    SrsMp4M2tsSegmentEncoder enc;
    HELPER_ASSERT_SUCCESS(enc.initialize(&w, 1, 0, 2));

    // 단독 트랙이면 그 traf가 tid를 쓴다 (muxed 규칙의 경계).
    string a0("A0");
    HELPER_ASSERT_SUCCESS(enc.write_sample(SrsMp4HandlerTypeSOUN, 0x00,
        0, 0, (uint8_t*)a0.data(), (uint32_t)a0.size()));

    uint64_t end_dts = 23;
    HELPER_ASSERT_SUCCESS(enc.flush(end_dts));

    const string& d = w.data;
    uint32_t moof_size = 0;
    int moof = mp4_find_root(d, "moof", 0, &moof_size);
    ASSERT_NE(-1, moof);
    uint32_t traf_size = 0;
    int traf = mp4_find_box(d, moof + 8, moof + (int)moof_size, "traf", 0, &traf_size);
    ASSERT_NE(-1, traf);
    EXPECT_EQ(-1, mp4_find_box(d, moof + 8, moof + (int)moof_size, "traf", 1, NULL));
    uint32_t tfhd_size = 0;
    int tfhd = mp4_find_box(d, traf + 8, traf + (int)traf_size, "tfhd", 0, &tfhd_size);
    ASSERT_NE(-1, tfhd);
    EXPECT_EQ(2u, mp4_u32(d, tfhd + 12)); // 단독 audio traf가 tid=2
    // data_offset도 moof+8 (video 페이로드 없음).
    uint32_t trun_size = 0;
    int trun = mp4_find_box(d, traf + 8, traf + (int)traf_size, "trun", 0, &trun_size);
    ASSERT_NE(-1, trun);
    EXPECT_EQ(moof_size + 8, mp4_u32(d, trun + 16));
}

// 완료 조건: muxed 스트림(video=1, audio=2)의 "오디오 전용 파트" — 파트에 비디오
// 샘플이 우연히 없어도 audio traf는 set_audio_tid로 고정된 2를 쓴다. 내용 추론에
// 맡기면 tid=1(비디오 트랙)이 되어 AAC가 h264로 디코딩되는 실버그 (S15에서 발견).
TEST(KernelMp4Test, SegmentEncoderAudioOnlyPartInMuxedStream)
{
    srs_error_t err = srs_success;

    MockMp4Writer w;
    SrsMp4M2tsSegmentEncoder enc;
    HELPER_ASSERT_SUCCESS(enc.initialize(&w, 1, 0, 1));
    enc.set_audio_tid(2); // 스트림 구성은 muxed — 파트 내용과 무관하게 audio는 2.

    string a0("A0");
    HELPER_ASSERT_SUCCESS(enc.write_sample(SrsMp4HandlerTypeSOUN, 0x00,
        0, 0, (uint8_t*)a0.data(), (uint32_t)a0.size()));

    uint64_t end_dts = 23;
    HELPER_ASSERT_SUCCESS(enc.flush(end_dts));

    const string& d = w.data;
    uint32_t moof_size = 0;
    int moof = mp4_find_root(d, "moof", 0, &moof_size);
    ASSERT_NE(-1, moof);
    uint32_t traf_size = 0;
    int traf = mp4_find_box(d, moof + 8, moof + (int)moof_size, "traf", 0, &traf_size);
    ASSERT_NE(-1, traf);
    uint32_t tfhd_size = 0;
    int tfhd = mp4_find_box(d, traf + 8, traf + (int)traf_size, "tfhd", 0, &tfhd_size);
    ASSERT_NE(-1, tfhd);
    EXPECT_EQ(2u, mp4_u32(d, tfhd + 12)); // 추론(1)이 아니라 고정된 audio tid.
}
