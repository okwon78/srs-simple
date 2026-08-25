// srs_simple — 원본: trunk/src/app/srs_app_source.cpp (2,812줄)
// SrsRtmpJitter::correct:74, SrsMessageQueue::shrink:339, SrsLiveConsumer::enqueue:450/wait:527,
// SrsGopCache::cache:611, SrsMetaCache::dumps:1626, fetch_or_create:1769,
// on_video_imp:2408, consumer_dumps:2703.
#include <srs_app_source.hpp>

#include <string.h>

#include <algorithm>
#include <chrono>
#include <sstream>

#include <srs_app_config.hpp>
#include <srs_app_hls.hpp>
#include <srs_app_llhls.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_protocol_amf0.hpp>
#include <srs_protocol_rtmp_msg_array.hpp>
#include <srs_protocol_rtmp_stack.hpp>

using namespace std;

#define srs_min(a, b) (((a) < (b))? (a) : (b))
#define srs_max(a, b) (((a) < (b))? (b) : (a))

// jitter 판별 임계 (원본: srs_app_source.cpp:39-41)
#define CONST_MAX_JITTER_MS         250
#define CONST_MAX_JITTER_MS_NEG         -250
#define DEFAULT_FRAME_TIME_MS         10

// for 26ms per audio packet,
// 115 packets is 3s.
#define SRS_PURE_AUDIO_GUESS_COUNT 115

// consumer wait의 cond 타임아웃 — 리스너 poll/리소스 매니저와 같은 100ms 이디엄 (CLAUDE.md §5.1).
#define SRS_CONSUMER_WAIT_TIMEOUT_MS 100

SrsRtmpJitter::SrsRtmpJitter()
{
    last_pkt_correct_time = -1;
    last_pkt_time = 0;
}

SrsRtmpJitter::~SrsRtmpJitter()
{
}

srs_error_t SrsRtmpJitter::correct(SrsSharedPtrMessage* msg, SrsRtmpJitterAlgorithm ag)
{
    srs_error_t err = srs_success;

    // for performance reasons
    if (ag != SrsRtmpJitterAlgorithmFULL) {
        // all jitter correct features are disabled, ignore.
        if (ag == SrsRtmpJitterAlgorithmOFF) {
            return err;
        }

        // start at zero, but do not ensure monotonic increase.
        if (ag == SrsRtmpJitterAlgorithmZERO) {
            // for the first time, last_pkt_correct_time is -1.
            if (last_pkt_correct_time == -1) {
                last_pkt_correct_time = msg->timestamp;
            }
            msg->timestamp -= last_pkt_correct_time;
            return err;
        }

        // other algorithms, ignore.
        return err;
    }

    // full jitter algorithm, do the jitter correction.
    // set to 0 for metadata.
    if (!msg->is_av()) {
        msg->timestamp = 0;
        return err;
    }

    /**
     * we use a very simple time jitter detect/correct algorithm:
     * 1. delta: ensure the delta is positive and valid,
     *     we set the delta to DEFAULT_FRAME_TIME_MS,
     *     if the delta of time is negative or greater than CONST_MAX_JITTER_MS.
     * 2. last_pkt_time: specifies the original packet time,
     *     is used to detect the next jitter.
     * 3. last_pkt_correct_time: simply add the positive delta,
     *     and enforce that the time is monotonic.
     */
    int64_t time = msg->timestamp;
    int64_t delta = time - last_pkt_time;

    // if jitter detected, reset the delta.
    if (delta < CONST_MAX_JITTER_MS_NEG || delta > CONST_MAX_JITTER_MS) {
        // use the default 10ms to indicate the problem of the stream.
        // @see https://github.com/ossrs/srs/issues/425
        delta = DEFAULT_FRAME_TIME_MS;
    }

    last_pkt_correct_time = srs_max(0, last_pkt_correct_time + delta);

    msg->timestamp = last_pkt_correct_time;
    last_pkt_time = time;

    return err;
}

int64_t SrsRtmpJitter::get_time()
{
    return last_pkt_correct_time;
}

SrsMessageQueue::SrsMessageQueue(bool ignore_shrink)
{
    _ignore_shrink = ignore_shrink;
    max_queue_size = 0;
    av_start_time = av_end_time = -1;
}

SrsMessageQueue::~SrsMessageQueue()
{
    clear();
}

int SrsMessageQueue::size()
{
    return (int)msgs.size();
}

srs_utime_t SrsMessageQueue::duration()
{
    return (av_end_time - av_start_time);
}

void SrsMessageQueue::set_queue_size(srs_utime_t queue_size)
{
    max_queue_size = queue_size;
}

