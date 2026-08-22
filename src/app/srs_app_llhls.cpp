// srs_simple — 원본 SRS 대응물 없음. OME 구조 차용 (헤더 주석과 PLANS.md 참조).
#include <srs_app_llhls.hpp>

#include <math.h>

#include <chrono>
#include <sstream>

#include <srs_app_config.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_kernel_log.hpp>
#include <srs_kernel_mp4.hpp>
#include <srs_protocol_rtmp_stack.hpp>

using namespace std;

// 줄바꿈 (srs_app_hls.cpp와 동일 — 원본: kernel/srs_kernel_consts.hpp)
#define SRS_CONSTS_LF '\n'

// 파일-로컬 (srs_app_hls.cpp와 동일 이디엄 — 원본: kernel/srs_kernel_utility.hpp)
#define srs_max(a, b) (((a) < (b))? (b) : (a))

// EXT-X-PART를 나열하는 세그먼트 수 — 파트는 라이브 엣지 근처에만 싣는다 (스펙 요구,
// 그 이전 세그먼트는 EXTINF만). OME MakeChunklist의 `> last - 3`과 동일 — PLANS.md D5.
static const int SRS_LLHLS_PART_SEGMENTS = 3;

string srs_llhls_init_uri(const string& stream)
{
    return stream + "/init.mp4";
}

string srs_llhls_part_uri(const string& stream, int64_t msn, int psn)
{
    stringstream ss;
    ss << stream << "/" << msn << "." << psn << ".m4s";
    return ss.str();
}

string srs_llhls_segment_uri(const string& stream, int64_t msn)
{
    stringstream ss;
    ss << stream << "/" << msn << ".m4s";
    return ss.str();
}

SrsLlHlsBufferWriter::SrsLlHlsBufferWriter()
{
}

SrsLlHlsBufferWriter::~SrsLlHlsBufferWriter()
{
}

srs_error_t SrsLlHlsBufferWriter::write(void* buf, size_t size, ssize_t* nwrite)
{
    data.append((const char*)buf, size);
    if (nwrite) {
        *nwrite = (ssize_t)size;
    }
    return srs_success;
}

srs_error_t SrsLlHlsBufferWriter::writev(const iovec* iov, int iov_size, ssize_t* nwrite)
{
    ssize_t total = 0;
    for (int i = 0; i < iov_size; i++) {
        data.append((const char*)iov[i].iov_base, iov[i].iov_len);
        total += (ssize_t)iov[i].iov_len;
    }
    if (nwrite) {
        *nwrite = total;
    }
    return srs_success;
}

SrsLlHlsPart::SrsLlHlsPart()
{
    psn = 0;
    duration = 0;
    independent = false;
}

SrsLlHlsPart::~SrsLlHlsPart()
{
}

SrsLlHlsSegment::SrsLlHlsSegment(int64_t seq)
{
    msn = seq;
    duration = 0;
    completed = false;
}

SrsLlHlsSegment::~SrsLlHlsSegment()
{
    deque<SrsLlHlsPart*>::iterator it;
    for (it = parts.begin(); it != parts.end(); ++it) {
        SrsLlHlsPart* part = *it;
        srs_freep(part);
    }
    parts.clear();
}

SrsLlHlsChunklist::SrsLlHlsChunklist()
{
    part_target_ = 0;
    segment_target_ = 0;
}

SrsLlHlsChunklist::~SrsLlHlsChunklist()
{
}

void SrsLlHlsChunklist::update_config(const string& stream, srs_utime_t part_target, srs_utime_t segment_target)
{
    stream_ = stream;
    part_target_ = part_target;
    segment_target_ = segment_target;
}

