// srs_simple — 원본: trunk/src/app/srs_app_hls.cpp (1,800줄+)
// segment_open:386, is_segment_overflow:536, flush_audio:577, do_segment_close:648,
// refresh_m3u8:759, SrsHlsController::write_audio:1014/write_video:1065/reap_segment:1106,
// SrsHls::on_audio:1352/on_video:1433.
#include <srs_app_hls.hpp>

#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include <sstream>

#include <srs_app_config.hpp>
#include <srs_app_source.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_file.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_kernel_log.hpp>
#include <srs_kernel_stream.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_protocol_utility.hpp>

using namespace std;

#define srs_max(a, b) (((a) < (b))? (b) : (a))

// The LF char (원본: kernel/srs_kernel_consts.hpp — consts 파일이 없어 여기 정의).
#define SRS_CONSTS_LF '\n'

// The minimal duration of a ts segment: 이보다 짧으면 재생할 데이터가 부족해 드롭.
// (원본: srs_app_hls.cpp:41)
#define SRS_HLS_SEGMENT_MIN_DURATION (100 * SRS_UTIME_MILLISECONDS)

SrsHlsSegment::SrsHlsSegment(SrsTsContext* c, SrsAudioCodecId ac, SrsVideoCodecId vc, SrsFileWriter* w)
{
    sequence_no = 0;
    writer = w;
    tscw = new SrsTsContextWriter(w, c, ac, vc);
}

SrsHlsSegment::~SrsHlsSegment()
{
    srs_freep(tscw);
}

SrsHlsMuxer::SrsHlsMuxer()
{
    req = NULL;
    hls_fragment = hls_window = 0;
    hls_aof_ratio = 1.0;
    hls_cleanup = true;
    hls_wait_keyframe = true;
    max_td = 0;
    _sequence_no = 0;
    latest_acodec_ = SrsAudioCodecIdForbidden;
    latest_vcodec_ = SrsVideoCodecIdForbidden;
    current = NULL;
    segments = new SrsFragmentWindow();
    context = new SrsTsContext();
    writer = new SrsFileWriter();
}

SrsHlsMuxer::~SrsHlsMuxer()
{
    srs_freep(current);
    srs_freep(segments);
    srs_freep(context);
    srs_freep(writer);
    srs_freep(req);
}

int SrsHlsMuxer::sequence_no()
{
    return _sequence_no;
}

srs_error_t SrsHlsMuxer::initialize()
{
    return srs_success;
}

srs_error_t SrsHlsMuxer::update_config(SrsRequest* r)
{
    srs_error_t err = srs_success;

    srs_freep(req);
    req = r->copy();

    // 원본은 vhost 설정에서 읽는다 — 여기서는 상수 config (CLAUDE.md §5.4).
    hls_fragment = _srs_config->hls_fragment;
    hls_window = _srs_config->hls_window;
    hls_aof_ratio = _srs_config->hls_aof_ratio;
    hls_cleanup = _srs_config->hls_cleanup;
    hls_wait_keyframe = _srs_config->hls_wait_keyframe;
    hls_path = _srs_config->hls_path;
    hls_ts_file = _srs_config->hls_ts_file;

    // 원본은 max_td = fragment * td_ratio (기본 1.0) — ratio 제거.
    max_td = hls_fragment;

    // The m3u8 file path: hls_path/[app]/[stream].m3u8
    string m3u8_file = srs_path_build_stream(_srs_config->hls_m3u8_file, req->vhost, req->app, req->stream);
    m3u8 = hls_path + "/" + m3u8_file;

    // The dir of m3u8 in template, ts uri를 m3u8 기준 상대 경로로 만들 때 뗀다.
    size_t pos = m3u8_file.rfind('/');
    m3u8_dir = (pos != string::npos) ? m3u8_file.substr(0, pos) : "";

    // Create the dir for m3u8 (ts도 같은 템플릿 구조라 대부분 같은 디렉터리).
    SrsFragment m3u8_fragment;
    m3u8_fragment.set_path(m3u8);
    if ((err = m3u8_fragment.create_dir()) != srs_success) {
        return srs_error_wrap(err, "create m3u8 dir");
    }

    return err;
}

