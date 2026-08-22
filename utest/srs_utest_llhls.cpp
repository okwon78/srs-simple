// srs_simple — S13 LL-HLS 앱 계층 테스트: 파트/세그먼트 컷 정책, 인메모리 스토리지,
// wait_for 블로킹. 합성 프레임(30fps 대신 정수 간격 25fps/50Hz)을 주입해 경계를
// 결정적으로 검증한다 (TASKS.md S13).
#include "srs_utest.hpp"

#include <pthread.h>
#include <unistd.h>

#include <srs_app_config.hpp>
#include <srs_app_llhls.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_protocol_rtmp_stack.hpp>

// ---- 테스트용 FLV 페이로드 빌더 (srs_utest_hls.cpp와 동일 + profile 파라미터) ----

// AVC 시퀀스 헤더 (FLV): 0x17 0x00 cts(3B) + avcC. profile로 서로 다른 sh를 만든다.
static string mock_avc_sh(uint8_t profile = 0x64)
{
    const uint8_t sps[] = {0x67, profile, 0x00, 0x1f, 0xac, 0xd9};
    static const uint8_t pps[] = {0x68, 0xeb, 0xec, 0xb2, 0x2c};

    string v;
    const uint8_t head[] = {0x17, 0x00, 0x00, 0x00, 0x00};
    v.append((const char*)head, sizeof(head));
    const uint8_t avcc[] = {0x01, profile, 0x00, 0x1f, 0xff, 0xe1};
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

// SrsFormat 파싱 → SrsLlHls 주입 (hub의 on_audio/on_video 경로를 흉내 낸다).
static srs_error_t feed_video(SrsLlHls* llhls, SrsFormat* f, int64_t dts, const string& payload)
{
    srs_error_t err = f->on_video(dts, (char*)payload.data(), (int)payload.size());
    if (err != srs_success) {
        return err;
    }
    SrsSharedPtrMessage msg;
    msg.timestamp = dts;
    return llhls->on_video(&msg, f);
}

static srs_error_t feed_audio(SrsLlHls* llhls, SrsFormat* f, int64_t dts, const string& payload)
{
    srs_error_t err = f->on_audio(dts, (char*)payload.data(), (int)payload.size());
    if (err != srs_success) {
        return err;
    }
    SrsSharedPtrMessage msg;
    msg.timestamp = dts;
    return llhls->on_audio(&msg, f);
}

// ---- SrsLlHlsStorage: 윈도우와 조회 ----

TEST(AppLlHlsTest, StorageWindowAndQueries)
{
    SrsLlHlsStorage s;
    s.update_config(3, "livestream", 500 * SRS_UTIME_MILLISECONDS, 2 * SRS_UTIME_SECONDS);
    s.on_publish();

    EXPECT_EQ(-1, s.latest_msn());
    EXPECT_EQ(-1, s.first_msn());

    string v;
    EXPECT_FALSE(s.get_init(v));
    s.set_init("INIT");
    EXPECT_TRUE(s.get_init(v));
    EXPECT_EQ("INIT", v);

    // 세그먼트 0: 파트 2개 후 완결.
    s.append_part("a0", 480 * SRS_UTIME_MILLISECONDS, true, false);
    s.append_part("a1", 480 * SRS_UTIME_MILLISECONDS, false, true);

    int64_t msn = -1; int psn = -1;
    EXPECT_TRUE(s.latest(msn, psn));
    EXPECT_EQ(0, msn);
    EXPECT_EQ(1, psn);

    EXPECT_TRUE(s.get_part(0, 0, v));
    EXPECT_EQ("a0", v);
    EXPECT_TRUE(s.get_segment(0, v));
    EXPECT_EQ("a0a1", v); // 세그먼트 = 파트들의 연결
    EXPECT_FALSE(s.get_part(0, 2, v));
    EXPECT_FALSE(s.get_segment(1, v));

    // 세그먼트 1~4 완결 → 윈도우 3 초과분(0, 1) 만료. 파트도 함께 사라진다.
    for (int i = 1; i <= 4; i++) {
        s.append_part("x", 1 * SRS_UTIME_SECONDS, true, true);
    }
    EXPECT_EQ(3, s.size());
    EXPECT_EQ(2, s.first_msn());
    EXPECT_EQ(4, s.latest_msn());
    EXPECT_FALSE(s.get_part(0, 0, v));
    EXPECT_FALSE(s.get_segment(0, v));
    EXPECT_TRUE(s.get_segment(4, v));

    // 진행 중(미완결) 세그먼트: 파트는 조회되지만 세그먼트 전체는 아직 아니다.
    s.append_part("y", 480 * SRS_UTIME_MILLISECONDS, true, false);
    EXPECT_EQ(5, s.latest_msn());
    EXPECT_TRUE(s.get_part(5, 0, v));
    EXPECT_FALSE(s.get_segment(5, v));

    // unpublish가 꼬리 세그먼트를 완결한다 (EXTINF 확정).
    s.on_unpublish();
    EXPECT_TRUE(s.get_segment(5, v));
    EXPECT_EQ("y", v);
}

// ---- 컷 정책: 파트 경계와 세그먼트 컷 (video 전용 — 경계가 결정적) ----

TEST(AppLlHlsTest, MuxerPartAndSegmentCut)
{
    srs_error_t err = srs_success;

    SrsSimpleConfig saved = *_srs_config;
    _srs_config->llhls_part = 500 * SRS_UTIME_MILLISECONDS;
    _srs_config->llhls_segment = 2 * SRS_UTIME_SECONDS;
    _srs_config->llhls_segment_count = 10;

    SrsRequest req;
    req.vhost = "__defaultVhost__";
    req.app = "live";
    req.stream = "livestream";

    SrsLlHls llhls;
    HELPER_ASSERT_SUCCESS(llhls.initialize(NULL, &req));
    HELPER_ASSERT_SUCCESS(llhls.on_publish());

    SrsFormat f;
    HELPER_ASSERT_SUCCESS(f.initialize());
    HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, 0, mock_avc_sh()));

    // 25fps(40ms), 키프레임 2000ms 간격(GOP=세그먼트 길이) — PLANS.md D5의 전제.
    string idr("\x65\x88\x84\x00\x33\xff", 6);
    string p_nalu("\x41\x9a\x00\x33", 4);
    for (int dts = 0; dts <= 4400; dts += 40) {
        bool keyframe = (dts % 2000) == 0;
        HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, dts, mock_avc_frame(keyframe, 0, keyframe ? idr : p_nalu)));
    }
    llhls.on_unpublish();

    SrsLlHlsStorage* st = llhls.storage();

    // init.mp4: ftyp로 시작, video 전용(avc1만).
    string init;
    ASSERT_TRUE(st->get_init(init));
    EXPECT_EQ("ftyp", init.substr(4, 4));
    EXPECT_NE(string::npos, init.find("avc1"));
    EXPECT_EQ(string::npos, init.find("mp4a"));

    // 세그먼트 3개: [0,2000) [2000,4000) [4000,4400+unpublish).
    ASSERT_EQ(3, st->size());

    // 세그먼트 0: 파트 480ms×4 + 80ms(세그먼트 마지막 — 짧아도 되는 스펙 예외).
    SrsLlHlsSegment* s0 = st->segments_[0];
    EXPECT_EQ(0, s0->msn);
    EXPECT_TRUE(s0->completed);
    ASSERT_EQ(5, (int)s0->parts.size());
    EXPECT_EQ(2000 * SRS_UTIME_MILLISECONDS, s0->duration);
    srs_utime_t sum = 0;
    for (int i = 0; i < (int)s0->parts.size(); i++) {
        SrsLlHlsPart* p = s0->parts[i];
        EXPECT_EQ(i, p->psn);
        sum += p->duration;
        // 마지막이 아닌 파트: 85%×target ≤ duration ≤ target (스펙 요구를 컷 정책이 보장).
        if (i + 1 < (int)s0->parts.size()) {
            EXPECT_GE(p->duration, _srs_config->llhls_part * 85 / 100);
            EXPECT_LE(p->duration, _srs_config->llhls_part);
        }
        // 파트 바이트는 styp(24B) + moof + mdat.
        ASSERT_GE((int)p->payload.size(), 32);
        EXPECT_EQ("styp", p->payload.substr(4, 4));
        EXPECT_EQ("moof", p->payload.substr(28, 4));
        EXPECT_NE(string::npos, p->payload.find("mdat"));
    }
    EXPECT_EQ(s0->duration, sum); // EXTINF = 파트 duration 합
    EXPECT_EQ(80 * SRS_UTIME_MILLISECONDS, s0->parts.back()->duration);

    // independent는 키프레임으로 시작하는 파트에만 — 세그먼트 선두 파트뿐이다.
    EXPECT_TRUE(s0->parts[0]->independent);
    for (int i = 1; i < (int)s0->parts.size(); i++) {
        EXPECT_FALSE(s0->parts[i]->independent);
    }

    // msn 연속 증가 + 세그먼트 1도 같은 모양.
    SrsLlHlsSegment* s1 = st->segments_[1];
    EXPECT_EQ(1, s1->msn);
    EXPECT_TRUE(s1->completed);
    EXPECT_EQ(2000 * SRS_UTIME_MILLISECONDS, s1->duration);
    EXPECT_TRUE(s1->parts[0]->independent);

    // 세그먼트 2: unpublish가 닫은 꼬리 — 파트 1개(400ms).
    SrsLlHlsSegment* s2 = st->segments_[2];
    EXPECT_EQ(2, s2->msn);
    EXPECT_TRUE(s2->completed);
    ASSERT_EQ(1, (int)s2->parts.size());
    EXPECT_EQ(400 * SRS_UTIME_MILLISECONDS, s2->duration);
    EXPECT_TRUE(s2->parts[0]->independent);

    *_srs_config = saved;
}