string SrsLlHlsChunklist::generate(const deque<SrsLlHlsSegment*>& segments, bool active)
{
    // 세그먼트(=첫 파트)가 나오기 전에는 플레이리스트가 없다.
    if (segments.empty()) {
        return "";
    }

    stringstream ss;
    ss.precision(3);
    ss.setf(ios::fixed, ios::floatfield);

    SrsLlHlsSegment* last = segments.back();

    // TARGETDURATION — 스펙: 모든 EXTINF 이상, 정수(올림). 목표(segment_target)로
    // 시작하되, GOP > 세그먼트 길이로 실제가 넘치면 그만큼 키운다 (기존 TS 경로와 동일).
    srs_utime_t max_duration = segment_target_;
    for (deque<SrsLlHlsSegment*>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
        max_duration = srs_max(max_duration, (*it)->duration);
    }
    int target_duration = (int)ceil(srsu2msi(max_duration) / 1000.0);

    // 헤더부 — 태그 순서는 OME MakeChunklist 준수 (TASKS.md S14).
    ss << "#EXTM3U" << SRS_CONSTS_LF;
    // EXT-X-PART 등 LL-HLS 태그는 프로토콜 버전 6부터.
    ss << "#EXT-X-VERSION:6" << SRS_CONSTS_LF;
    ss << "#EXT-X-TARGETDURATION:" << target_duration << SRS_CONSTS_LF;
    // 블로킹 리로드 지원 선언 + 파트 홀드백 (스펙: ≥ 2×PART-TARGET, 권장 3× — D5).
    ss << "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,PART-HOLD-BACK="
       << srsu2msi(3 * part_target_) / 1000.0 << SRS_CONSTS_LF;
    ss << "#EXT-X-PART-INF:PART-TARGET=" << srsu2msi(part_target_) / 1000.0 << SRS_CONSTS_LF;
    // 윈도우에서 밀려난 세그먼트를 클라이언트가 알 수 있게 첫 세그먼트의 msn.
    ss << "#EXT-X-MEDIA-SEQUENCE:" << segments.front()->msn << SRS_CONSTS_LF;
    // fMP4 필수 — init 세그먼트. 해당 세그먼트들보다 먼저 등장해야 한다.
    ss << "#EXT-X-MAP:URI=\"" << srs_llhls_init_uri(stream_) << "\"" << SRS_CONSTS_LF;

    // 세그먼트부 — 오래된 것부터. 각 세그먼트에서 PART 나열이 EXTINF보다 먼저다(OME 순서).
    for (deque<SrsLlHlsSegment*>::const_iterator it = segments.begin(); it != segments.end(); ++it) {
        SrsLlHlsSegment* seg = *it;

        // 파트는 최근 SRS_LLHLS_PART_SEGMENTS개 세그먼트에만 — 그 이전은 EXTINF만.
        if (seg->msn > last->msn - SRS_LLHLS_PART_SEGMENTS) {
            for (deque<SrsLlHlsPart*>::const_iterator pit = seg->parts.begin(); pit != seg->parts.end(); ++pit) {
                SrsLlHlsPart* part = *pit;
                ss << "#EXT-X-PART:DURATION=" << srsu2msi(part->duration) / 1000.0
                   << ",URI=\"" << srs_llhls_part_uri(stream_, seg->msn, part->psn) << "\"";
                if (part->independent) {
                    ss << ",INDEPENDENT=YES";
                }
                ss << SRS_CONSTS_LF;
            }
        }

        // 완결 세그먼트만 EXTINF+URI — 진행 중 세그먼트는 PART만 (EXTINF는 close 후).
        if (seg->completed) {
            ss << "#EXTINF:" << srsu2msi(seg->duration) / 1000.0 << "," << SRS_CONSTS_LF;
            ss << srs_llhls_segment_uri(stream_, seg->msn) << SRS_CONSTS_LF;
        }
    }

    // PRELOAD-HINT — 다음에 생길 파트. 게시가 계속될 때만 싣는다 (unpublish 후에
    // 오지 않을 파트를 힌트하면 클라이언트가 헛되이 홀드된다). 힌트된 URI에 대한
    // GET은 리소스가 생길 때까지 홀드 후 200 (S15의 wait_for와 짝).
    if (active) {
        int64_t hint_msn = last->completed ? last->msn + 1 : last->msn;
        int hint_psn = last->completed ? 0 : (int)last->parts.size();
        ss << "#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\""
           << srs_llhls_part_uri(stream_, hint_msn, hint_psn) << "\"" << SRS_CONSTS_LF;
    }

    return ss.str();
}