srs_error_t SrsMessageQueue::enqueue(SrsSharedPtrMessage* msg, bool* is_overflow)
{
    srs_error_t err = srs_success;

    msgs.push_back(msg);

    // If jitter is off, the timestamp of the first sequence header is zero, which will cause SRS to shrink and drop the
    // keyframes even if there are no overflow packets in the queue, so we must ignore the zero timestamps, please
    // @see https://github.com/ossrs/srs/pull/2186#issuecomment-953383063
    if (msg->is_av() && msg->timestamp != 0) {
        if (av_start_time == -1) {
            av_start_time = srs_utime_t(msg->timestamp * SRS_UTIME_MILLISECONDS);
        }

        av_end_time = srs_utime_t(msg->timestamp * SRS_UTIME_MILLISECONDS);
    }

    if (max_queue_size <= 0) {
        return err;
    }

    while (av_end_time - av_start_time > max_queue_size) {
        // notify the caller that the queue already overflowed and was shrunk.
        if (is_overflow) {
            *is_overflow = true;
        }

        shrink();
    }

    return err;
}

srs_error_t SrsMessageQueue::dump_packets(int max_count, SrsSharedPtrMessage** pmsgs, int& count)
{
    srs_error_t err = srs_success;

    int nb_msgs = (int)msgs.size();
    if (nb_msgs <= 0) {
        return err;
    }

    srs_assert(max_count > 0);
    count = srs_min(max_count, nb_msgs);

    SrsSharedPtrMessage** omsgs = msgs.data();
    memcpy(pmsgs, omsgs, count * sizeof(SrsSharedPtrMessage*));

    SrsSharedPtrMessage* last = omsgs[count - 1];
    av_start_time = srs_utime_t(last->timestamp * SRS_UTIME_MILLISECONDS);

    if (count >= nb_msgs) {
        // the pmsgs is big enough and clears msgs most of the time.
        msgs.clear();
    } else {
        // erasing some vector elements may cause a memory copy,
        // maybe we can use the more efficient vector.swap to avoid the copy.
        // @remark because the pmsgs is big enough, for instance, mw_msgs 128,
        //      the rtmp play client will get 128 msgs at once, so this branch rarely executes.
        msgs.erase(msgs.begin(), msgs.begin() + count);
    }

    return err;
}

srs_error_t SrsMessageQueue::dump_packets(SrsLiveConsumer* consumer, SrsRtmpJitterAlgorithm ag)
{
    srs_error_t err = srs_success;

    int nb_msgs = (int)msgs.size();
    if (nb_msgs <= 0) {
        return err;
    }

    SrsSharedPtrMessage** omsgs = msgs.data();
    for (int i = 0; i < nb_msgs; i++) {
        SrsSharedPtrMessage* msg = omsgs[i];
        if ((err = consumer->enqueue(msg, ag)) != srs_success) {
            return srs_error_wrap(err, "consume message");
        }
    }

    return err;
}

void SrsMessageQueue::shrink()
{
    SrsSharedPtrMessage* video_sh = NULL;
    SrsSharedPtrMessage* audio_sh = NULL;
    int msgs_size = (int)msgs.size();

    // Remove all msgs, mark the sequence headers.
    for (int i = 0; i < (int)msgs.size(); i++) {
        SrsSharedPtrMessage* msg = msgs.at(i);

        if (msg->is_video() && SrsFlvVideo::sh(msg->payload, msg->size)) {
            srs_freep(video_sh);
            video_sh = msg;
            continue;
        }
        else if (msg->is_audio() && SrsFlvAudio::sh(msg->payload, msg->size)) {
            srs_freep(audio_sh);
            audio_sh = msg;
            continue;
        }

        srs_freep(msg);
    }
    msgs.clear();

    // Update av_start_time, the start time of the queue.
    av_start_time = av_end_time;

    // Push back sequence headers and update their timestamps.
    if (video_sh) {
        video_sh->timestamp = srsu2ms(av_end_time);
        msgs.push_back(video_sh);
    }
    if (audio_sh) {
        audio_sh->timestamp = srsu2ms(av_end_time);
        msgs.push_back(audio_sh);
    }

    if (!_ignore_shrink) {
        srs_trace("shrinking, size=%d, removed=%d, max=%dms", (int)msgs.size(), msgs_size - (int)msgs.size(), srsu2msi(max_queue_size));
    }
}

void SrsMessageQueue::clear()
{
    std::vector<SrsSharedPtrMessage*>::iterator it;

    for (it = msgs.begin(); it != msgs.end(); ++it) {
        SrsSharedPtrMessage* msg = *it;
        srs_freep(msg);
    }

    msgs.clear();

    av_start_time = av_end_time = -1;
}

SrsLiveConsumer::SrsLiveConsumer(SrsLiveSource* s)
{
    source_ = s;
    jitter = new SrsRtmpJitter();
    queue = new SrsMessageQueue();
    should_update_source_id = false;

    mw_waiting = false;
    mw_min_msgs = 0;
    mw_duration = 0;
}

SrsLiveConsumer::~SrsLiveConsumer()
{
    source_->on_consumer_destroy(this);
    srs_freep(jitter);
    srs_freep(queue);
}

