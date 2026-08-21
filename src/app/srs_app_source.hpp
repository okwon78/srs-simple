// srs_simple — 원본: trunk/src/app/srs_app_source.hpp
// 스트림 허브: 1 publisher → N player 팬아웃. 이 프로젝트의 두 번째 심장.
//   SrsRtmpJitter      — consumer별 타임스탬프 재구성 (위생 처리한 델타 누적)
//   SrsMessageQueue    — consumer 큐. overflow 시 shrink(전체 비우고 시퀀스 헤더만 재주입)
//   SrsLiveConsumer    — play 클라이언트 1개 = jitter + queue + cond wait
//   SrsGopCache        — 키프레임에서 clear 후 재시작. 항상 [마지막 키프레임..현재] 유지
//   SrsMetaCache       — onMetaData + AVC/AAC 시퀀스 헤더. 중간 입장 플레이어의 정합성
//   SrsOriginHub       — RTMP 밖으로 나가는 소비자(S10: HLS)로의 분기점
//   SrsLiveSource      — 스트림 1개의 허브. on_audio/on_video/on_meta_data → 팬아웃
//   SrsLiveSourceManager — "vhost/app/stream" → source 맵
// 원본 대비 제거: edge/bridge/mix_correct·atc/reload/통계/hourglass 소스 청소,
// OriginHub의 DVR·Forward·Transcode·HDS(HLS만 유지 — S10) — CLAUDE.md §5.6.
// pthread 전환: SrsLiveSource/SrsLiveConsumer에 내부 mutex (CLAUDE.md §5.1).
#ifndef SRS_APP_SOURCE_HPP
#define SRS_APP_SOURCE_HPP

#include <srs_core.hpp>

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <srs_kernel_log.hpp>

class SrsLiveSource;
class SrsLiveConsumer;
class SrsCommonMessage;
class SrsSharedPtrMessage;
class SrsMessageArray;
class SrsMessageHeader;
class SrsOnMetaDataPacket;
class SrsRequest;
class SrsFormat;
class SrsHls;

// The time jitter algorithm:
// 1. full, to ensure stream start at zero, and ensure stream monotonically increasing.
// 2. zero, only ensure stream start at zero, ignore timestamp jitter.
// 3. off, disable the time jitter algorithm, like atc.
enum SrsRtmpJitterAlgorithm
{
    SrsRtmpJitterAlgorithmFULL = 0x01,
    SrsRtmpJitterAlgorithmZERO,
    SrsRtmpJitterAlgorithmOFF
};

// Time jitter detect and correct, to ensure the rtmp stream is monotonically.
// consumer마다 개별 소유 — 입력 타임스탬프의 절대값을 믿지 않고 위생 처리한 델타를
// 누적해 0부터 시작하는 출력 타임라인을 재구성한다 (CLAUDE.md §4.2).
class SrsRtmpJitter
{
private:
    int64_t last_pkt_time;
    int64_t last_pkt_correct_time;
public:
    SrsRtmpJitter();
    virtual ~SrsRtmpJitter();
public:
    // detect the time jitter and correct it.
    // @param ag the algorithm to use for time jitter.
    virtual srs_error_t correct(SrsSharedPtrMessage* msg, SrsRtmpJitterAlgorithm ag);
    // Get current client time, the last packet time.
    virtual int64_t get_time();
};

// The message queue for the consumer(client).
// We limit the size in seconds, drop old messages(the whole gop) if full.
class SrsMessageQueue
{
private:
    // The start and end time.
    srs_utime_t av_start_time;
    srs_utime_t av_end_time;
private:
    // Whether do logging when shrinking.
    bool _ignore_shrink;
    // The max queue size, shrink if exceed it.
    srs_utime_t max_queue_size;
    std::vector<SrsSharedPtrMessage*> msgs;
public:
    SrsMessageQueue(bool ignore_shrink = false);
    virtual ~SrsMessageQueue();
public:
    // Get the size of queue.
    virtual int size();
    // Get the duration of queue.
    virtual srs_utime_t duration();
    // Set the queue size
    // @param queue_size the queue size in srs_utime_t.
    virtual void set_queue_size(srs_utime_t queue_size);
public:
    // Enqueue the message, the timestamp always monotonically.
    // @param msg, the msg to enqueue, user never free it whatever the return code.
    // @param is_overflow, whether overflow and shrinked. NULL to ignore.
    virtual srs_error_t enqueue(SrsSharedPtrMessage* msg, bool* is_overflow = NULL);
    // Get packets in consumer queue.
    // @pmsgs SrsSharedPtrMessage*[], used to store the msgs, user must alloc it.
    // @count the count in array, output param.
    // @max_count the max count to dequeue, must be positive.
    virtual srs_error_t dump_packets(int max_count, SrsSharedPtrMessage** pmsgs, int& count);
    // Dumps packets to consumer, use specified args.
    virtual srs_error_t dump_packets(SrsLiveConsumer* consumer, SrsRtmpJitterAlgorithm ag);
private:
    // 느린 소비자 정책: 오래된 N개를 버리는 게 아니라 전체를 비우고 최신 시퀀스
    // 헤더만 재주입 — 뒤처진 플레이어를 라이브 시점으로 스냅 (CLAUDE.md §4.2).
    virtual void shrink();
public:
    // clear all messages in queue.
    virtual void clear();
};