srs_error_t SrsHlsMuxer::segment_open()
{
    srs_error_t err = srs_success;

    if (current) {
        srs_warn("ignore the segment open, for segment is already open.");
        return err;
    }

    // load the default acodec/vcodec. 원본은 config(hls_acodec/hls_vcodec)에서 읽지만
    // 상수로 대체 — 스트림에서 실제 코덱을 본 뒤에는 그것을 쓴다.
    SrsAudioCodecId default_acodec = SrsAudioCodecIdAAC;
    if (latest_acodec_ != SrsAudioCodecIdForbidden) {
        default_acodec = latest_acodec_;
    }
    SrsVideoCodecId default_vcodec = SrsVideoCodecIdAVC;
    if (latest_vcodec_ != SrsVideoCodecIdForbidden) {
        default_vcodec = latest_vcodec_;
    }

    // new segment.
    current = new SrsHlsSegment(context, default_acodec, default_vcodec, writer);
    current->sequence_no = _sequence_no++;

    // generate filename.
    string ts_file = srs_path_build_stream(hls_ts_file, req->vhost, req->app, req->stream);
    if (true) {
        std::stringstream ss;
        ss << current->sequence_no;
        ts_file = srs_string_replace(ts_file, "[seq]", ss.str());
    }
    current->set_path(hls_path + "/" + ts_file);

    // the ts url, relative to the m3u8 dir.
    current->uri = ts_file;
    if (!m3u8_dir.empty() && ts_file.compare(0, m3u8_dir.length() + 1, m3u8_dir + "/") == 0) {
        current->uri = ts_file.substr(m3u8_dir.length() + 1);
    }

    // create dir recursively for ts file.
    if ((err = current->create_dir()) != srs_success) {
        return srs_error_wrap(err, "create dir");
    }

    // open temp ts file. 완성되면 rename — 플레이어가 미완성 세그먼트를 받지 않게.
    string tmp_file = current->tmppath();
    if ((err = current->writer->open(tmp_file)) != srs_success) {
        return srs_error_wrap(err, "open hls muxer");
    }

    // reset the context for a new sequence header + PAT/PMT.
    // HLS 세그먼트는 독립 디코딩 가능해야 한다 (CLAUDE.md §4 세 캐시와 같은 원리).
    context->reset();

    return err;
}

srs_error_t SrsHlsMuxer::on_sequence_header()
{
    srs_assert(current);

    // set the current segment to sequence header,
    // when close the segement, it will write a discontinuity to m3u8 file.
    current->set_sequence_header(true);

    return srs_success;
}

bool SrsHlsMuxer::is_segment_overflow()
{
    srs_assert(current);

    // to prevent very small segment.
    if (current->duration() < 2 * SRS_HLS_SEGMENT_MIN_DURATION) {
        return false;
    }

    // 원본은 hls_ts_floor의 deviation 보정이 있으나 floor 모드 제거 (CLAUDE.md §5.6 S10).
    return current->duration() >= max_td;
}

bool SrsHlsMuxer::wait_keyframe()
{
    return hls_wait_keyframe;
}

bool SrsHlsMuxer::is_segment_absolutely_overflow()
{
    srs_assert(current);

    // to prevent very small segment.
    if (current->duration() < 2 * SRS_HLS_SEGMENT_MIN_DURATION) {
        return false;
    }

    return current->duration() >= hls_aof_ratio * hls_fragment;
}

SrsAudioCodecId SrsHlsMuxer::latest_acodec()
{
    // 세그먼트가 열려 있으면 그 라이터의 코덱이 진실이다.
    if (current && current->tscw) return current->tscw->acodec();
    return latest_acodec_;
}

void SrsHlsMuxer::set_latest_acodec(SrsAudioCodecId v)
{
    if (current && current->tscw) current->tscw->set_acodec(v);
    latest_acodec_ = v;
}

SrsVideoCodecId SrsHlsMuxer::latest_vcodec()
{
    if (current && current->tscw) return current->tscw->vcodec();
    return latest_vcodec_;
}

void SrsHlsMuxer::set_latest_vcodec(SrsVideoCodecId v)
{
    if (current && current->tscw) current->tscw->set_vcodec(v);
    latest_vcodec_ = v;
}