SrsLlHlsStorage::SrsLlHlsStorage()
{
    active_ = false;
    max_segments_ = _srs_config->llhls_segment_count;
    next_msn_ = 0;
    latest_msn_ = -1;
    latest_psn_ = -1;

    chunklist_ = new SrsLlHlsChunklist();
}

SrsLlHlsStorage::~SrsLlHlsStorage()
{
    deque<SrsLlHlsSegment*>::iterator it;
    for (it = segments_.begin(); it != segments_.end(); ++it) {
        SrsLlHlsSegment* seg = *it;
        srs_freep(seg);
    }
    segments_.clear();

    srs_freep(chunklist_);
}

void SrsLlHlsStorage::update_config(int max_segments, const string& stream, srs_utime_t part_target, srs_utime_t segment_target)
{
    lock_guard<mutex> guard(lock_);
    max_segments_ = max_segments;
    chunklist_->update_config(stream, part_target, segment_target);
}

void SrsLlHlsStorage::on_publish()
{
    lock_guard<mutex> guard(lock_);
    active_ = true;
}

void SrsLlHlsStorage::on_unpublish()
{
    lock_guard<mutex> guard(lock_);

    active_ = false;

    // muxer가 마지막 파트를 close_segment로 밀어 넣지만, 파트가 열려 있지 않은 채로
    // unpublish가 오면 꼬리 세그먼트가 미완결로 남는다 — 여기서 닫는다 (EXTINF 확정).
    if (!segments_.empty() && !segments_.back()->completed && !segments_.back()->parts.empty()) {
        segments_.back()->completed = true;
    }

    // 꼬리 EXTINF 확정 + PRELOAD-HINT 제거(active=false)가 m3u8에 반영되게 재생성.
    refresh_playlist();

    // 홀드 중인 요청 전부 기상 — shutdown-wake 이디엄 (CLAUDE.md §5.6 S7과 동일 취지).
    cond_.notify_all();
}

bool SrsLlHlsStorage::active()
{
    lock_guard<mutex> guard(lock_);
    return active_;
}

void SrsLlHlsStorage::set_init(const string& v)
{
    lock_guard<mutex> guard(lock_);
    init_ = v;
    cond_.notify_all();
}

bool SrsLlHlsStorage::get_init(string& v)
{
    lock_guard<mutex> guard(lock_);
    if (init_.empty()) {
        return false;
    }
    v = init_;
    return true;
}

void SrsLlHlsStorage::append_part(const string& payload, srs_utime_t duration, bool independent, bool close_segment)
{
    lock_guard<mutex> guard(lock_);

    // 열린(미완결) 세그먼트가 없으면 새 msn으로 연다.
    SrsLlHlsSegment* seg = NULL;
    if (segments_.empty() || segments_.back()->completed) {
        seg = new SrsLlHlsSegment(next_msn_++);
        segments_.push_back(seg);
    } else {
        seg = segments_.back();
    }

    SrsLlHlsPart* part = new SrsLlHlsPart();
    part->psn = (int)seg->parts.size();
    part->duration = duration;
    part->independent = independent;
    part->payload = payload;

    seg->parts.push_back(part);
    seg->duration += duration;

    latest_msn_ = seg->msn;
    latest_psn_ = part->psn;

    if (close_segment) {
        seg->completed = true;
        // 윈도우 초과분 제거 — 파트는 부모 세그먼트 만료와 함께 사라진다 (스펙 요구).
        shrink();
    }

    // m3u8 재생성(캐시) — notify_all보다 먼저. 블로킹 리로드로 깨어난 HTTP 스레드가
    // 방금 게시된 파트가 실린 플레이리스트를 읽는다 (S14, TASKS.md).
    refresh_playlist();

    // 게시 통지 — wait_for(블로킹 리로드/프리로드 힌트 홀드, S15)가 깨어난다.
    cond_.notify_all();
}