void SrsLiveConsumer::set_queue_size(srs_utime_t queue_size)
{
    std::lock_guard<std::mutex> guard(lock_);
    queue->set_queue_size(queue_size);
}

void SrsLiveConsumer::update_source_id()
{
    std::lock_guard<std::mutex> guard(lock_);
    should_update_source_id = true;
}

int64_t SrsLiveConsumer::get_time()
{
    std::lock_guard<std::mutex> guard(lock_);
    return jitter->get_time();
}

srs_error_t SrsLiveConsumer::enqueue(SrsSharedPtrMessage* shared_msg, SrsRtmpJitterAlgorithm ag)
{
    srs_error_t err = srs_success;

    SrsSharedPtrMessage* msg = shared_msg->copy();

    std::lock_guard<std::mutex> guard(lock_);

    if ((err = jitter->correct(msg, ag)) != srs_success) {
        return srs_error_wrap(err, "consume message");
    }

    if ((err = queue->enqueue(msg, NULL)) != srs_success) {
        return srs_error_wrap(err, "enqueue message");
    }

    // fire the mw when there are enough msgs.
    if (mw_waiting) {
        // For RTMP, we wait for messages and duration.
        srs_utime_t duration = queue->duration();
        bool match_min_msgs = queue->size() > mw_min_msgs;

        // when duration ok, signal to flush.
        if (match_min_msgs && duration > mw_duration) {
            mw_wait.notify_all();
            mw_waiting = false;
            return err;
        }
    }

    return err;
}

srs_error_t SrsLiveConsumer::dump_packets(SrsMessageArray* msgs, int& count)
{
    srs_error_t err = srs_success;

    srs_assert(count >= 0);
    srs_assert(msgs->max > 0);

    // the count is used as input to reset the max if positive.
    int max = count? srs_min(count, msgs->max) : msgs->max;

    // the count specifies the max acceptable count,
    // here it may be 1+, and we must set it to 0 when we got nothing.
    count = 0;

    std::lock_guard<std::mutex> guard(lock_);

    if (should_update_source_id) {
        srs_trace("update source_id=%s/%s", source_->source_id().c_str(), source_->pre_source_id().c_str());
        should_update_source_id = false;
    }

    // pump msgs from the queue.
    if ((err = queue->dump_packets(max, msgs->msgs, count)) != srs_success) {
        return srs_error_wrap(err, "dump packets");
    }

    return err;
}

void SrsLiveConsumer::wait(int nb_msgs, srs_utime_t msgs_duration)
{
    std::unique_lock<std::mutex> guard(lock_);

    mw_min_msgs = nb_msgs;
    mw_duration = msgs_duration;

    srs_utime_t duration = queue->duration();
    bool match_min_msgs = queue->size() > mw_min_msgs;

    // when duration ok, signal to flush.
    if (match_min_msgs && duration > mw_duration) {
        return;
    }

    // the enqueue will notify this cond.
    mw_waiting = true;

    // use a blocking cond wait for high performance mode.
    // 원본은 무한 대기(st_cond_wait) — pthread에서는 타임아웃으로 깨어나
    // 호출자가 pull()/컨트롤 메시지를 재확인한다 (CLAUDE.md §5.1).
    mw_wait.wait_for(guard, std::chrono::milliseconds(SRS_CONSUMER_WAIT_TIMEOUT_MS));
    mw_waiting = false;
}

SrsGopCache::SrsGopCache()
{
    cached_video_count = 0;
    enable_gop_cache = true;
    audio_after_last_video_count = 0;
    gop_cache_max_frames_ = 0;
}

SrsGopCache::~SrsGopCache()
{
    clear();
}

void SrsGopCache::set(bool v)
{
    enable_gop_cache = v;

    if (!v) {
        clear();
        return;
    }
}

void SrsGopCache::set_gop_cache_max_frames(int v)
{
    gop_cache_max_frames_ = v;
}

bool SrsGopCache::enabled()
{
    return enable_gop_cache;
}

srs_error_t SrsGopCache::cache(SrsSharedPtrMessage* shared_msg)
{
    srs_error_t err = srs_success;

    if (!enable_gop_cache) {
        return err;
    }

    // the gop cache knows when to gop it.
    SrsSharedPtrMessage* msg = shared_msg;

    // we got video, update the video count if acceptable
    if (msg->is_video()) {
        // Drop video when not h.264.
        if (!SrsFlvVideo::h264(msg->payload, msg->size)) {
            return err;
        }

        cached_video_count++;
        audio_after_last_video_count = 0;
    }

    // no acceptable video or pure audio, disable the cache.
    if (pure_audio()) {
        return err;
    }

    // ok, the gop cache is enabled, and we got an audio.
    if (msg->is_audio()) {
        audio_after_last_video_count++;
    }

    // clear the gop cache when the pure audio count overflows
    if (audio_after_last_video_count > SRS_PURE_AUDIO_GUESS_COUNT) {
        srs_warn("clear gop cache for guess pure audio overflow");
        clear();
        return err;
    }

    // clear the gop cache when we get a key frame
    if (msg->is_video() && SrsFlvVideo::keyframe(msg->payload, msg->size)) {
        clear();

        // the current msg is a video frame, so we set it to 1.
        cached_video_count = 1;
    }

    // cache the frame.
    gop_cache.push_back(msg->copy());

    // Clear the gop cache if it exceeds the max frames.
    if (gop_cache_max_frames_ > 0 && gop_cache.size() > (size_t)gop_cache_max_frames_) {
        srs_warn("Gop cache exceed max frames=%d, total=%d, videos=%d, aalvc=%d",
            gop_cache_max_frames_, (int)gop_cache.size(), cached_video_count, audio_after_last_video_count);
        clear();
    }

    return err;
}