// The consumer for SrsLiveSource, that is a play client.
// pthread: publisher 스레드(enqueue)와 player 스레드(dump_packets/wait)가 공유하므로
// 내부 mutex + condition_variable로 보호한다 (원본 SRS_PERF_QUEUE_COND_WAIT의 pthread판).
class SrsLiveConsumer
{
private:
    // Because source references to this object, so we should directly use the source ptr.
    SrsLiveSource* source_;
private:
    SrsRtmpJitter* jitter;
    SrsMessageQueue* queue;
    // when source id changed, notice all consumers
    bool should_update_source_id;
private:
    // The cond wait for mw(merged-write).
    // 원본은 st_cond — pthread에서는 mutex + condition_variable (CLAUDE.md §5.1).
    std::mutex lock_;
    std::condition_variable mw_wait;
    bool mw_waiting;
    int mw_min_msgs;
    srs_utime_t mw_duration;
public:
    SrsLiveConsumer(SrsLiveSource* s);
    virtual ~SrsLiveConsumer();
public:
    // Set the size of queue.
    virtual void set_queue_size(srs_utime_t queue_size);
    // when source id changed, notice client to print.
    virtual void update_source_id();
public:
    // Get current client time, the last packet time.
    virtual int64_t get_time();
    // Enqueue an shared ptr message.
    // @param shared_msg, directly ptr, copy it if need to save it.
    // @param ag the algorithm of time jitter.
    virtual srs_error_t enqueue(SrsSharedPtrMessage* shared_msg, SrsRtmpJitterAlgorithm ag);
    // Get packets in consumer queue.
    // @param msgs the msgs array to dump packets to send.
    // @param count the count in array, intput and output param.
    // @remark user can specifies the count to get specified msgs; 0 to get all if possible.
    virtual srs_error_t dump_packets(SrsMessageArray* msgs, int& count);
    // wait for messages incomming, atleast nb_msgs and in duration.
    // 원본은 enqueue가 깨울 때까지 무한 대기(수신 코루틴이 별도 존재) — pthread에서는
    // 100ms 타임아웃으로 깨어나 호출자가 pull()/컨트롤 메시지를 재확인한다 (CLAUDE.md §5.1).
    // @param nb_msgs the messages count to wait.
    // @param msgs_duration the messages duration to wait.
    virtual void wait(int nb_msgs, srs_utime_t msgs_duration);
};

// cache a gop of video/audio data,
// delivery at the connect of flash player,
// To enable it to fast startup.
class SrsGopCache
{
private:
    // if disabled the gop cache,
    // The client will wait for the next keyframe for h264,
    // and will be black-screen.
    bool enable_gop_cache;
    // to limit the max gop cache frames
    // without this limit, if ingest stream always has no IDR frame
    // it will cause srs run out of memory
    int gop_cache_max_frames_;
    // The video frame count, avoid cache for pure audio stream.
    int cached_video_count;
    // when user disabled video when publishing, and gop cache enalbed,
    // We will cache the audio/video for we already got video, but we never
    // know when to clear the gop cache, for there is no video in future,
    // so we must guess whether user disabled the video.
    // when we got some audios after laster video, for instance, 600 audio packets,
    // about 3s(26ms per packet) 115 audio packets, clear gop cache.
    // @see: https://github.com/ossrs/srs/issues/124
    int audio_after_last_video_count;
    // cached gop.
    std::vector<SrsSharedPtrMessage*> gop_cache;
public:
    SrsGopCache();
    virtual ~SrsGopCache();
public:
    // To enable or disable the gop cache.
    virtual void set(bool v);
    virtual void set_gop_cache_max_frames(int v);
    virtual bool enabled();
    // only for h264 codec
    // 1. cache the gop when got h264 video packet.
    // 2. clear gop when got keyframe.
    // @param shared_msg, directly ptr, copy it if need to save it.
    virtual srs_error_t cache(SrsSharedPtrMessage* shared_msg);
    // clear the gop cache.
    virtual void clear();
    // dump the cached gop to consumer.
    virtual srs_error_t dump(SrsLiveConsumer* consumer, SrsRtmpJitterAlgorithm jitter_algorithm);
    virtual bool empty();
    // Get the start time of gop cache, in srs_utime_t.
    // @return 0 if no packets.
    virtual srs_utime_t start_time();
    // whether current stream is pure audio,
    // when no video in gop cache, the stream is pure audio right now.
    virtual bool pure_audio();
};