bool SrsLlHlsStorage::get_playlist(string& v)
{
    lock_guard<mutex> guard(lock_);
    if (playlist_.empty()) {
        return false;
    }
    v = playlist_;
    return true;
}

bool SrsLlHlsStorage::get_part(int64_t msn, int psn, string& payload)
{
    lock_guard<mutex> guard(lock_);

    SrsLlHlsSegment* seg = find(msn);
    if (!seg || psn < 0 || psn >= (int)seg->parts.size()) {
        return false;
    }

    payload = seg->parts[psn]->payload;
    return true;
}

bool SrsLlHlsStorage::get_segment(int64_t msn, string& payload)
{
    lock_guard<mutex> guard(lock_);

    SrsLlHlsSegment* seg = find(msn);
    if (!seg || !seg->completed) {
        return false;
    }

    payload.clear();
    deque<SrsLlHlsPart*>::iterator it;
    for (it = seg->parts.begin(); it != seg->parts.end(); ++it) {
        payload.append((*it)->payload);
    }
    return true;
}

int64_t SrsLlHlsStorage::first_msn()
{
    lock_guard<mutex> guard(lock_);
    return segments_.empty() ? -1 : segments_.front()->msn;
}

int64_t SrsLlHlsStorage::latest_msn()
{
    lock_guard<mutex> guard(lock_);
    return latest_msn_;
}

bool SrsLlHlsStorage::latest(int64_t& msn, int& psn)
{
    lock_guard<mutex> guard(lock_);
    if (latest_msn_ < 0) {
        return false;
    }
    msn = latest_msn_;
    psn = latest_psn_;
    return true;
}

int SrsLlHlsStorage::size()
{
    lock_guard<mutex> guard(lock_);
    return (int)segments_.size();
}

bool SrsLlHlsStorage::wait_for(int64_t msn, int psn, srs_utime_t timeout)
{
    unique_lock<mutex> guard(lock_);

    // 게시(notify_all)/unpublish로 깨어나고, 조건 미충족이면 타임아웃까지 재대기.
    // wait_for의 predicate가 spurious wakeup을 걸러 준다.
    SrsLlHlsStorage* self = this;
    cond_.wait_for(guard, chrono::microseconds(timeout), [self, msn, psn]() {
        return !self->active_ || self->reached(msn, psn);
    });

    return reached(msn, psn);
}

bool SrsLlHlsStorage::reached(int64_t msn, int psn)
{
    if (latest_msn_ < 0) {
        return false;
    }
    // 이미 만료된 과거 msn — 기다릴 것이 없다 (호출자는 조회 실패로 404).
    if (!segments_.empty() && msn < segments_.front()->msn) {
        return true;
    }
    if (latest_msn_ != msn) {
        return latest_msn_ > msn;
    }
    return latest_psn_ >= (psn < 0 ? 0 : psn);
}

SrsLlHlsSegment* SrsLlHlsStorage::find(int64_t msn)
{
    deque<SrsLlHlsSegment*>::iterator it;
    for (it = segments_.begin(); it != segments_.end(); ++it) {
        if ((*it)->msn == msn) {
            return *it;
        }
    }
    return NULL;
}

void SrsLlHlsStorage::shrink()
{
    // back()은 진행 중 세그먼트일 수 있으므로 완결된 앞쪽만 밀어낸다.
    while ((int)segments_.size() > max_segments_ && segments_.front()->completed) {
        SrsLlHlsSegment* seg = segments_.front();
        segments_.pop_front();
        srs_freep(seg);
    }
}

void SrsLlHlsStorage::refresh_playlist()
{
    playlist_ = chunklist_->generate(segments_, active_);
}