// ---- Muxed A/V: init에 두 트랙, 파트 상한 준수, independent 마킹 ----

TEST(AppLlHlsTest, MuxerMuxedAvAndInit)
{
    srs_error_t err = srs_success;

    SrsSimpleConfig saved = *_srs_config;
    _srs_config->llhls_part = 500 * SRS_UTIME_MILLISECONDS;
    _srs_config->llhls_segment = 2 * SRS_UTIME_SECONDS;

    SrsRequest req;
    req.app = "live";
    req.stream = "livestream";

    SrsLlHls llhls;
    HELPER_ASSERT_SUCCESS(llhls.initialize(NULL, &req));
    HELPER_ASSERT_SUCCESS(llhls.on_publish());

    SrsFormat f;
    HELPER_ASSERT_SUCCESS(f.initialize());
    HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, 0, mock_avc_sh()));
    HELPER_ASSERT_SUCCESS(feed_audio(&llhls, &f, 0, mock_aac_sh()));

    // video 40ms(키프레임 2000ms 간격) + audio 20ms 인터리브.
    string idr("\x65\x88\x84\x00\x33\xff", 6);
    string p_nalu("\x41\x9a\x00\x33", 4);
    string aac_raw("\x21\x10\x04\x60\x8c\x1c", 6);
    for (int t = 0; t <= 2100; t += 20) {
        if ((t % 40) == 0) {
            bool keyframe = (t % 2000) == 0;
            HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, t, mock_avc_frame(keyframe, 0, keyframe ? idr : p_nalu)));
        }
        HELPER_ASSERT_SUCCESS(feed_audio(&llhls, &f, t, mock_aac_raw(aac_raw)));
    }
    llhls.on_unpublish();

    SrsLlHlsStorage* st = llhls.storage();

    // muxed init: avc1(video trak) + mp4a(audio trak) 둘 다.
    string init;
    ASSERT_TRUE(st->get_init(init));
    EXPECT_NE(string::npos, init.find("avc1"));
    EXPECT_NE(string::npos, init.find("mp4a"));

    // 키프레임 2000ms에서 세그먼트 0이 닫힌다.
    ASSERT_GE(st->size(), 2);
    SrsLlHlsSegment* s0 = st->segments_[0];
    EXPECT_TRUE(s0->completed);
    EXPECT_EQ(2000 * SRS_UTIME_MILLISECONDS, s0->duration);

    srs_utime_t sum = 0;
    int independents = 0;
    for (int i = 0; i < (int)s0->parts.size(); i++) {
        SrsLlHlsPart* p = s0->parts[i];
        sum += p->duration;
        if (p->independent) independents++;
        // 모든 파트 ≤ PART-TARGET (오디오 프레임이 컷을 당겨도 상한은 지켜진다).
        EXPECT_LE(p->duration, _srs_config->llhls_part);
    }
    EXPECT_EQ(s0->duration, sum);
    // independent는 키프레임으로 시작하는 선두 파트 하나뿐 (키프레임은 0/2000ms에만).
    EXPECT_EQ(1, independents);
    EXPECT_TRUE(s0->parts[0]->independent);

    *_srs_config = saved;
}

