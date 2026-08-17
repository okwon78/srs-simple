// srs_simple — S8 스트림 허브 테스트.
// 원본 utest/srs_utest_app2.cpp(소스/캐시)의 축소판: jitter 보정, 큐 shrink 정책,
// GOP 캐시 clear 규칙, MetaCache dumps 순서, 팬아웃/중간 입장 프리필.
#include <srs_utest.hpp>

#include <srs_app_config.hpp>
#include <srs_app_source.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_protocol_rtmp_msg_array.hpp>
#include <srs_protocol_rtmp_stack.hpp>

// FLV 태그 첫 1~2바이트 (srs_kernel_codec 판별자 기준).
static const char MOCK_VIDEO_SH[]    = {0x17, 0x00, 0x00, 0x00, 0x00}; // AVC keyframe + sequence header
static const char MOCK_VIDEO_KEY[]   = {0x17, 0x01, 0x00, 0x00, 0x00}; // AVC keyframe + NALU
static const char MOCK_VIDEO_INTER[] = {0x27, 0x01, 0x00, 0x00, 0x00}; // AVC inter frame + NALU
static const char MOCK_AUDIO_SH[]    = {(char)0xaf, 0x00, 0x12};       // AAC + sequence header
static const char MOCK_AUDIO_RAW[]   = {(char)0xaf, 0x01, 0x34};       // AAC + raw data

// 송신용 SrsSharedPtrMessage 생성 (페이로드 복사).
static SrsSharedPtrMessage* mock_shared_msg(int8_t type, int64_t timestamp, const char* data, int size)
{
    SrsMessageHeader header;
    header.message_type = type;
    header.timestamp = timestamp;
    header.payload_length = size;

    char* payload = new char[size];
    memcpy(payload, data, size);

    SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
    srs_error_t err = msg->create(&header, payload, size);
    srs_freep(err);
    return msg;
}

// 수신용 SrsCommonMessage 생성 (SrsLiveSource::on_audio/on_video 입력).
static void mock_common_msg(SrsCommonMessage* msg, int8_t type, int64_t timestamp, const char* data, int size)
{
    msg->header.message_type = type;
    msg->header.timestamp = timestamp;
    msg->header.payload_length = size;
    msg->create_payload(size);
    memcpy(msg->payload, data, size);
    msg->size = size;
}

static void mock_request(SrsRequest* req)
{
    req->vhost = SRS_CONSTS_RTMP_DEFAULT_VHOST;
    req->app = "live";
    req->stream = "test";
}

// FULL 알고리즘: 위생 처리한 델타 누적. 점프(>250ms)/역행 델타는 10ms로 클램프.
VOID TEST(AppSourceTest, RtmpJitterCorrect)
{
    srs_error_t err = srs_success;

    SrsRtmpJitter jitter;

    // 첫 패킷: delta=0, 0에서 시작.
    SrsSharedPtrMessage* m0 = mock_shared_msg(RTMP_MSG_VideoMessage, 0, MOCK_VIDEO_KEY, sizeof(MOCK_VIDEO_KEY));
    HELPER_EXPECT_SUCCESS(jitter.correct(m0, SrsRtmpJitterAlgorithmFULL));
    EXPECT_EQ(0, (int)m0->timestamp);
    srs_freep(m0);

    // 정상 델타(40ms)는 그대로 누적.
    SrsSharedPtrMessage* m1 = mock_shared_msg(RTMP_MSG_VideoMessage, 40, MOCK_VIDEO_INTER, sizeof(MOCK_VIDEO_INTER));
    HELPER_EXPECT_SUCCESS(jitter.correct(m1, SrsRtmpJitterAlgorithmFULL));
    EXPECT_EQ(40, (int)m1->timestamp);
    srs_freep(m1);

    // 점프(40→10000, delta 9960 > 250ms): 10ms로 클램프.
    SrsSharedPtrMessage* m2 = mock_shared_msg(RTMP_MSG_VideoMessage, 10000, MOCK_VIDEO_INTER, sizeof(MOCK_VIDEO_INTER));
    HELPER_EXPECT_SUCCESS(jitter.correct(m2, SrsRtmpJitterAlgorithmFULL));
    EXPECT_EQ(50, (int)m2->timestamp);
    srs_freep(m2);

    // 역행(10000→9000, delta -1000 < -250ms): 10ms로 클램프 — 단조 증가 보장.
    SrsSharedPtrMessage* m3 = mock_shared_msg(RTMP_MSG_VideoMessage, 9000, MOCK_VIDEO_INTER, sizeof(MOCK_VIDEO_INTER));
    HELPER_EXPECT_SUCCESS(jitter.correct(m3, SrsRtmpJitterAlgorithmFULL));
    EXPECT_EQ(60, (int)m3->timestamp);
    EXPECT_EQ(60, (int)jitter.get_time());
    srs_freep(m3);

    // 메타데이터(비 A/V)는 0으로 고정.
    SrsSharedPtrMessage* m4 = mock_shared_msg(RTMP_MSG_AMF0DataMessage, 12345, MOCK_AUDIO_RAW, sizeof(MOCK_AUDIO_RAW));
    HELPER_EXPECT_SUCCESS(jitter.correct(m4, SrsRtmpJitterAlgorithmFULL));
    EXPECT_EQ(0, (int)m4->timestamp);
    srs_freep(m4);
}