// Each stream have optional meta(sps/pps in sequence header and metadata).
// This class cache and update the meta.
// 중간 입장 플레이어가 즉시 디코딩을 시작할 수 있는 이유 (CLAUDE.md §4.2):
// onMetaData(해상도/코덱) + AVC 시퀀스 헤더(SPS/PPS) + AAC 시퀀스 헤더는
// publish 직후 한 번만 오므로, 캐시 없이는 늦게 온 플레이어가 영원히 못 받는다.
class SrsMetaCache
{
private:
    // The cached metadata, FLV script data tag.
    SrsSharedPtrMessage* meta;
    // The cached video sequence header, for example, sps/pps for h.264.
    SrsSharedPtrMessage* video;
    // The cached audio sequence header, for example, asc for aac.
    SrsSharedPtrMessage* audio;
public:
    SrsMetaCache();
    virtual ~SrsMetaCache();
public:
    // For each publishing, clear the metadata cache.
    virtual void clear();
public:
    // Get the cached metadata.
    virtual SrsSharedPtrMessage* data();
    // Get the cached vsh(video sequence header).
    virtual SrsSharedPtrMessage* vsh();
    // Get the cached ash(audio sequence header).
    virtual SrsSharedPtrMessage* ash();
    // Dumps cached metadata to consumer.
    // @param dm Whether dumps the metadata.
    // @param ds Whether dumps the sequence header.
    // audio를 video보다 먼저 — 원본 :1636 주석 (hls가 audio 코덱을 빨리 파싱하도록).
    virtual srs_error_t dumps(SrsLiveConsumer* consumer, SrsRtmpJitterAlgorithm ag, bool dm, bool ds);
public:
    // Update the cached metadata by packet.
    virtual srs_error_t update_data(SrsMessageHeader* header, SrsOnMetaDataPacket* metadata, bool& updated);
    // Update the cached audio sequence header.
    virtual srs_error_t update_ash(SrsSharedPtrMessage* msg);
    // Update the cached video sequence header.
    virtual srs_error_t update_vsh(SrsSharedPtrMessage* msg);
};

// The hub for origin: RTMP 팬아웃 밖의 소비자로 가는 분기점.
// 원본은 HLS/DVR/Forward/Transcode/HDS를 모두 물고 있으나 HLS만 유지 (CLAUDE.md §5.6 S10).
// 원본은 SrsLiveSource가 SrsRtmpFormat을 소유하고 hub에 넘기지만,
// 여기서는 hub가 SrsFormat을 직접 소유한다 — 코덱 파싱의 유일한 소비자가 HLS라서.
class SrsOriginHub
{
private:
    SrsLiveSource* source;
    SrsRequest* req_;
    bool is_active;
private:
    // The format, codec information (avcC의 SPS/PPS, AudioSpecificConfig 파싱 결과).
    SrsFormat* format;
    // hls handler.
    SrsHls* hls;
public:
    SrsOriginHub();
    virtual ~SrsOriginHub();
public:
    // Initialize the hub with source and request.
    // @param r The request object, managed by source.
    virtual srs_error_t initialize(SrsLiveSource* s, SrsRequest* r);
    // Whether the stream hub is active, or stream is publishing.
    virtual bool active();
public:
    // When got a parsed audio/video packet.
    virtual srs_error_t on_audio(SrsSharedPtrMessage* shared_audio);
    virtual srs_error_t on_video(SrsSharedPtrMessage* shared_video, bool is_sequence_header);
public:
    // When start publish stream.
    virtual srs_error_t on_publish();
    // When stop publish stream.
    virtual void on_unpublish();
};