// ---- Pure audio: 오디오가 파트/세그먼트 컷을 주도, 전 파트 independent ----

TEST(AppLlHlsTest, MuxerPureAudio)
{
    srs_error_t err = srs_success;

    SrsSimpleConfig saved = *_srs_config;
    _srs_config->llhls_part = 500 * SRS_UTIME_MILLISECONDS;
    _srs_config->llhls_segment = 2 * SRS_UTIME_SECONDS;

    SrsRequest req;
    req.app = "live";
    req.stream = "livestream";

    SrsLlHls llhls;
    HELPER_ASSERT_SUCCESS(llhls.initialize(NULL, &req));
    HELPER_ASSERT_SUCCESS(llhls.on_publish());

    SrsFormat f;
    HELPER_ASSERT_SUCCESS(f.initialize());
    HELPER_ASSERT_SUCCESS(feed_audio(&llhls, &f, 0, mock_aac_sh()));

    string aac_raw("\x21\x10\x04\x60\x8c\x1c", 6);
    for (int dts = 0; dts <= 4400; dts += 20) {
        HELPER_ASSERT_SUCCESS(feed_audio(&llhls, &f, dts, mock_aac_raw(aac_raw)));
    }
    llhls.on_unpublish();

    SrsLlHlsStorage* st = llhls.storage();

    // audio 전용 init (avc1 없음).
    string init;
    ASSERT_TRUE(st->get_init(init));
    EXPECT_NE(string::npos, init.find("mp4a"));
    EXPECT_EQ(string::npos, init.find("avc1"));

    // 키프레임 없이도 세그먼트가 2000ms에서 닫힌다 (오디오는 전부 independent).
    ASSERT_EQ(3, st->size());
    SrsLlHlsSegment* s0 = st->segments_[0];
    EXPECT_TRUE(s0->completed);
    EXPECT_EQ(2000 * SRS_UTIME_MILLISECONDS, s0->duration);
    for (int i = 0; i < (int)s0->parts.size(); i++) {
        EXPECT_TRUE(s0->parts[i]->independent);
    }

    *_srs_config = saved;
}