void SrsGopCache::clear()
{
    std::vector<SrsSharedPtrMessage*>::iterator it;
    for (it = gop_cache.begin(); it != gop_cache.end(); ++it) {
        SrsSharedPtrMessage* msg = *it;
        srs_freep(msg);
    }
    gop_cache.clear();

    cached_video_count = 0;
    audio_after_last_video_count = 0;
}

srs_error_t SrsGopCache::dump(SrsLiveConsumer* consumer, SrsRtmpJitterAlgorithm jitter_algorithm)
{
    srs_error_t err = srs_success;

    std::vector<SrsSharedPtrMessage*>::iterator it;
    for (it = gop_cache.begin(); it != gop_cache.end(); ++it) {
        SrsSharedPtrMessage* msg = *it;
        if ((err = consumer->enqueue(msg, jitter_algorithm)) != srs_success) {
            return srs_error_wrap(err, "enqueue message");
        }
    }
    srs_trace("dispatch cached gop success. count=%d, duration=%d", (int)gop_cache.size(), (int)consumer->get_time());

    return err;
}

bool SrsGopCache::empty()
{
    return gop_cache.empty();
}

srs_utime_t SrsGopCache::start_time()
{
    if (empty()) {
        return 0;
    }

    SrsSharedPtrMessage* msg = gop_cache[0];
    srs_assert(msg);

    return srs_utime_t(msg->timestamp * SRS_UTIME_MILLISECONDS);
}

bool SrsGopCache::pure_audio()
{
    return cached_video_count == 0;
}

SrsMetaCache::SrsMetaCache()
{
    meta = video = audio = NULL;
}

SrsMetaCache::~SrsMetaCache()
{
    clear();
}

void SrsMetaCache::clear()
{
    srs_freep(meta);
    srs_freep(video);
    srs_freep(audio);
}

SrsSharedPtrMessage* SrsMetaCache::data()
{
    return meta;
}

SrsSharedPtrMessage* SrsMetaCache::vsh()
{
    return video;
}

SrsSharedPtrMessage* SrsMetaCache::ash()
{
    return audio;
}

srs_error_t SrsMetaCache::dumps(SrsLiveConsumer* consumer, SrsRtmpJitterAlgorithm ag, bool dm, bool ds)
{
    srs_error_t err = srs_success;

    // copy metadata.
    if (dm && meta && (err = consumer->enqueue(meta, ag)) != srs_success) {
        return srs_error_wrap(err, "enqueue metadata");
    }

    // copy sequence header
    // copy the audio sequence first, so that hls can quickly parse the "right" audio codec.
    // @see https://github.com/ossrs/srs/issues/301
    if (ds && audio && (err = consumer->enqueue(audio, ag)) != srs_success) {
        return srs_error_wrap(err, "enqueue audio sh");
    }

    if (ds && video && (err = consumer->enqueue(video, ag)) != srs_success) {
        return srs_error_wrap(err, "enqueue video sh");
    }

    return err;
}