// The source manager to create and refresh all stream sources.
// 원본은 SrsSharedPtr + hourglass 타이머로 죽은 소스를 청소하지만, 여기서는
// 생성된 소스를 프로세스 종료까지 유지한다 (CLAUDE.md §5.6 "S8 부수 단순화").
class SrsLiveSourceManager
{
private:
    std::mutex lock;
    std::map<std::string, SrsLiveSource*> pool;
public:
    SrsLiveSourceManager();
    virtual ~SrsLiveSourceManager();
public:
    //  create source when fetch from cache failed.
    // @param r the client request.
    // @param pps the matched source, if success never be NULL.
    virtual srs_error_t fetch_or_create(SrsRequest* r, SrsLiveSource** pps);
    // Get the exists source, NULL when not exists.
    virtual SrsLiveSource* fetch(SrsRequest* r);
};

// Global singleton instance.
extern SrsLiveSourceManager* _srs_sources;

// The live streaming source.
// publisher 스레드(on_audio/on_video/on_meta_data)와 player 스레드들(create_consumer/
// consumer_dumps/on_consumer_destroy)이 공유 — 내부 mutex 필요 (CLAUDE.md §5.1).
class SrsLiveSource
{
    friend class SrsLiveConsumer;
private:
    // For publish, it's the publish client id.
    // when source id changed, for example, the encoder reconnect,
    // invoke the on_source_id_changed() to let all clients know.
    SrsContextId _source_id;
    // previous source id.
    SrsContextId _pre_source_id;
    // deep copy of client request.
    SrsRequest* req;
    // To delivery stream to clients.
    std::vector<SrsLiveConsumer*> consumers;
    // The time jitter algorithm for vhost.
    SrsRtmpJitterAlgorithm jitter_algorithm;
    // The gop cache for client fast startup.
    SrsGopCache* gop_cache;
    // The metadata cache.
    SrsMetaCache* meta;
    // The hub for origin server: RTMP 밖(HLS)으로 가는 분기점 (S10).
    SrsOriginHub* hub;
private:
    // Whether source is avaiable for publishing.
    bool can_publish_;
    // pthread: 위 상태 전부를 보호한다. 락 순서는 항상 source → consumer.
    std::mutex lock_;
public:
    SrsLiveSource();
    virtual ~SrsLiveSource();
public:
    // Initialize the live source with request.
    virtual srs_error_t initialize(SrsRequest* r);
public:
    // Get current source id.
    virtual SrsContextId source_id();
    virtual SrsContextId pre_source_id();
public:
    virtual bool can_publish();
    virtual srs_error_t on_meta_data(SrsCommonMessage* msg, SrsOnMetaDataPacket* metadata);
public:
    virtual srs_error_t on_audio(SrsCommonMessage* audio);
private:
    virtual srs_error_t on_audio_imp(SrsSharedPtrMessage* audio);
public:
    virtual srs_error_t on_video(SrsCommonMessage* video);
private:
    virtual srs_error_t on_video_imp(SrsSharedPtrMessage* video);
private:
    // The source id changed. 호출자가 lock_을 잡고 있어야 한다.
    virtual srs_error_t on_source_id_changed(SrsContextId id);
public:
    // Publish stream event notify.
    // 원본은 conn의 acquire_publish가 can_publish 검사 후 호출하지만(단일 스레드라 원자적),
    // pthread에서는 검사+점유가 여기서 원자적으로 일어난다 — 이미 publish 중이면
    // ERROR_SYSTEM_STREAM_BUSY 반환 (CLAUDE.md §5.6).
    virtual srs_error_t on_publish();
    virtual void on_unpublish();
public:
    // Create consumer
    // @param consumer, output the create consumer.
    virtual srs_error_t create_consumer(SrsLiveConsumer*& consumer);
    // Dumps packets in cache to consumer.
    // ★ 반드시 송신 루프 시작 전에 호출 — 순서가 바뀌면 새 플레이어의 첫 바이트가
    // SPS/PPS 없는 GOP 중간이 된다 (CLAUDE.md §5.3).
    // @param ds, whether dumps the sequence header.
    // @param dm, whether dumps the metadata.
    // @param dg, whether dumps the gop cache.
    virtual srs_error_t consumer_dumps(SrsLiveConsumer* consumer, bool ds = true, bool dm = true, bool dg = true);
    virtual void on_consumer_destroy(SrsLiveConsumer* consumer);
    virtual void set_cache(bool enabled);
    virtual void set_gop_cache_max_frames(int v);
};

#endif