// ---- 시퀀스 헤더: 동일 재전송 무시, 변경 시 세그먼트 컷 + init 재생성 ----

TEST(AppLlHlsTest, MuxerSequenceHeaderChange)
{
    srs_error_t err = srs_success;

    SrsSimpleConfig saved = *_srs_config;
    _srs_config->llhls_part = 500 * SRS_UTIME_MILLISECONDS;
    _srs_config->llhls_segment = 2 * SRS_UTIME_SECONDS;

    SrsRequest req;
    req.app = "live";
    req.stream = "livestream";

    SrsLlHls llhls;
    HELPER_ASSERT_SUCCESS(llhls.initialize(NULL, &req));
    HELPER_ASSERT_SUCCESS(llhls.on_publish());

    SrsFormat f;
    HELPER_ASSERT_SUCCESS(f.initialize());
    HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, 0, mock_avc_sh(0x64)));

    string idr("\x65\x88\x84\x00\x33\xff", 6);
    string p_nalu("\x41\x9a\x00\x33", 4);
    for (int dts = 0; dts <= 400; dts += 40) {
        HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, dts, mock_avc_frame(dts == 0, 0, dts == 0 ? idr : p_nalu)));
    }

    SrsLlHlsStorage* st = llhls.storage();
    string init1;
    ASSERT_TRUE(st->get_init(init1));

    // 동일 시퀀스 헤더 재전송 → 컷도 재생성도 없다 (파트가 아직 열려 있어 storage 비어 있음).
    HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, 400, mock_avc_sh(0x64)));
    EXPECT_EQ(0, st->size());

    // 변경된 시퀀스 헤더 → 진행 중 세그먼트를 닫는다 (400ms — 목표 2s 전이라도).
    HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, 400, mock_avc_sh(0x42)));
    ASSERT_EQ(1, st->size());
    EXPECT_TRUE(st->segments_[0]->completed);
    EXPECT_EQ(400 * SRS_UTIME_MILLISECONDS, st->segments_[0]->duration);

    // 새 설정의 첫 파트가 열리면 init이 재생성된다 (새 세그먼트 선두 — MAP 공유 규칙).
    HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, 440, mock_avc_frame(true, 0, idr)));
    HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, 480, mock_avc_frame(false, 0, p_nalu)));
    string init2;
    ASSERT_TRUE(st->get_init(init2));
    EXPECT_NE(init1, init2);

    // 새 세그먼트는 첫 파트가 flush될 때 storage에 나타난다 — unpublish로 확정.
    llhls.on_unpublish();
    ASSERT_EQ(2, st->size());
    EXPECT_EQ(1, st->segments_[1]->msn);
    EXPECT_TRUE(st->segments_[1]->parts[0]->independent);
    *_srs_config = saved;
}