srs_error_t SrsMetaCache::update_data(SrsMessageHeader* header, SrsOnMetaDataPacket* metadata, bool& updated)
{
    updated = false;

    srs_error_t err = srs_success;

    SrsAmf0Any* prop = NULL;

    // when the duration exists, remove it to make ExoPlayer happy.
    if (metadata->metadata->get_property("duration") != NULL) {
        metadata->metadata->remove("duration");
    }

    // generate metadata info to print
    std::stringstream ss;
    if ((prop = metadata->metadata->ensure_property_number("width")) != NULL) {
        ss << ", width=" << (int)prop->to_number();
    }
    if ((prop = metadata->metadata->ensure_property_number("height")) != NULL) {
        ss << ", height=" << (int)prop->to_number();
    }
    if ((prop = metadata->metadata->ensure_property_number("videocodecid")) != NULL) {
        ss << ", vcodec=" << (int)prop->to_number();
    }
    if ((prop = metadata->metadata->ensure_property_number("audiocodecid")) != NULL) {
        ss << ", acodec=" << (int)prop->to_number();
    }
    srs_trace("got metadata%s", ss.str().c_str());

    // add server info to metadata
    metadata->metadata->set("server", SrsAmf0Any::str(RTMP_SIG_SRS_SERVER));

    // version, for example, 1.0.0
    // add the version to the metadata, please do not remove it, for debugging.
    metadata->metadata->set("server_version", SrsAmf0Any::str(RTMP_SIG_SRS_VERSION));

    // encode the metadata to payload
    int size = 0;
    char* payload = NULL;
    if ((err = metadata->encode(size, payload)) != srs_success) {
        return srs_error_wrap(err, "encode metadata");
    }

    if (size <= 0) {
        srs_warn("ignore the invalid metadata. size=%d", size);
        return err;
    }

    // create a shared ptr message.
    srs_freep(meta);
    meta = new SrsSharedPtrMessage();
    updated = true;

    // dump message to shared ptr message.
    // the payload/size is managed by msg, the user should not free it.
    if ((err = meta->create(header, payload, size)) != srs_success) {
        return srs_error_wrap(err, "create metadata");
    }

    return err;
}

srs_error_t SrsMetaCache::update_ash(SrsSharedPtrMessage* msg)
{
    srs_freep(audio);
    audio = msg->copy();
    return srs_success;
}

srs_error_t SrsMetaCache::update_vsh(SrsSharedPtrMessage* msg)
{
    srs_freep(video);
    video = msg->copy();
    return srs_success;
}

SrsOriginHub::SrsOriginHub()
{
    source = NULL;
    req_ = NULL;
    is_active = false;

    format = new SrsFormat();
    hls = new SrsHls();
    llhls = new SrsLlHls();
}

SrsOriginHub::~SrsOriginHub()
{
    srs_freep(llhls);
    srs_freep(hls);
    srs_freep(format);
}

srs_error_t SrsOriginHub::initialize(SrsLiveSource* s, SrsRequest* r)
{
    srs_error_t err = srs_success;

    req_ = r;
    source = s;

    if ((err = format->initialize()) != srs_success) {
        return srs_error_wrap(err, "format initialize");
    }

    if ((err = hls->initialize(this, req_)) != srs_success) {
        return srs_error_wrap(err, "hls initialize");
    }

    if ((err = llhls->initialize(this, req_)) != srs_success) {
        return srs_error_wrap(err, "llhls initialize");
    }

    return err;
}

bool SrsOriginHub::active()
{
    return is_active;
}

SrsLlHlsStorage* SrsOriginHub::llhls_storage()
{
    return llhls->storage();
}

srs_error_t SrsOriginHub::on_audio(SrsSharedPtrMessage* shared_audio)
{
    srs_error_t err = srs_success;

    SrsSharedPtrMessage* msg = shared_audio;

    // 코덱 파싱 (시퀀스 헤더면 AudioSpecificConfig, 아니면 AAC raw 샘플).
    // 파싱 실패가 publish를 죽이지 않게 경고만 남기고 무시한다 (CLAUDE.md §5.6 S10).
    if ((err = format->on_audio(msg->timestamp, msg->payload, msg->size)) != srs_success) {
        srs_warn("hub: ignore audio format error %s", srs_error_desc(err).c_str());
        srs_error_reset(err);
        return err;
    }

    // HLS 오류도 publish를 죽이지 않는다 — 원본 hls_on_error의 'ignore' 전략으로 고정.
    if ((err = hls->on_audio(msg, format)) != srs_success) {
        srs_warn("hls: ignore audio error %s", srs_error_desc(err).c_str());
        hls->on_unpublish();
        srs_error_reset(err);
    }

    // LL-HLS도 같은 오류 전략 (S13).
    if ((err = llhls->on_audio(msg, format)) != srs_success) {
        srs_warn("llhls: ignore audio error %s", srs_error_desc(err).c_str());
        llhls->on_unpublish();
        srs_error_reset(err);
    }

    return err;
}

srs_error_t SrsOriginHub::on_video(SrsSharedPtrMessage* shared_video, bool is_sequence_header)
{
    srs_error_t err = srs_success;

    SrsSharedPtrMessage* msg = shared_video;

    // 코덱 파싱 (시퀀스 헤더면 avcC의 SPS/PPS, 아니면 NALU 샘플 목록).
    if ((err = format->on_video(msg->timestamp, msg->payload, msg->size)) != srs_success) {
        srs_warn("hub: ignore video format error %s", srs_error_desc(err).c_str());
        srs_error_reset(err);
        return err;
    }

    if ((err = hls->on_video(msg, format)) != srs_success) {
        srs_warn("hls: ignore video error %s", srs_error_desc(err).c_str());
        hls->on_unpublish();
        srs_error_reset(err);
    }

    // LL-HLS도 같은 오류 전략 (S13).
    if ((err = llhls->on_video(msg, format)) != srs_success) {
        srs_warn("llhls: ignore video error %s", srs_error_desc(err).c_str());
        llhls->on_unpublish();
        srs_error_reset(err);
    }

    return err;
}