SrsLlHlsMuxer::SrsLlHlsMuxer()
{
    req = NULL;
    storage = NULL;

    part_target = 0;
    segment_target = 0;

    enc = NULL;
    sequence_ = 0;

    part_open_ = false;
    part_start_dts = 0;
    last_dts = 0;
    part_nb_samples = 0;
    part_has_video = false;
    part_independent = false;
    segment_duration = 0;

    init_dirty_ = false;
    video_configured_ = false;
}

SrsLlHlsMuxer::~SrsLlHlsMuxer()
{
    srs_freep(enc);
}

srs_error_t SrsLlHlsMuxer::initialize(SrsLlHlsStorage* s)
{
    storage = s;
    return srs_success;
}

srs_error_t SrsLlHlsMuxer::update_config(SrsRequest* r)
{
    req = r;

    part_target = _srs_config->llhls_part;
    segment_target = _srs_config->llhls_segment;
    // 스트림 이름은 m3u8의 상대 URI({stream}/…)에 쓰인다 (S14 URI 헬퍼).
    storage->update_config(_srs_config->llhls_segment_count, req->stream, part_target, segment_target);

    // 파트 상태 리셋. msn(storage)과 mfhd sequence(muxer)는 재publish에도 이어진다.
    srs_freep(enc);
    writer.data.clear();
    part_open_ = false;
    part_nb_samples = 0;
    part_has_video = false;
    part_independent = false;
    segment_duration = 0;

    return srs_success;
}

srs_error_t SrsLlHlsMuxer::on_sequence_header(SrsFormat* format)
{
    srs_error_t err = srs_success;

    // 동일 시퀀스 헤더의 재전송은 무시 — 재전송마다 컷하면 세그먼트가 잘게 쪼개진다.
    bool changed = false;
    if (format->vcodec && format->vcodec->is_avc_codec_ok()
        && format->vcodec->avc_extra_data != latest_avc_extra_) {
        latest_avc_extra_ = format->vcodec->avc_extra_data;
        changed = true;
    }
    if (format->acodec && format->acodec->is_aac_codec_ok()
        && format->acodec->aac_extra_data != latest_aac_extra_) {
        latest_aac_extra_ = format->acodec->aac_extra_data;
        changed = true;
    }
    if (!changed) {
        return err;
    }

    // 진행 중 세그먼트를 닫는다 — 다른 코덱 설정의 샘플이 한 세그먼트(=한 EXT-X-MAP)에
    // 섞이지 않게 (OME FMP4Packager::UpdateTrack의 Flush + 세그먼트 완결).
    if (part_open_) {
        if ((err = flush_part(last_dts, true)) != srs_success) {
            return srs_error_wrap(err, "llhls: cut on sequence header");
        }
    }

    // init 재생성은 다음 파트 열 때(=세그먼트 선두) — open_part 참조.
    init_dirty_ = true;

    return err;
}

srs_error_t SrsLlHlsMuxer::write_audio(SrsFormat* format, int64_t dts)
{
    srs_error_t err = srs_success;

    // pure-audio 스트림은 모든 프레임이 independent라 세그먼트 컷도 오디오가 주도한다.
    // A/V 스트림에서는 키프레임(비디오) 컷만 세그먼트를 닫는다.
    if ((err = maybe_cut(dts, !video_configured_)) != srs_success) {
        return srs_error_wrap(err, "llhls: cut");
    }

    if (!part_open_) {
        if ((err = open_part(format, dts)) != srs_success) {
            return srs_error_wrap(err, "llhls: open part");
        }
    }

    // raw = FLV 태그 헤더 뒤의 AAC raw 프레임 — mdat에 그대로 실린다 (S12, PLANS.md).
    if ((err = enc->write_sample(SrsMp4HandlerTypeSOUN, 0x00,
        (uint32_t)dts, (uint32_t)dts, (uint8_t*)format->raw, (uint32_t)format->nb_raw)) != srs_success) {
        return srs_error_wrap(err, "llhls: write audio sample");
    }

    last_dts = dts;
    part_nb_samples++;

    return err;
}