// ---- wait_for: 게시 웨이크업 / 타임아웃 / unpublish 전원 기상 ----

struct MockWaiter
{
    SrsLlHlsStorage* storage;
    int64_t msn;
    int psn;
    srs_utime_t timeout;
    bool reached;
};

static void* mock_waiter_run(void* arg)
{
    MockWaiter* w = (MockWaiter*)arg;
    w->reached = w->storage->wait_for(w->msn, w->psn, w->timeout);
    return NULL;
}

TEST(AppLlHlsTest, StorageWaitFor)
{
    SrsLlHlsStorage s;
    s.update_config(10, "livestream", 500 * SRS_UTIME_MILLISECONDS, 2 * SRS_UTIME_SECONDS);
    s.on_publish();

    // 타임아웃: 아무것도 게시되지 않으면 false.
    EXPECT_FALSE(s.wait_for(0, 0, 30 * SRS_UTIME_MILLISECONDS));

    // 게시 웨이크업: 다른 스레드가 (0,0)을 기다리는 동안 파트를 게시한다.
    MockWaiter w1 = {&s, 0, 0, 5 * SRS_UTIME_SECONDS, false};
    pthread_t t1;
    ASSERT_EQ(0, pthread_create(&t1, NULL, mock_waiter_run, &w1));
    usleep(30 * 1000);
    s.append_part("p", 480 * SRS_UTIME_MILLISECONDS, true, false);
    pthread_join(t1, NULL);
    EXPECT_TRUE(w1.reached);

    // 이미 게시된 과거 위치는 즉시 true.
    EXPECT_TRUE(s.wait_for(0, 0, 0));
    // 아직인 미래 위치는 대기 대상 (여기서는 짧은 타임아웃으로 false 확인).
    EXPECT_FALSE(s.wait_for(0, 1, 30 * SRS_UTIME_MILLISECONDS));

    // unpublish 전원 기상: 먼 미래를 기다리던 스레드가 조건 미충족(false)으로 깨어난다.
    MockWaiter w2 = {&s, 100, 0, 30 * SRS_UTIME_SECONDS, true};
    pthread_t t2;
    ASSERT_EQ(0, pthread_create(&t2, NULL, mock_waiter_run, &w2));
    usleep(30 * 1000);
    s.on_unpublish();
    pthread_join(t2, NULL); // 30초 타임아웃 전에 즉시 깨어나야 join이 바로 끝난다
    EXPECT_FALSE(w2.reached);
}

// ---- S14 SrsLlHlsChunklist: 태그 순서/값, 헤더부 (TASKS.md S14) ----