srs_error_t SrsOriginHub::on_publish()
{
    srs_error_t err = srs_success;

    if ((err = hls->on_publish()) != srs_success) {
        return srs_error_wrap(err, "hls publish");
    }

    if ((err = llhls->on_publish()) != srs_success) {
        return srs_error_wrap(err, "llhls publish");
    }

    is_active = true;

    return err;
}

void SrsOriginHub::on_unpublish()
{
    is_active = false;

    hls->on_unpublish();
    llhls->on_unpublish();
}

// 원본은 main에서 생성/대입하지만, _srs_config와 같은 정적 초기화 이디엄을 쓴다.
static SrsLiveSourceManager _sources_instance;
SrsLiveSourceManager* _srs_sources = &_sources_instance;

SrsLiveSourceManager::SrsLiveSourceManager()
{
}

SrsLiveSourceManager::~SrsLiveSourceManager()
{
    std::map<std::string, SrsLiveSource*>::iterator it;
    for (it = pool.begin(); it != pool.end(); ++it) {
        SrsLiveSource* source = it->second;
        srs_freep(source);
    }
    pool.clear();
}

srs_error_t SrsLiveSourceManager::fetch_or_create(SrsRequest* r, SrsLiveSource** pps)
{
    srs_error_t err = srs_success;

    // Use lock to protect concurrent fetch/create between connections.
    // (원본 주석의 @bug #1230과 같은 목적 — 코루틴 스위치 대신 pthread 경쟁)
    std::lock_guard<std::mutex> guard(lock);

    string stream_url = r->get_stream_url();
    std::map<std::string, SrsLiveSource*>::iterator it = pool.find(stream_url);

    if (it != pool.end()) {
        *pps = it->second;
        return err;
    }

    SrsLiveSource* source = new SrsLiveSource();
    srs_trace("new live source, stream_url=%s", stream_url.c_str());

    if ((err = source->initialize(r)) != srs_success) {
        srs_freep(source);
        return srs_error_wrap(err, "init source %s", stream_url.c_str());
    }

    pool[stream_url] = source;
    *pps = source;
    return err;
}

SrsLiveSource* SrsLiveSourceManager::fetch(SrsRequest* r)
{
    std::lock_guard<std::mutex> guard(lock);

    string stream_url = r->get_stream_url();
    std::map<std::string, SrsLiveSource*>::iterator it = pool.find(stream_url);

    if (it == pool.end()) {
        return NULL;
    }

    return it->second;
}

SrsLiveSource* SrsLiveSourceManager::fetch(string app, string stream)
{
    std::lock_guard<std::mutex> guard(lock);

    // 키는 "vhost/app/stream"(기본 vhost면 "/app/stream") — vhost를 무시하고
    // "/app/stream" 접미사로 찾는다. 접미사가 '/'로 시작하므로 부분 일치 오탐 없음.
    string suffix = "/" + app + "/" + stream;
    std::map<std::string, SrsLiveSource*>::iterator it;
    for (it = pool.begin(); it != pool.end(); ++it) {
        const string& url = it->first;
        if (url.length() >= suffix.length()
            && url.compare(url.length() - suffix.length(), suffix.length(), suffix) == 0) {
            return it->second;
        }
    }

    return NULL;
}

SrsLiveSource::SrsLiveSource()
{
    req = NULL;
    // 원본 기본값 full (conf: time_jitter full).
    jitter_algorithm = SrsRtmpJitterAlgorithmFULL;

    can_publish_ = true;

    gop_cache = new SrsGopCache();
    meta = new SrsMetaCache();
    hub = new SrsOriginHub();
}

SrsLiveSource::~SrsLiveSource()
{
    // never free the consumers,
    // for all consumers are auto freed.
    consumers.clear();

    // hub가 req를 참조하므로 req보다 먼저 해제한다.
    srs_freep(hub);
    srs_freep(meta);
    srs_freep(gop_cache);

    srs_freep(req);
}

srs_error_t SrsLiveSource::initialize(SrsRequest* r)
{
    srs_error_t err = srs_success;

    srs_assert(!req);
    req = r->copy();

    if ((err = hub->initialize(this, req)) != srs_success) {
        return srs_error_wrap(err, "hub");
    }

    return err;
}

srs_error_t SrsLiveSource::on_source_id_changed(SrsContextId id)
{
    srs_error_t err = srs_success;

    if (_source_id == id) {
        return err;
    }

    if (_pre_source_id.empty()) {
        _pre_source_id = id;
    }
    _source_id = id;

    // notify all consumers
    std::vector<SrsLiveConsumer*>::iterator it;
    for (it = consumers.begin(); it != consumers.end(); ++it) {
        SrsLiveConsumer* consumer = *it;
        consumer->update_source_id();
    }

    return err;
}