srs_error_t SrsLlHlsMuxer::write_video(SrsFormat* format, int64_t dts)
{
    srs_error_t err = srs_success;

    bool keyframe = (format->video->frame_type == SrsVideoAvcFrameTypeKeyFrame);

    if ((err = maybe_cut(dts, keyframe)) != srs_success) {
        return srs_error_wrap(err, "llhls: cut");
    }

    if (!part_open_) {
        if ((err = open_part(format, dts)) != srs_success) {
            return srs_error_wrap(err, "llhls: open part");
        }
    }

    // raw = FLV 태그 헤더 뒤의 AVCC 페이로드 전체(길이 프리픽스 포함 NALU들).
    int64_t pts = dts + format->video->cts;
    if ((err = enc->write_sample(SrsMp4HandlerTypeVIDE, (uint16_t)format->video->frame_type,
        (uint32_t)dts, (uint32_t)pts, (uint8_t*)format->raw, (uint32_t)format->nb_raw)) != srs_success) {
        return srs_error_wrap(err, "llhls: write video sample");
    }

    // 파트의 첫 비디오 샘플이 키프레임이면 INDEPENDENT=YES.
    if (!part_has_video) {
        part_has_video = true;
        part_independent = keyframe;
    }

    last_dts = dts;
    part_nb_samples++;

    return err;
}

srs_error_t SrsLlHlsMuxer::on_unpublish()
{
    srs_error_t err = srs_success;

    // 남은 파트를 부모 세그먼트의 마지막 파트로 확정한다 (EXTINF 확정은 storage가).
    // end dts는 마지막 샘플 dts — 마지막 샘플 duration은 직전 간격을 재사용한다 (S12 flush).
    if (part_open_) {
        if ((err = flush_part(last_dts, true)) != srs_success) {
            return srs_error_wrap(err, "llhls: flush on unpublish");
        }
    }

    return err;
}

srs_error_t SrsLlHlsMuxer::maybe_cut(int64_t dts, bool can_end_segment)
{
    srs_error_t err = srs_success;

    if (!part_open_ || part_nb_samples == 0) {
        return err;
    }

    srs_utime_t part_dur = (dts - part_start_dts) * SRS_UTIME_MILLISECONDS;

    // 세그먼트 컷: 목표를 채웠고 지금 도착한 프레임이 새 세그먼트의 선두가 될 수 있을 때
    // (키프레임 또는 pure-audio). 파트 길이와 무관하게 우선한다 — 세그먼트 마지막 파트는
    // 짧아도 된다 (스펙의 "final Partial Segment" 예외).
    if (can_end_segment && segment_duration + part_dur >= segment_target) {
        return flush_part(dts, true);
    }

    // 파트 컷: 이 프레임까지의 duration이 target을 넘으면 자르고, 85%를 넘었고
    // 다음 프레임까지 넣으면 target을 넘길 것 같으면(직전 프레임 간격으로 추정) 미리
    // 자른다 — 파트가 PART-TARGET을 넘지 않게 (스펙 상한, OME AppendSample의 규칙).
    srs_utime_t frame_gap = (dts - last_dts) * SRS_UTIME_MILLISECONDS;
    if (part_dur >= part_target
        || (part_dur >= part_target * 85 / 100 && part_dur + frame_gap > part_target)) {
        return flush_part(dts, false);
    }

    return err;
}