// overflow 시 오래된 N개를 버리는 게 아니라 전체를 비우고 최신 시퀀스 헤더만 재주입.
VOID TEST(AppSourceTest, MessageQueueShrink)
{
    srs_error_t err = srs_success;

    SrsMessageQueue queue;
    queue.set_queue_size(100 * SRS_UTIME_MILLISECONDS);

    HELPER_EXPECT_SUCCESS(queue.enqueue(mock_shared_msg(RTMP_MSG_VideoMessage, 10, MOCK_VIDEO_SH, sizeof(MOCK_VIDEO_SH))));
    HELPER_EXPECT_SUCCESS(queue.enqueue(mock_shared_msg(RTMP_MSG_AudioMessage, 15, MOCK_AUDIO_SH, sizeof(MOCK_AUDIO_SH))));
    HELPER_EXPECT_SUCCESS(queue.enqueue(mock_shared_msg(RTMP_MSG_VideoMessage, 20, MOCK_VIDEO_KEY, sizeof(MOCK_VIDEO_KEY))));
    HELPER_EXPECT_SUCCESS(queue.enqueue(mock_shared_msg(RTMP_MSG_VideoMessage, 60, MOCK_VIDEO_INTER, sizeof(MOCK_VIDEO_INTER))));
    EXPECT_EQ(4, queue.size());

    // duration(10ms→200ms=190ms) > 100ms → shrink 발동.
    bool is_overflow = false;
    HELPER_EXPECT_SUCCESS(queue.enqueue(mock_shared_msg(RTMP_MSG_VideoMessage, 200, MOCK_VIDEO_INTER, sizeof(MOCK_VIDEO_INTER)), &is_overflow));
    EXPECT_TRUE(is_overflow);

    // 전체가 비워지고 비디오/오디오 시퀀스 헤더만 큐 끝 시각(200ms)으로 재주입된다.
    EXPECT_EQ(2, queue.size());
    SrsSharedPtrMessage* remains[2] = {NULL, NULL};
    int count = 0;
    HELPER_EXPECT_SUCCESS(queue.dump_packets(2, remains, count));
    ASSERT_EQ(2, count);
    EXPECT_TRUE(remains[0]->is_video() && SrsFlvVideo::sh(remains[0]->payload, remains[0]->size));
    EXPECT_TRUE(remains[1]->is_audio() && SrsFlvAudio::sh(remains[1]->payload, remains[1]->size));
    EXPECT_EQ(200, (int)remains[0]->timestamp);
    EXPECT_EQ(200, (int)remains[1]->timestamp);
    srs_freep(remains[0]);
    srs_freep(remains[1]);
}

// 키프레임이 오면 clear 후 재시작 — 항상 [마지막 키프레임..현재]를 유지.
VOID TEST(AppSourceTest, GopCacheClearOnKeyframe)
{
    srs_error_t err = srs_success;

    SrsGopCache cache;
    EXPECT_TRUE(cache.empty());

    SrsSharedPtrMessage* key1 = mock_shared_msg(RTMP_MSG_VideoMessage, 0, MOCK_VIDEO_KEY, sizeof(MOCK_VIDEO_KEY));
    SrsSharedPtrMessage* inter = mock_shared_msg(RTMP_MSG_VideoMessage, 40, MOCK_VIDEO_INTER, sizeof(MOCK_VIDEO_INTER));
    SrsSharedPtrMessage* audio = mock_shared_msg(RTMP_MSG_AudioMessage, 60, MOCK_AUDIO_RAW, sizeof(MOCK_AUDIO_RAW));
    SrsSharedPtrMessage* key2 = mock_shared_msg(RTMP_MSG_VideoMessage, 2000, MOCK_VIDEO_KEY, sizeof(MOCK_VIDEO_KEY));

    HELPER_EXPECT_SUCCESS(cache.cache(key1));
    HELPER_EXPECT_SUCCESS(cache.cache(inter));
    HELPER_EXPECT_SUCCESS(cache.cache(audio));
    EXPECT_EQ(3, (int)cache.gop_cache.size());

    // 새 키프레임 → 이전 GOP 전체 clear 후 새 키프레임부터 재시작.
    HELPER_EXPECT_SUCCESS(cache.cache(key2));
    EXPECT_EQ(1, (int)cache.gop_cache.size());
    EXPECT_FALSE(cache.empty());
    EXPECT_EQ(2000 * SRS_UTIME_MILLISECONDS, cache.start_time());
    EXPECT_FALSE(cache.pure_audio());

    srs_freep(key1);
    srs_freep(inter);
    srs_freep(audio);
    srs_freep(key2);
}