TEST(AppLlHlsTest, ChunklistTagsAndValues)
{
    SrsLlHlsStorage s;
    s.update_config(10, "livestream", 500 * SRS_UTIME_MILLISECONDS, 2 * SRS_UTIME_SECONDS);
    s.on_publish();

    // 세그먼트(=첫 파트) 전에는 플레이리스트가 없다.
    string m3u8;
    EXPECT_FALSE(s.get_playlist(m3u8));

    // 세그먼트 0: 500ms 파트 4개로 완결(2s). 세그먼트 1: 진행 중 파트 1개.
    s.set_init("INIT");
    s.append_part("p0", 500 * SRS_UTIME_MILLISECONDS, true, false);
    s.append_part("p1", 500 * SRS_UTIME_MILLISECONDS, false, false);
    s.append_part("p2", 500 * SRS_UTIME_MILLISECONDS, false, false);
    s.append_part("p3", 500 * SRS_UTIME_MILLISECONDS, false, true);
    s.append_part("q0", 480 * SRS_UTIME_MILLISECONDS, true, false);

    ASSERT_TRUE(s.get_playlist(m3u8));

    // 필수 태그와 값 (D5: HOLD-BACK = 3×PART-TARGET, TARGETDURATION은 올림).
    EXPECT_NE(string::npos, m3u8.find("#EXTM3U\n"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-VERSION:6\n"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-TARGETDURATION:2\n"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,PART-HOLD-BACK=1.500\n"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-PART-INF:PART-TARGET=0.500\n"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-MEDIA-SEQUENCE:0\n"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-MAP:URI=\"livestream/init.mp4\"\n"));

    // 태그 순서: 헤더부는 OME MakeChunklist 순서, 세그먼트부는 PART가 EXTINF보다
    // 먼저, PRELOAD-HINT는 맨 끝.
    size_t p_ver = m3u8.find("#EXT-X-VERSION");
    size_t p_td = m3u8.find("#EXT-X-TARGETDURATION");
    size_t p_sc = m3u8.find("#EXT-X-SERVER-CONTROL");
    size_t p_pi = m3u8.find("#EXT-X-PART-INF");
    size_t p_ms = m3u8.find("#EXT-X-MEDIA-SEQUENCE");
    size_t p_map = m3u8.find("#EXT-X-MAP");
    size_t p_part = m3u8.find("#EXT-X-PART:");
    size_t p_extinf = m3u8.find("#EXTINF");
    size_t p_hint = m3u8.find("#EXT-X-PRELOAD-HINT");
    EXPECT_TRUE(m3u8.find("#EXTM3U") < p_ver && p_ver < p_td && p_td < p_sc);
    EXPECT_TRUE(p_sc < p_pi && p_pi < p_ms && p_ms < p_map && p_map < p_part);
    EXPECT_TRUE(p_part < p_extinf && p_extinf < p_hint);

    // PART 라인: INDEPENDENT=YES는 independent 파트에만.
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-PART:DURATION=0.500,URI=\"livestream/0.0.m4s\",INDEPENDENT=YES\n"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-PART:DURATION=0.500,URI=\"livestream/0.1.m4s\"\n"));

    // 완결 세그먼트 0: EXTINF = 파트 duration 합. 진행 중 세그먼트 1: PART만, EXTINF 없음.
    EXPECT_NE(string::npos, m3u8.find("#EXTINF:2.000,\nlivestream/0.m4s\n"));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-PART:DURATION=0.480,URI=\"livestream/1.0.m4s\",INDEPENDENT=YES\n"));
    EXPECT_EQ(string::npos, m3u8.find("livestream/1.m4s"));
    EXPECT_EQ(p_extinf, m3u8.rfind("#EXTINF"));
}

// ---- S14: 파트는 최근 3세그먼트에만, 그 이전은 EXTINF만 ----

TEST(AppLlHlsTest, ChunklistPartsOnlyRecentSegments)
{
    SrsLlHlsStorage s;
    s.update_config(10, "livestream", 500 * SRS_UTIME_MILLISECONDS, 2 * SRS_UTIME_SECONDS);
    s.on_publish();

    // 완결 세그먼트 5개 (msn 0~4).
    for (int i = 0; i < 5; i++) {
        s.append_part("x", 2 * SRS_UTIME_SECONDS, true, true);
    }

    string m3u8;
    ASSERT_TRUE(s.get_playlist(m3u8));

    // 파트는 최근 3개(msn 2,3,4)에만 — msn 0,1은 EXTINF만 남는다.
    EXPECT_EQ(string::npos, m3u8.find("URI=\"livestream/0.0.m4s\""));
    EXPECT_EQ(string::npos, m3u8.find("URI=\"livestream/1.0.m4s\""));
    EXPECT_NE(string::npos, m3u8.find("URI=\"livestream/2.0.m4s\""));
    EXPECT_NE(string::npos, m3u8.find("URI=\"livestream/3.0.m4s\""));
    EXPECT_NE(string::npos, m3u8.find("URI=\"livestream/4.0.m4s\""));

    // EXTINF는 완결 세그먼트 5개 전부.
    for (int i = 0; i < 5; i++) {
        char uri[64];
        snprintf(uri, sizeof(uri), "livestream/%d.m4s\n", i);
        EXPECT_NE(string::npos, m3u8.find(uri));
    }
}