SrsContextId SrsLiveSource::source_id()
{
    std::lock_guard<std::mutex> guard(lock_);
    return _source_id;
}

SrsContextId SrsLiveSource::pre_source_id()
{
    std::lock_guard<std::mutex> guard(lock_);
    return _pre_source_id;
}

bool SrsLiveSource::can_publish()
{
    std::lock_guard<std::mutex> guard(lock_);
    return can_publish_;
}

SrsLlHlsStorage* SrsLiveSource::llhls_storage()
{
    // hub 포인터는 생성자에서 만들어져 불변 — source lock_ 불필요 (hpp 주석 참조).
    return hub->llhls_storage();
}

srs_error_t SrsLiveSource::on_meta_data(SrsCommonMessage* msg, SrsOnMetaDataPacket* metadata)
{
    srs_error_t err = srs_success;

    std::lock_guard<std::mutex> guard(lock_);

    // Update the meta cache.
    bool updated = false;
    if ((err = meta->update_data(&msg->header, metadata, updated)) != srs_success) {
        return srs_error_wrap(err, "update metadata");
    }
    if (!updated) {
        return err;
    }

    // copy to all consumers
    std::vector<SrsLiveConsumer*>::iterator it;
    for (it = consumers.begin(); it != consumers.end(); ++it) {
        SrsLiveConsumer* consumer = *it;
        if ((err = consumer->enqueue(meta->data(), jitter_algorithm)) != srs_success) {
            return srs_error_wrap(err, "consume metadata");
        }
    }

    return err;
}

srs_error_t SrsLiveSource::on_audio(SrsCommonMessage* shared_audio)
{
    srs_error_t err = srs_success;

    // convert shared_audio to msg, the user should not use shared_audio again.
    // the payload is transferred to msg, and set to NULL in shared_audio.
    SrsSharedPtrMessage msg;
    if ((err = msg.create(shared_audio)) != srs_success) {
        return srs_error_wrap(err, "create message");
    }

    // 원본은 on_frame에서 mix_correct를 분기하지만, mix_correct 제거로 직행 (CLAUDE.md §1).
    std::lock_guard<std::mutex> guard(lock_);
    return on_audio_imp(&msg);
}

srs_error_t SrsLiveSource::on_audio_imp(SrsSharedPtrMessage* msg)
{
    srs_error_t err = srs_success;

    // Whether the current packet is a sequence header (AudioSpecificConfig).
    // 원본은 SrsFormat 파싱 결과를 쓰지만, kernel codec 판별자로 대체 (CLAUDE.md §5.6).
    bool is_sequence_header = SrsFlvAudio::sh(msg->payload, msg->size);

    // Copy to hub to all utilities (S10: 코덱 파싱 + HLS).
    if ((err = hub->on_audio(msg)) != srs_success) {
        return srs_error_wrap(err, "consume audio");
    }

    // copy to all consumers
    for (int i = 0; i < (int)consumers.size(); i++) {
        SrsLiveConsumer* consumer = consumers.at(i);
        if ((err = consumer->enqueue(msg, jitter_algorithm)) != srs_success) {
            return srs_error_wrap(err, "consume message");
        }
    }

    // Refresh the sequence header in the metadata.
    // 원본 주석: MP3는 시퀀스 헤더가 없어 첫 패킷을 대신 캐시한다.
    if (is_sequence_header || !meta->ash()) {
        if ((err = meta->update_ash(msg)) != srs_success) {
            return srs_error_wrap(err, "meta consume audio");
        }
    }

    // when it is a sequence header, do not push it to the gop cache and adjust the timestamp.
    if (is_sequence_header) {
        return err;
    }

    // cache the last gop packets
    if ((err = gop_cache->cache(msg)) != srs_success) {
        return srs_error_wrap(err, "gop cache consume audio");
    }

    return err;
}

srs_error_t SrsLiveSource::on_video(SrsCommonMessage* shared_video)
{
    srs_error_t err = srs_success;

    // convert shared_video to msg, the user should not use shared_video again.
    // the payload is transferred to msg, and set to NULL in shared_video.
    SrsSharedPtrMessage msg;
    if ((err = msg.create(shared_video)) != srs_success) {
        return srs_error_wrap(err, "create message");
    }

    std::lock_guard<std::mutex> guard(lock_);
    return on_video_imp(&msg);
}