srs_error_t SrsHlsMuxer::flush_audio(SrsTsMessageCache* cache)
{
    srs_error_t err = srs_success;

    // if current is NULL, segment is not open, ignore the flush event.
    if (!current) {
        srs_warn("flush audio ignored, for segment is not open.");
        return err;
    }

    if (!cache->audio || cache->audio->payload->length() <= 0) {
        return err;
    }

    // update the duration of segment.
    update_duration(cache->audio->dts);

    if ((err = current->tscw->write_audio(cache->audio)) != srs_success) {
        return srs_error_wrap(err, "hls: write audio");
    }

    // write success, clear and free the msg
    srs_freep(cache->audio);

    return err;
}

srs_error_t SrsHlsMuxer::flush_video(SrsTsMessageCache* cache)
{
    srs_error_t err = srs_success;

    // if current is NULL, segment is not open, ignore the flush event.
    if (!current) {
        srs_warn("flush video ignored, for segment is not open.");
        return err;
    }

    if (!cache->video || cache->video->payload->length() <= 0) {
        return err;
    }

    // update the duration of segment.
    update_duration(cache->video->dts);

    if ((err = current->tscw->write_video(cache->video)) != srs_success) {
        return srs_error_wrap(err, "hls: write video");
    }

    // write success, clear and free the msg
    srs_freep(cache->video);

    return err;
}

void SrsHlsMuxer::update_duration(uint64_t dts)
{
    // dts는 90kHz — ms로 바꿔 세그먼트 duration을 갱신.
    current->append(dts / 90);
}

srs_error_t SrsHlsMuxer::segment_close()
{
    srs_error_t err = do_segment_close();

    // We always cleanup current segment.
    srs_freep(current);

    return err;
}

srs_error_t SrsHlsMuxer::on_unpublish()
{
    return segment_close();
}

srs_error_t SrsHlsMuxer::do_segment_close()
{
    srs_error_t err = srs_success;

    if (!current) {
        srs_warn("ignore the segment close, for segment is not open.");
        return err;
    }

    // We should always close the underlayer writer.
    if (current && current->writer) {
        current->writer->close();
    }

    // valid, add to segments if segment duration is ok
    // when too small, it maybe not enough data to play.
    // when too large, it maybe timestamp corrupt.
    // make the segment more acceptable, when in [min, max_td * 3], it's ok.
    bool matchMinDuration = current->duration() >= SRS_HLS_SEGMENT_MIN_DURATION;
    bool matchMaxDuration = current->duration() <= max_td * 3 * 1000;
    if (matchMinDuration && matchMaxDuration) {
        // rename from tmp to real path
        if ((err = current->rename()) != srs_success) {
            return srs_error_wrap(err, "rename");
        }

        // 원본은 여기서 on_hls/on_hls_notify 훅을 async로 실행 — 훅 제거 (CLAUDE.md §5.6 S10).

        // close the muxer of finished segment.
        srs_freep(current->tscw);

        segments->append(current);
        current = NULL;
    } else {
        // reuse current segment index.
        _sequence_no--;

        srs_trace("Drop ts segment, sequence_no=%d, uri=%s, duration=%dms",
            current->sequence_no, current->uri.c_str(), srsu2msi(current->duration()));

        // rename from tmp to real path
        if ((err = current->unlink_tmpfile()) != srs_success) {
            return srs_error_wrap(err, "rename");
        }
    }

    // shrink the segments.
    segments->shrink(hls_window);

    // refresh the m3u8, donot contains the removed ts
    err = refresh_m3u8();

    // remove the ts file.
    segments->clear_expired(hls_cleanup);

    // check ret of refresh m3u8
    if (err != srs_success) {
        return srs_error_wrap(err, "hls: refresh m3u8");
    }

    return err;
}

srs_error_t SrsHlsMuxer::refresh_m3u8()
{
    srs_error_t err = srs_success;

    // no segments, also no m3u8, return.
    if (segments->empty()) {
        return err;
    }

    // 임시 파일에 쓰고 rename — 플레이어가 절대 반쯤 쓰인 m3u8을 읽지 않게 (원자적 교체).
    std::string temp_m3u8 = m3u8 + ".temp";
    if ((err = _refresh_m3u8(temp_m3u8)) == srs_success) {
        if (::rename(temp_m3u8.c_str(), m3u8.c_str()) < 0) {
            err = srs_error_new(ERROR_SYSTEM_FILE_RENAME, "hls: rename m3u8 file failed. %s => %s", temp_m3u8.c_str(), m3u8.c_str());
        }
    }

    // remove the temp file.
    if (srs_path_exists(temp_m3u8)) {
        if (::unlink(temp_m3u8.c_str()) < 0) {
            srs_warn("ignore remove m3u8 failed, %s", temp_m3u8.c_str());
        }
    }

    return err;
}