srs_error_t SrsLlHlsMuxer::open_part(SrsFormat* format, int64_t dts)
{
    srs_error_t err = srs_success;

    // init 재생성은 세그먼트 선두에서만 — 한 세그먼트의 파트들은 같은 MAP을 공유해야
    // 한다. 컷 정책이 시퀀스 헤더 변경 시 세그먼트를 닫으므로 dirty면 여기는 항상
    // 세그먼트 선두다 (on_sequence_header 참조).
    if (init_dirty_) {
        if ((err = write_init(format)) != srs_success) {
            return srs_error_wrap(err, "llhls: write init");
        }
    }

    srs_freep(enc);
    writer.data.clear();

    enc = new SrsMp4M2tsSegmentEncoder();
    // tid=1: video traf가 1, audio traf는 2 (단독 트랙이면 그 traf가 1) — S12 D2 규칙.
    if ((err = enc->initialize(&writer, sequence_++, dts * SRS_UTIME_MILLISECONDS, 1)) != srs_success) {
        return srs_error_wrap(err, "llhls: init part encoder");
    }
    // audio traf의 track id는 파트 내용이 아니라 스트림 구성(init의 트랙)으로 정한다 —
    // muxed 스트림의 오디오 전용 파트(세그먼트 꼬리)가 비디오 트랙(1)에 실려
    // AAC가 h264로 디코딩되는 버그 방지 (S15에서 실스트림으로 발견).
    enc->set_audio_tid(video_configured_ ? 2 : 1);

    part_open_ = true;
    part_start_dts = dts;
    last_dts = dts;
    part_nb_samples = 0;
    part_has_video = false;
    part_independent = false;

    return err;
}

srs_error_t SrsLlHlsMuxer::flush_part(int64_t end_dts, bool close_segment)
{
    srs_error_t err = srs_success;

    // 빈 파트는 만들지 않는다 — m3u8에 실을 수 없다.
    if (!part_open_ || part_nb_samples == 0) {
        return err;
    }

    uint64_t dts = (uint64_t)end_dts;
    if ((err = enc->flush(dts)) != srs_success) {
        return srs_error_wrap(err, "llhls: flush part");
    }

    // 비디오 없는 파트는 pure-audio 스트림에서만 independent (AAC 프레임은 전부 독립).
    bool independent = part_has_video ? part_independent : !video_configured_;
    srs_utime_t duration = (end_dts - part_start_dts) * SRS_UTIME_MILLISECONDS;

    storage->append_part(writer.data, duration, independent, close_segment);

    if (close_segment) {
        segment_duration = 0;
    } else {
        segment_duration += duration;
    }

    srs_freep(enc);
    writer.data.clear();
    part_open_ = false;
    part_nb_samples = 0;
    part_has_video = false;
    part_independent = false;

    return err;
}

srs_error_t SrsLlHlsMuxer::write_init(SrsFormat* format)
{
    srs_error_t err = srs_success;

    bool has_video = format->vcodec && format->vcodec->is_avc_codec_ok();
    bool has_audio = format->acodec && format->acodec->is_aac_codec_ok();

    // 아직 아무 시퀀스 헤더도 파싱 전 — dirty를 유지하고 다음 기회에 다시 시도한다.
    // (SrsLlHls가 코덱 ok 전의 프레임을 드롭하므로 실제로는 도달하지 않는 방어선)
    if (!has_video && !has_audio) {
        return err;
    }

    SrsLlHlsBufferWriter w;
    SrsMp4M2tsInitEncoder init_enc;
    if ((err = init_enc.initialize(&w)) != srs_success) {
        return srs_error_wrap(err, "llhls: init encoder");
    }

    if (has_video && has_audio) {
        err = init_enc.write(format); // muxed: video tid=1 + audio tid=2 (D2)
    } else if (has_video) {
        err = init_enc.write(format, true, 1);
    } else {
        err = init_enc.write(format, false, 1);
    }
    if (err != srs_success) {
        return srs_error_wrap(err, "llhls: write init.mp4");
    }

    storage->set_init(w.data);
    init_dirty_ = false;
    video_configured_ = has_video;

    srs_trace("llhls: init.mp4 %dB, video=%d, audio=%d", (int)w.data.size(), has_video, has_audio);

    return err;
}

SrsLlHls::SrsLlHls()
{
    req = NULL;
    hub = NULL;
    enabled = false;

    storage_ = new SrsLlHlsStorage();
    muxer = new SrsLlHlsMuxer();
}

SrsLlHls::~SrsLlHls()
{
    srs_freep(muxer);
    srs_freep(storage_);
}