// ---- S14: PRELOAD-HINT URI = 다음 게시에서 실제 생기는 파트 (게시 진행시키며 검증) ----

TEST(AppLlHlsTest, ChunklistPreloadHint)
{
    SrsLlHlsStorage s;
    s.update_config(10, "livestream", 500 * SRS_UTIME_MILLISECONDS, 2 * SRS_UTIME_SECONDS);
    s.on_publish();

    // 진행 중 세그먼트 0에 파트 0 → 힌트는 같은 세그먼트의 다음 파트 0.1.
    s.append_part("p", 500 * SRS_UTIME_MILLISECONDS, true, false);
    string m3u8;
    ASSERT_TRUE(s.get_playlist(m3u8));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"livestream/0.1.m4s\"\n"));

    // 게시를 진행시키면 힌트한 위치 (0,1)에 파트가 실제로 생긴다.
    s.append_part("q", 500 * SRS_UTIME_MILLISECONDS, false, false);
    string v;
    EXPECT_TRUE(s.get_part(0, 1, v));

    // 세그먼트를 완결하면 힌트는 새 세그먼트의 선두 1.0으로 넘어간다.
    s.append_part("r", 500 * SRS_UTIME_MILLISECONDS, false, true);
    ASSERT_TRUE(s.get_playlist(m3u8));
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"livestream/1.0.m4s\"\n"));

    // 실제 다음 게시 위치도 (1, 0)이다.
    s.append_part("s", 500 * SRS_UTIME_MILLISECONDS, true, false);
    int64_t msn = -1; int psn = -1;
    ASSERT_TRUE(s.latest(msn, psn));
    EXPECT_EQ(1, msn);
    EXPECT_EQ(0, psn);

    // unpublish 후에는 힌트가 없다 — 오지 않을 파트를 클라이언트가 홀드하지 않게.
    s.on_unpublish();
    ASSERT_TRUE(s.get_playlist(m3u8));
    EXPECT_EQ(string::npos, m3u8.find("#EXT-X-PRELOAD-HINT"));
}

// ---- S14: 윈도우 shrink 후 MEDIA-SEQUENCE 증가, msn 연속성 ----

TEST(AppLlHlsTest, ChunklistWindowMediaSequence)
{
    SrsLlHlsStorage s;
    s.update_config(3, "livestream", 500 * SRS_UTIME_MILLISECONDS, 2 * SRS_UTIME_SECONDS);
    s.on_publish();

    // 완결 세그먼트 5개 → 윈도우 3 초과분(msn 0,1) 만료.
    for (int i = 0; i < 5; i++) {
        s.append_part("x", 2 * SRS_UTIME_SECONDS, true, true);
    }

    string m3u8;
    ASSERT_TRUE(s.get_playlist(m3u8));

    // MEDIA-SEQUENCE = 윈도우 첫 세그먼트의 msn (만료를 클라이언트에 알린다).
    EXPECT_NE(string::npos, m3u8.find("#EXT-X-MEDIA-SEQUENCE:2\n"));
    EXPECT_EQ(string::npos, m3u8.find("livestream/0.m4s"));
    EXPECT_EQ(string::npos, m3u8.find("livestream/1.m4s"));

    // 남은 msn은 연속 (2, 3, 4).
    size_t u2 = m3u8.find("livestream/2.m4s\n");
    size_t u3 = m3u8.find("livestream/3.m4s\n");
    size_t u4 = m3u8.find("livestream/4.m4s\n");
    EXPECT_NE(string::npos, u2);
    EXPECT_NE(string::npos, u3);
    EXPECT_NE(string::npos, u4);
    EXPECT_TRUE(u2 < u3 && u3 < u4);
}

// ---- S15 회귀: muxed 스트림의 오디오 전용 파트가 오디오 트랙(2)에 실리는지 ----