srs_error_t SrsHlsMuxer::_refresh_m3u8(string m3u8_file)
{
    srs_error_t err = srs_success;

    // no segments, return.
    if (segments->empty()) {
        return err;
    }

    SrsFileWriter fw;
    if ((err = fw.open(m3u8_file)) != srs_success) {
        return srs_error_wrap(err, "hls: open m3u8 file %s", m3u8_file.c_str());
    }

    // #EXTM3U\n
    // #EXT-X-VERSION:3\n
    std::stringstream ss;
    ss << "#EXTM3U" << SRS_CONSTS_LF;
    ss << "#EXT-X-VERSION:3" << SRS_CONSTS_LF;

    // #EXT-X-MEDIA-SEQUENCE — 윈도우에서 밀려난 세그먼트를 클라이언트가 알 수 있게
    // 첫 세그먼트의 시퀀스 번호를 싣는다.
    SrsHlsSegment* first = dynamic_cast<SrsHlsSegment*>(segments->first());
    if (first == NULL) {
        return srs_error_new(ERROR_SYSTEM_FILE_WRITE, "segments cast");
    }
    ss << "#EXT-X-MEDIA-SEQUENCE:" << first->sequence_no << SRS_CONSTS_LF;

    // #EXT-X-TARGETDURATION — 스펙: 모든 세그먼트 duration 이상, 정수(올림), 변하면 안 됨.
    srs_utime_t max_duration = segments->max_duration();
    int target_duration = (int)ceil(srsu2msi(srs_max(max_duration, max_td)) / 1000.0);
    ss << "#EXT-X-TARGETDURATION:" << target_duration << SRS_CONSTS_LF;

    // write all segments
    for (int i = 0; i < segments->size(); i++) {
        SrsHlsSegment* segment = dynamic_cast<SrsHlsSegment*>(segments->at(i));

        if (segment->is_sequence_header()) {
            // #EXT-X-DISCONTINUITY — 시퀀스 헤더(코덱/해상도 변경 후보)가 있던 세그먼트.
            ss << "#EXT-X-DISCONTINUITY" << SRS_CONSTS_LF;
        }

        // "#EXTINF:10.000, no desc\n"
        ss.precision(3);
        ss.setf(std::ios::fixed, std::ios::floatfield);
        ss << "#EXTINF:" << srsu2msi(segment->duration()) / 1000.0 << ", no desc" << SRS_CONSTS_LF;

        // {file name}\n
        ss << segment->uri << SRS_CONSTS_LF;
    }

    // write m3u8 to writer.
    std::string content = ss.str();
    if ((err = fw.write((char*)content.c_str(), (int)content.length(), NULL)) != srs_success) {
        return srs_error_wrap(err, "hls: write m3u8");
    }

    return err;
}

SrsHlsController::SrsHlsController()
{
    tsmc = new SrsTsMessageCache();
    muxer = new SrsHlsMuxer();
}

SrsHlsController::~SrsHlsController()
{
    srs_freep(muxer);
    srs_freep(tsmc);
}

srs_error_t SrsHlsController::initialize()
{
    srs_error_t err = muxer->initialize();
    if (err != srs_success) {
        return srs_error_wrap(err, "hls muxer initialize");
    }
    return srs_success;
}

int SrsHlsController::sequence_no()
{
    return muxer->sequence_no();
}

srs_error_t SrsHlsController::on_publish(SrsRequest* req)
{
    srs_error_t err = srs_success;

    if ((err = muxer->update_config(req)) != srs_success) {
        return srs_error_wrap(err, "hls: update config");
    }

    if ((err = muxer->segment_open()) != srs_success) {
        return srs_error_wrap(err, "hls: segment open");
    }

    srs_trace("hls: win=%dms, frag=%dms, cleanup=%d, wait_keyframe=%d, aof=%.2f",
        srsu2msi(_srs_config->hls_window), srsu2msi(_srs_config->hls_fragment),
        _srs_config->hls_cleanup, _srs_config->hls_wait_keyframe, _srs_config->hls_aof_ratio);

    return err;
}