srs_error_t SrsLlHls::initialize(SrsOriginHub* h, SrsRequest* r)
{
    srs_error_t err = srs_success;

    hub = h;
    req = r;

    if ((err = muxer->initialize(storage_)) != srs_success) {
        return srs_error_wrap(err, "muxer initialize");
    }

    return err;
}

srs_error_t SrsLlHls::on_publish()
{
    srs_error_t err = srs_success;

    if (!_srs_config->llhls_enabled) {
        return err;
    }

    if ((err = muxer->update_config(req)) != srs_success) {
        return srs_error_wrap(err, "llhls: update config");
    }
    storage_->on_publish();

    enabled = true;

    srs_trace("llhls: part=%dms, segment=%dms, window=%d segments",
        srsu2msi(_srs_config->llhls_part), srsu2msi(_srs_config->llhls_segment),
        _srs_config->llhls_segment_count);

    return err;
}

void SrsLlHls::on_unpublish()
{
    srs_error_t err = srs_success;

    // support multiple unpublish.
    if (!enabled) {
        return;
    }

    if ((err = muxer->on_unpublish()) != srs_success) {
        srs_warn("llhls: ignore unpublish failed %s", srs_error_desc(err).c_str());
        srs_freep(err);
    }
    storage_->on_unpublish();

    enabled = false;
}

srs_error_t SrsLlHls::on_audio(SrsSharedPtrMessage* shared_audio, SrsFormat* format)
{
    srs_error_t err = srs_success;

    if (!enabled) {
        return err;
    }

    // 아직 어떤 오디오도 파싱 전이거나 AAC가 아니면 무시 (기존 SrsHls와 동일한 필터).
    if (!format->acodec || !format->audio) {
        return err;
    }
    if (format->acodec->id != SrsAudioCodecIdAAC) {
        return err;
    }

    // 시퀀스 헤더는 파트에 쓰지 않는다 — init.mp4(esds)로 간다.
    if (format->audio->aac_packet_type == SrsAudioAacFrameTraitSequenceHeader) {
        return muxer->on_sequence_header(format);
    }

    if (format->audio->nb_samples == 0) {
        return err;
    }

    // 시퀀스 헤더 파싱 전의 raw 프레임은 디코딩 불가 — 드롭.
    if (!format->acodec->is_aac_codec_ok()) {
        return err;
    }

    // dts는 RTMP ms 그대로 — fMP4 timescale=1000 (기존 TS 경로의 *90과 다르다, D5).
    if ((err = muxer->write_audio(format, shared_audio->timestamp)) != srs_success) {
        return srs_error_wrap(err, "llhls: write audio");
    }

    return err;
}

srs_error_t SrsLlHls::on_video(SrsSharedPtrMessage* shared_video, SrsFormat* format)
{
    srs_error_t err = srs_success;

    if (!enabled) {
        return err;
    }

    // 아직 어떤 비디오도 파싱 전이거나 H.264가 아니면 무시.
    if (!format->vcodec || !format->video) {
        return err;
    }
    if (format->vcodec->id != SrsVideoCodecIdAVC) {
        return err;
    }

    // 시퀀스 헤더는 파트에 쓰지 않는다 — init.mp4(avcC)로 간다.
    if (format->video->avc_packet_type == SrsVideoAvcFrameTraitSequenceHeader) {
        return muxer->on_sequence_header(format);
    }

    // NALU가 아니거나(end-of-sequence 등) 샘플이 없으면 무시 (기존 TS 경로와 동일 가드).
    if (format->video->avc_packet_type != SrsVideoAvcFrameTraitNALU || format->video->nb_samples == 0) {
        return err;
    }

    if (!format->vcodec->is_avc_codec_ok()) {
        return err;
    }

    if ((err = muxer->write_video(format, shared_video->timestamp)) != srs_success) {
        return srs_error_wrap(err, "llhls: write video");
    }

    return err;
}

SrsLlHlsStorage* SrsLlHls::storage()
{
    return storage_;
}