// 페이로드에서 n번째 "tfhd"의 track_ID를 읽는다. 없으면 -1.
// tfhd 박스: size(4) type(4) version/flags(4) track_ID(4).
static int64_t mock_tfhd_tid(const string& d, int nth)
{
    size_t pos = 0;
    for (int i = 0; ; i++) {
        pos = d.find("tfhd", pos);
        if (pos == string::npos || pos + 12 > d.length()) {
            return -1;
        }
        if (i == nth) {
            return ((int64_t)(uint8_t)d[pos + 8] << 24) | ((uint8_t)d[pos + 9] << 16)
                | ((uint8_t)d[pos + 10] << 8) | (uint8_t)d[pos + 11];
        }
        pos += 4;
    }
}

// 완료 조건: 비디오가 세그먼트 중간에 끊겨 꼬리 파트가 오디오만 담아도, 그 파트의
// audio traf는 init의 오디오 트랙(2)을 가리킨다. 파트 내용으로 추론하면 비디오
// 트랙(1)에 실려 AAC가 h264로 디코딩된다 — S15 실스트림에서 발견한 버그의 재현.
TEST(AppLlHlsTest, MuxedAudioOnlyTailPartTrackId)
{
    srs_error_t err = srs_success;

    SrsSimpleConfig saved = *_srs_config;
    _srs_config->llhls_part = 500 * SRS_UTIME_MILLISECONDS;
    _srs_config->llhls_segment = 2 * SRS_UTIME_SECONDS;
    _srs_config->llhls_segment_count = 10;

    SrsRequest req;
    req.vhost = "__defaultVhost__";
    req.app = "live";
    req.stream = "livestream";

    SrsLlHls llhls;
    HELPER_ASSERT_SUCCESS(llhls.initialize(NULL, &req));
    HELPER_ASSERT_SUCCESS(llhls.on_publish());

    SrsFormat f;
    HELPER_ASSERT_SUCCESS(f.initialize());
    HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, 0, mock_avc_sh()));
    HELPER_ASSERT_SUCCESS(feed_audio(&llhls, &f, 0, mock_aac_sh()));

    // 비디오는 960ms까지만(25fps), 오디오는 20ms 간격으로 2초까지 —
    // [1000, 2000) 구간의 파트들은 오디오만 담는다. 2000ms 키프레임이 세그먼트 컷.
    string idr("\x65\x88\x84\x00\x33\xff", 6);
    string p_nalu("\x41\x9a\x00\x33", 4);
    string aac_raw("\x21\x10\x04\x60\x8c\x1c", 6);
    for (int dts = 0; dts <= 2000; dts += 20) {
        if (dts % 40 == 0 && (dts <= 960 || dts == 2000)) {
            bool keyframe = (dts % 2000) == 0;
            HELPER_ASSERT_SUCCESS(feed_video(&llhls, &f, dts, mock_avc_frame(keyframe, 0, keyframe ? idr : p_nalu)));
        }
        HELPER_ASSERT_SUCCESS(feed_audio(&llhls, &f, dts, mock_aac_raw(aac_raw)));
    }
    llhls.on_unpublish();

    SrsLlHlsStorage* st = llhls.storage();
    ASSERT_GE(st->size(), 1);
    SrsLlHlsSegment* s0 = st->segments_[0];
    ASSERT_GE((int)s0->parts.size(), 3);

    // 모든 파트에서: traf 1개(오디오 전용)면 tid=2, 2개(muxed)면 {1, 2}.
    bool saw_audio_only = false;
    for (int i = 0; i < (int)s0->parts.size(); i++) {
        const string& d = s0->parts[i]->payload;
        int64_t t0 = mock_tfhd_tid(d, 0);
        int64_t t1 = mock_tfhd_tid(d, 1);
        ASSERT_NE(-1, t0);
        if (t1 == -1) {
            saw_audio_only = true;
            EXPECT_EQ(2, t0); // 오디오 전용 파트 — 반드시 오디오 트랙.
        } else {
            EXPECT_EQ(1, t0); // video traf 먼저.
            EXPECT_EQ(2, t1);
        }
    }
    // 시나리오가 실제로 오디오 전용 파트를 만들었는지 (테스트 자체 검증).
    EXPECT_TRUE(saw_audio_only);

    *_srs_config = saved;
}