srs_error_t SrsHlsController::on_unpublish()
{
    srs_error_t err = srs_success;

    // 남은 캐시를 마지막 세그먼트에 밀어 넣고 닫는다.
    if ((err = muxer->flush_audio(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush audio");
    }
    if ((err = muxer->flush_video(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush video");
    }

    if ((err = muxer->on_unpublish()) != srs_success) {
        return srs_error_wrap(err, "hls: on unpublish");
    }

    return err;
}

srs_error_t SrsHlsController::on_sequence_header()
{
    // 시퀀스 헤더는 세그먼트에 쓰지 않는다 — TS에서는 IDR 앞에 SPS/PPS를 재삽입하고
    // (srs_kernel_ts의 do_cache_avc), m3u8에는 DISCONTINUITY 후보로 마킹만 한다.
    return muxer->on_sequence_header();
}

srs_error_t SrsHlsController::write_audio(SrsAudioFrame* frame, int64_t dts)
{
    srs_error_t err = srs_success;

    // Refresh the codec ASAP.
    if (muxer->latest_acodec() != frame->acodec()->id) {
        muxer->set_latest_acodec(frame->acodec()->id);
    }

    // write audio to cache.
    if ((err = tsmc->cache_audio(frame, dts)) != srs_success) {
        return srs_error_wrap(err, "hls: cache audio");
    }

    // First, update the duration of the segment, as we might reap the segment. The duration should
    // cover from the first frame to the last frame.
    muxer->update_duration(tsmc->audio->dts);

    // reap when current source is pure audio: 키프레임이 없어 비디오 쪽 컷이
    // 영영 안 오므로, hls_aof_ratio 배를 넘기면 오디오에서라도 자른다.
    // (A/V 스트림에서는 aof(2.1배)보다 비디오 컷(1배)이 먼저 오므로 발동하지 않는다)
    if (tsmc->audio && muxer->is_segment_absolutely_overflow()) {
        if ((err = reap_segment()) != srs_success) {
            return srs_error_wrap(err, "hls: reap segment");
        }
    }

    // directly write the audio frame by frame to ts (원본의 pure-audio 집계 최적화 제거).
    if ((err = muxer->flush_audio(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush audio");
    }

    return err;
}

srs_error_t SrsHlsController::write_video(SrsVideoFrame* frame, int64_t dts)
{
    srs_error_t err = srs_success;

    // Refresh the codec ASAP.
    if (muxer->latest_vcodec() != frame->vcodec()->id) {
        muxer->set_latest_vcodec(frame->vcodec()->id);
    }

    // write video to cache.
    if ((err = tsmc->cache_video(frame, dts)) != srs_success) {
        return srs_error_wrap(err, "hls: cache video");
    }

    // First, update the duration of the segment, as we might reap the segment. The duration should
    // cover from the first frame to the last frame.
    muxer->update_duration(tsmc->video->dts);

    // when segment overflow, reap if possible.
    if (muxer->is_segment_overflow()) {
        // do reap ts if any of:
        //      a. wait keyframe and got keyframe.
        //      b. always reap when not wait keyframe.
        if (!muxer->wait_keyframe() || frame->frame_type == SrsVideoAvcFrameTypeKeyFrame) {
            // reap the segment, which will also flush the video.
            if ((err = reap_segment()) != srs_success) {
                return srs_error_wrap(err, "hls: reap segment");
            }
        }
    }

    // flush video when got one
    if ((err = muxer->flush_video(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush video");
    }

    return err;
}

srs_error_t SrsHlsController::reap_segment()
{
    srs_error_t err = srs_success;

    // close current ts.
    if ((err = muxer->segment_close()) != srs_success) {
        // When close segment error, we must reopen it for next packet to write.
        srs_error_t r0 = muxer->segment_open();
        if (r0 != srs_success) {
            srs_warn("close segment err %s", srs_error_desc(r0).c_str());
            srs_freep(r0);
        }

        return srs_error_wrap(err, "hls: segment close");
    }

    // open new ts.
    if ((err = muxer->segment_open()) != srs_success) {
        return srs_error_wrap(err, "hls: segment open");
    }

    // segment open, flush video first.
    if ((err = muxer->flush_video(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush video");
    }

    // segment open, flush the audio.
    // @see: ngx_rtmp_hls_open_fragment
    /* start fragment with audio to make iPhone happy */
    if ((err = muxer->flush_audio(tsmc)) != srs_success) {
        return srs_error_wrap(err, "hls: flush audio");
    }

    return err;
}

SrsHls::SrsHls()
{
    req = NULL;
    hub = NULL;
    enabled = false;

    controller = new SrsHlsController();
}

SrsHls::~SrsHls()
{
    srs_freep(controller);
}

srs_error_t SrsHls::initialize(SrsOriginHub* h, SrsRequest* r)
{
    srs_error_t err = srs_success;

    hub = h;
    req = r;

    if ((err = controller->initialize()) != srs_success) {
        return srs_error_wrap(err, "controller initialize");
    }

    return err;
}

srs_error_t SrsHls::on_publish()
{
    srs_error_t err = srs_success;

    // update the hls time, for hls never init.
    if (!_srs_config->hls_enabled) {
        return err;
    }

    // if enabled, open the muxer.
    if ((err = controller->on_publish(req)) != srs_success) {
        return srs_error_wrap(err, "hls: on publish");
    }

    // ok, the hls can be dispose, or need to be dispose.
    enabled = true;

    return err;
}

void SrsHls::on_unpublish()
{
    srs_error_t err = srs_success;

    // support multiple unpublish.
    if (!enabled) {
        return;
    }

    if ((err = controller->on_unpublish()) != srs_success) {
        srs_warn("hls: ignore unpublish failed %s", srs_error_desc(err).c_str());
        srs_freep(err);
    }

    enabled = false;
}

srs_error_t SrsHls::on_audio(SrsSharedPtrMessage* shared_audio, SrsFormat* format)
{
    srs_error_t err = srs_success;

    if (!enabled) {
        return err;
    }

    // 아직 어떤 오디오도 파싱 전이거나 AAC가 아니면 무시 (HLS는 AAC만).
    if (!format->acodec || !format->audio) {
        return err;
    }
    if (format->acodec->id != SrsAudioCodecIdAAC) {
        return err;
    }

    // ignore sequence header — TS에는 쓰지 않고 마킹만 (do_cache_aac가 ADTS로 대신한다).
    if (format->audio->aac_packet_type == SrsAudioAacFrameTraitSequenceHeader) {
        return controller->on_sequence_header();
    }

    // 샘플 없는 프레임은 무시 (빈 페이로드 등).
    if (format->audio->nb_samples == 0) {
        return err;
    }

    // 시퀀스 헤더 파싱 전의 raw 프레임은 디코딩 불가 — 드롭.
    if (!format->acodec->is_aac_codec_ok()) {
        return err;
    }

    // RTMP ms → 90kHz. 원본은 AAC 샘플 수 누적으로 dts를 재구성하지만(이슈 #547),
    // hls_dts_directly(원본 이슈 #1506) 방식으로 타임스탬프를 직접 쓴다 — CLAUDE.md §5.6 S10.
    int64_t dts = shared_audio->timestamp * 90;
    if ((err = controller->write_audio(format->audio, dts)) != srs_success) {
        return srs_error_wrap(err, "hls: write audio");
    }

    return err;
}

srs_error_t SrsHls::on_video(SrsSharedPtrMessage* shared_video, SrsFormat* format)
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

    // ignore sequence header — TS에는 쓰지 않고 마킹만 (do_cache_avc가 IDR 앞에 재삽입).
    if (format->video->avc_packet_type == SrsVideoAvcFrameTraitSequenceHeader) {
        return controller->on_sequence_header();
    }

    // NALU가 아니거나(end-of-sequence 등) 샘플이 없으면 무시 —
    // AUD만 있는 액세스 유닛이 TS에 나가면 디코더가 "missing picture"를 낸다.
    if (format->video->avc_packet_type != SrsVideoAvcFrameTraitNALU || format->video->nb_samples == 0) {
        return err;
    }

    // 시퀀스 헤더 파싱 전의 NALU는 디코딩 불가 — 드롭.
    if (!format->vcodec->is_avc_codec_ok()) {
        return err;
    }

    int64_t dts = shared_video->timestamp * 90;
    if ((err = controller->write_video(format->video, dts)) != srs_success) {
        return srs_error_wrap(err, "hls: write video");
    }

    return err;
}