// 비디오가 없으면(순수 오디오) GOP 캐시는 동작하지 않는다.
VOID TEST(AppSourceTest, GopCachePureAudio)
{
    srs_error_t err = srs_success;

    SrsGopCache cache;

    SrsSharedPtrMessage* audio = mock_shared_msg(RTMP_MSG_AudioMessage, 0, MOCK_AUDIO_RAW, sizeof(MOCK_AUDIO_RAW));
    HELPER_EXPECT_SUCCESS(cache.cache(audio));
    EXPECT_TRUE(cache.empty());
    EXPECT_TRUE(cache.pure_audio());
    srs_freep(audio);
}

// dumps는 onMetaData → AAC 시퀀스 헤더 → AVC 시퀀스 헤더 순서 (audio를 video보다 먼저).
VOID TEST(AppSourceTest, MetaCacheDumpsOrder)
{
    srs_error_t err = srs_success;

    SrsRequest req;
    mock_request(&req);
    SrsLiveSource source;
    HELPER_EXPECT_SUCCESS(source.initialize(&req));

    SrsMetaCache meta;
    SrsSharedPtrMessage* vsh = mock_shared_msg(RTMP_MSG_VideoMessage, 100, MOCK_VIDEO_SH, sizeof(MOCK_VIDEO_SH));
    SrsSharedPtrMessage* ash = mock_shared_msg(RTMP_MSG_AudioMessage, 100, MOCK_AUDIO_SH, sizeof(MOCK_AUDIO_SH));
    HELPER_EXPECT_SUCCESS(meta.update_vsh(vsh));
    HELPER_EXPECT_SUCCESS(meta.update_ash(ash));
    srs_freep(vsh);
    srs_freep(ash);
    EXPECT_TRUE(meta.vsh() && meta.ash());
    EXPECT_TRUE(meta.data() == NULL);

    SrsLiveConsumer* consumer = NULL;
    HELPER_EXPECT_SUCCESS(source.create_consumer(consumer));
    HELPER_EXPECT_SUCCESS(meta.dumps(consumer, SrsRtmpJitterAlgorithmFULL, true, true));

    SrsMessageArray msgs(16);
    int count = 0;
    HELPER_EXPECT_SUCCESS(consumer->dump_packets(&msgs, count));
    ASSERT_EQ(2, count);
    EXPECT_TRUE(msgs.msgs[0]->is_audio());
    EXPECT_TRUE(msgs.msgs[1]->is_video());
    msgs.free(count);

    srs_freep(consumer);
}

// publish 점유는 원자적: 두 번째 publisher는 ERROR_SYSTEM_STREAM_BUSY.
VOID TEST(AppSourceTest, LiveSourcePublishBusy)
{
    srs_error_t err = srs_success;

    SrsRequest req;
    mock_request(&req);
    SrsLiveSource source;
    HELPER_EXPECT_SUCCESS(source.initialize(&req));

    EXPECT_TRUE(source.can_publish());
    HELPER_EXPECT_SUCCESS(source.on_publish());
    EXPECT_FALSE(source.can_publish());

    err = source.on_publish();
    EXPECT_EQ(ERROR_SYSTEM_STREAM_BUSY, srs_error_code(err));
    srs_freep(err);

    source.on_unpublish();
    EXPECT_TRUE(source.can_publish());
}