srs_error_t SrsLiveSource::on_video_imp(SrsSharedPtrMessage* msg)
{
    srs_error_t err = srs_success;

    bool is_sequence_header = SrsFlvVideo::sh(msg->payload, msg->size);

    // cache the sequence header if h264
    // do not cache the sequence header to gop_cache, return here.
    if (is_sequence_header && (err = meta->update_vsh(msg)) != srs_success) {
        return srs_error_wrap(err, "meta update video");
    }

    // Copy to hub to all utilities (S10: 코덱 파싱 + HLS).
    if ((err = hub->on_video(msg, is_sequence_header)) != srs_success) {
        return srs_error_wrap(err, "hub consume video");
    }

    // copy to all consumers
    for (int i = 0; i < (int)consumers.size(); i++) {
        SrsLiveConsumer* consumer = consumers.at(i);
        if ((err = consumer->enqueue(msg, jitter_algorithm)) != srs_success) {
            return srs_error_wrap(err, "consume video");
        }
    }

    // when it is a sequence header, do not push it to the gop cache and adjust the timestamp.
    if (is_sequence_header) {
        return err;
    }

    // cache the last gop packets
    if ((err = gop_cache->cache(msg)) != srs_success) {
        return srs_error_wrap(err, "gop cache consume vdieo");
    }

    return err;
}

srs_error_t SrsLiveSource::on_publish()
{
    srs_error_t err = srs_success;

    // update the request object.
    srs_assert(req);

    std::lock_guard<std::mutex> guard(lock_);

    // Check whether RTMP stream is busy — 검사+점유를 원자적으로 (CLAUDE.md §5.6).
    if (!can_publish_) {
        return srs_error_new(ERROR_SYSTEM_STREAM_BUSY, "live source busy, url=%s", req->get_stream_url().c_str());
    }
    can_publish_ = false;

    // whatever, the publish thread is the source,
    // save its id to source id.
    if ((err = on_source_id_changed(_srs_context->get_id())) != srs_success) {
        return srs_error_wrap(err, "source id change");
    }

    // 허브(HLS) 기동 — 세그먼트 디렉터리 생성 실패 등은 publish 실패로 전파한다.
    if ((err = hub->on_publish()) != srs_success) {
        can_publish_ = true;
        return srs_error_wrap(err, "hub publish");
    }

    // Reset the metadata cache, to make VLC happy when disabling/enabling the stream.
    // @see https://github.com/ossrs/srs/issues/1630#issuecomment-597979448
    meta->clear();

    return err;
}

void SrsLiveSource::on_unpublish()
{
    std::lock_guard<std::mutex> guard(lock_);

    // ignore when already unpublished.
    if (can_publish_) {
        return;
    }

    // 허브(HLS) 정리 — 남은 캐시를 마지막 세그먼트에 밀어 넣고 닫는다.
    hub->on_unpublish();

    // only clear the gop cache,
    // do not clear the sequence header, for it may not have changed,
    // when dropping a dup sequence header, drop the metadata also.
    gop_cache->clear();

    srs_trace("cleanup when unpublish");

    if (!_source_id.empty()) {
        _pre_source_id = _source_id;
    }
    _source_id = SrsContextId();

    can_publish_ = true;
}

srs_error_t SrsLiveSource::create_consumer(SrsLiveConsumer*& consumer)
{
    srs_error_t err = srs_success;

    std::lock_guard<std::mutex> guard(lock_);

    consumer = new SrsLiveConsumer(this);
    consumers.push_back(consumer);

    return err;
}

srs_error_t SrsLiveSource::consumer_dumps(SrsLiveConsumer* consumer, bool ds, bool dm, bool dg)
{
    srs_error_t err = srs_success;

    std::lock_guard<std::mutex> guard(lock_);

    srs_utime_t queue_size = _srs_config->queue_length;
    consumer->set_queue_size(queue_size);

    // If the stream is publishing, dump the sequence header and gop cache.
    bool active = hub->active();
    if (active) {
        // Copy the metadata and sequence header to the consumer.
        if ((err = meta->dumps(consumer, jitter_algorithm, dm, ds)) != srs_success) {
            return srs_error_wrap(err, "meta dumps");
        }

        // copy the gop cache to the client.
        if (dg && (err = gop_cache->dump(consumer, jitter_algorithm)) != srs_success) {
            return srs_error_wrap(err, "gop cache dumps");
        }
    }

    // print status.
    if (dg) {
        srs_trace("create consumer, active=%d, queue_size=%dms, jitter=%d", active, srsu2msi(queue_size), jitter_algorithm);
    } else {
        srs_trace("create consumer, active=%d, ignore gop cache, jitter=%d", active, jitter_algorithm);
    }

    return err;
}

void SrsLiveSource::on_consumer_destroy(SrsLiveConsumer* consumer)
{
    std::lock_guard<std::mutex> guard(lock_);

    std::vector<SrsLiveConsumer*>::iterator it;
    it = std::find(consumers.begin(), consumers.end(), consumer);
    if (it != consumers.end()) {
        it = consumers.erase(it);
    }
}

void SrsLiveSource::set_cache(bool enabled)
{
    std::lock_guard<std::mutex> guard(lock_);
    gop_cache->set(enabled);
}

void SrsLiveSource::set_gop_cache_max_frames(int v)
{
    std::lock_guard<std::mutex> guard(lock_);
    gop_cache->set_gop_cache_max_frames(v);
}