// 팬아웃 + 중간 입장 프리필: 시퀀스 헤더는 MetaCache로(GOP 제외), 새 consumer는
// consumer_dumps로 [ash, vsh, GOP]를 받고, 이후 라이브 메시지를 팬아웃으로 받는다.
VOID TEST(AppSourceTest, LiveSourceFanoutAndMidJoin)
{
    srs_error_t err = srs_success;

    SrsRequest req;
    mock_request(&req);
    SrsLiveSource source;
    HELPER_EXPECT_SUCCESS(source.initialize(&req));
    HELPER_EXPECT_SUCCESS(source.on_publish());

    // publisher가 시퀀스 헤더와 GOP를 밀어 넣는다.
    SrsCommonMessage m;
    mock_common_msg(&m, RTMP_MSG_VideoMessage, 0, MOCK_VIDEO_SH, sizeof(MOCK_VIDEO_SH));
    HELPER_EXPECT_SUCCESS(source.on_video(&m));

    SrsCommonMessage m2;
    mock_common_msg(&m2, RTMP_MSG_AudioMessage, 0, MOCK_AUDIO_SH, sizeof(MOCK_AUDIO_SH));
    HELPER_EXPECT_SUCCESS(source.on_audio(&m2));

    SrsCommonMessage m3;
    mock_common_msg(&m3, RTMP_MSG_VideoMessage, 40, MOCK_VIDEO_KEY, sizeof(MOCK_VIDEO_KEY));
    HELPER_EXPECT_SUCCESS(source.on_video(&m3));

    SrsCommonMessage m4;
    mock_common_msg(&m4, RTMP_MSG_VideoMessage, 80, MOCK_VIDEO_INTER, sizeof(MOCK_VIDEO_INTER));
    HELPER_EXPECT_SUCCESS(source.on_video(&m4));

    // 시퀀스 헤더는 MetaCache에만, GOP 캐시에는 [키프레임, 인터]만.
    EXPECT_TRUE(source.meta->vsh() && source.meta->ash());
    EXPECT_EQ(2, (int)source.gop_cache->gop_cache.size());

    // 중간 입장: 프리필 = ash + vsh + GOP 2개 = 4개, 0부터 시작하는 타임라인.
    SrsLiveConsumer* late = NULL;
    HELPER_EXPECT_SUCCESS(source.create_consumer(late));
    HELPER_EXPECT_SUCCESS(source.consumer_dumps(late));

    SrsMessageArray msgs(16);
    int count = 0;
    HELPER_EXPECT_SUCCESS(late->dump_packets(&msgs, count));
    ASSERT_EQ(4, count);
    EXPECT_TRUE(msgs.msgs[0]->is_audio() && SrsFlvAudio::sh(msgs.msgs[0]->payload, msgs.msgs[0]->size));
    EXPECT_TRUE(msgs.msgs[1]->is_video() && SrsFlvVideo::sh(msgs.msgs[1]->payload, msgs.msgs[1]->size));
    EXPECT_TRUE(msgs.msgs[2]->is_video() && SrsFlvVideo::keyframe(msgs.msgs[2]->payload, msgs.msgs[2]->size));
    EXPECT_TRUE(msgs.msgs[3]->is_video());
    // 지터 보정: GOP 재생분(과거 타임스탬프)도 0 기반의 단조 증가 타임라인으로 나간다.
    EXPECT_LE(0, (int)msgs.msgs[0]->timestamp);
    EXPECT_LE((int)msgs.msgs[2]->timestamp, (int)msgs.msgs[3]->timestamp);
    msgs.free(count);

    // 라이브 팬아웃: 새 메시지는 모든 consumer 큐로 들어간다 (payload는 무복사 공유).
    SrsCommonMessage m5;
    mock_common_msg(&m5, RTMP_MSG_VideoMessage, 120, MOCK_VIDEO_INTER, sizeof(MOCK_VIDEO_INTER));
    HELPER_EXPECT_SUCCESS(source.on_video(&m5));

    count = 0;
    HELPER_EXPECT_SUCCESS(late->dump_packets(&msgs, count));
    ASSERT_EQ(1, count);
    EXPECT_TRUE(msgs.msgs[0]->is_video());
    msgs.free(count);

    // consumer 해제 후에도 publish는 계속된다.
    srs_freep(late);
    EXPECT_EQ(0, (int)source.consumers.size());

    SrsCommonMessage m6;
    mock_common_msg(&m6, RTMP_MSG_VideoMessage, 160, MOCK_VIDEO_INTER, sizeof(MOCK_VIDEO_INTER));
    HELPER_EXPECT_SUCCESS(source.on_video(&m6));

    // unpublish는 GOP만 비우고 시퀀스 헤더는 유지한다 (재-publish 대비).
    source.on_unpublish();
    EXPECT_TRUE(source.gop_cache->empty());
    EXPECT_TRUE(source.meta->vsh() != NULL);
}

// fetch_or_create는 같은 url이면 같은 소스를 돌려준다.
VOID TEST(AppSourceTest, SourceManagerFetchOrCreate)
{
    srs_error_t err = srs_success;

    SrsLiveSourceManager manager;

    SrsRequest req;
    mock_request(&req);
    req.stream = "manager-test";

    SrsLiveSource* s1 = NULL;
    HELPER_EXPECT_SUCCESS(manager.fetch_or_create(&req, &s1));
    ASSERT_TRUE(s1 != NULL);

    SrsLiveSource* s2 = NULL;
    HELPER_EXPECT_SUCCESS(manager.fetch_or_create(&req, &s2));
    EXPECT_TRUE(s1 == s2);
    EXPECT_TRUE(manager.fetch(&req) == s1);

    SrsRequest other;
    mock_request(&other);
    other.stream = "another";
    EXPECT_TRUE(manager.fetch(&other) == NULL);
}
