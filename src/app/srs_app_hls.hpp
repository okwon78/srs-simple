// srs_simple — 원본: trunk/src/app/srs_app_hls.hpp
// RTMP 라이브 스트림 → HLS(ts 세그먼트 + m3u8) 트랜스먹서.
//
//   SrsHls           SrsOriginHub가 부르는 진입점. 프레임을 컨트롤러로 넘긴다
//   SrsHlsController 세그먼트 컷 판단 (fragment 길이 도달 + 키프레임 대기)
//   SrsHlsMuxer      세그먼트 파일 열기/닫기, m3u8 재작성, 롤링 윈도우 관리
//   SrsHlsSegment    ts 세그먼트 1개 = SrsFragment + TS 컨텍스트 라이터
//
// 원본 대비 제거 (CLAUDE.md §5.6 S10): AES-128 암호화(hls_keys), hls_ts_floor,
// hls_entry_prefix, on_hls/on_hls_notify 훅, async reload/dispose 타이머, HEVC/MP3.
#ifndef SRS_APP_HLS_HPP
#define SRS_APP_HLS_HPP

#include <srs_core.hpp>

#include <string>

#include <srs_app_fragment.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_ts.hpp>

class SrsFileWriter;
class SrsRequest;
class SrsSharedPtrMessage;
class SrsFormat;
class SrsOriginHub;

// The wrapper of m3u8 segment: ts 세그먼트 1개.
class SrsHlsSegment : public SrsFragment
{
public:
    // sequence number in m3u8.
    int sequence_no;
    // ts uri in m3u8 (m3u8 파일 기준 상대 경로).
    std::string uri;
    // The file writer to write ts file, muxer가 소유하고 세그먼트 간 재사용한다.
    SrsFileWriter* writer;
    // The TS context writer to write TS to file.
    SrsTsContextWriter* tscw;
public:
    SrsHlsSegment(SrsTsContext* c, SrsAudioCodecId ac, SrsVideoCodecId vc, SrsFileWriter* w);
    virtual ~SrsHlsSegment();
};

// The hls muxer: 세그먼트 파일과 m3u8을 실제로 만드는 계층.
// 원본 주석: "muxer, the HLS muxer, group segments to m3u8."
class SrsHlsMuxer
{
private:
    SrsRequest* req;
private:
    // 설정 스냅샷 (원본은 update_config가 vhost 설정에서 읽는다 — 여기서는 _srs_config).
    srs_utime_t hls_fragment;
    srs_utime_t hls_window;
    double hls_aof_ratio;
    bool hls_cleanup;
    bool hls_wait_keyframe;
    std::string hls_path;
    std::string hls_ts_file;
    // The m3u8 file path and its dir prefix (ts uri 계산용).
    std::string m3u8;
    std::string m3u8_dir;
private:
    // The max duration of segment before reap (= hls_fragment; 원본은 *td_ratio).
    srs_utime_t max_td;
    int _sequence_no;
    // 스트림에서 마지막으로 본 코덱 — 새 세그먼트의 TS 컨텍스트 라이터에 물려준다.
    SrsAudioCodecId latest_acodec_;
    SrsVideoCodecId latest_vcodec_;
private:
    // The current writing segment. NULL when closed.
    SrsHlsSegment* current;
    // The ts segments in m3u8 (rolling window).
    SrsFragmentWindow* segments;
    // The TS context, 세그먼트를 넘어 공유 (continuity counter 유지).
    SrsTsContext* context;
    // The file writer, 세그먼트마다 close/reopen해서 재사용.
    SrsFileWriter* writer;
public:
    SrsHlsMuxer();
    virtual ~SrsHlsMuxer();
public:
    virtual int sequence_no();
    virtual srs_error_t initialize();
    // On publish, update the config and open the first segment.
    virtual srs_error_t update_config(SrsRequest* r);
    // Open a new segment (a new ts file).
    virtual srs_error_t segment_open();
    virtual srs_error_t on_sequence_header();
    // Whether segment overflow, 세그먼트가 hls_fragment를 넘겨 잘라야 하는지.
    virtual bool is_segment_overflow();
    // Whether to wait for a keyframe to reap the segment.
    virtual bool wait_keyframe();
    // Whether segment absolutely overflow (pure audio용 — 키프레임이 없어
    // 잘릴 기회가 없으므로 hls_aof_ratio 배를 넘으면 무조건 자른다).
    virtual bool is_segment_absolutely_overflow();
public:
    virtual SrsAudioCodecId latest_acodec();
    virtual void set_latest_acodec(SrsAudioCodecId v);
    virtual SrsVideoCodecId latest_vcodec();
    virtual void set_latest_vcodec(SrsVideoCodecId v);
    // Flush the cached audio/video into the current segment file.
    virtual srs_error_t flush_audio(SrsTsMessageCache* cache);
    virtual srs_error_t flush_video(SrsTsMessageCache* cache);
    // Update the segment duration by the frame dts.
    // @param dts in 90kHz.
    virtual void update_duration(uint64_t dts);
    // Close segment: rename tmp → ts, m3u8 재작성, 윈도우 shrink + 만료 파일 삭제.
    virtual srs_error_t segment_close();
    // On unpublish, close the current segment and keep the VoD m3u8.
    virtual srs_error_t on_unpublish();
private:
    virtual srs_error_t do_segment_close();
    virtual srs_error_t refresh_m3u8();
    virtual srs_error_t _refresh_m3u8(std::string m3u8_file);
};

// The hls stream cache: 프레임 흐름과 세그먼트 컷 판단.
// 원본 주석의 비유 그대로 — muxer가 "손"이면 controller는 "머리".
class SrsHlsController
{
private:
    // The HLS muxer to reap ts and m3u8.
    SrsHlsMuxer* muxer;
    // The TS cache: FLV 프레임 → PES 페이로드 변환 버퍼.
    SrsTsMessageCache* tsmc;
public:
    SrsHlsController();
    virtual ~SrsHlsController();
public:
    virtual srs_error_t initialize();
    virtual int sequence_no();
public:
    virtual srs_error_t on_publish(SrsRequest* req);
    virtual srs_error_t on_unpublish();
    // When got sequence header: 세그먼트에 쓰지 않고 마킹만 한다
    // (SPS/PPS는 TS에서 IDR 앞에 재삽입되고, m3u8엔 DISCONTINUITY 후보로 남는다).
    virtual srs_error_t on_sequence_header();
    // Write an audio frame to the cache and flush. @param dts in 90kHz.
    virtual srs_error_t write_audio(SrsAudioFrame* frame, int64_t dts);
    // Write a video frame to the cache and flush. @param dts in 90kHz.
    virtual srs_error_t write_video(SrsVideoFrame* frame, int64_t dts);
private:
    // Close the current segment and open a new one:
    // ★ 새 세그먼트에 video를 먼저 flush — 키프레임이 세그먼트 선두에 오게 (원본 iPhone 호환).
    virtual srs_error_t reap_segment();
};

// The HLS transmuxer: RTMP 스트림을 HLS로 변환하는 SrsOriginHub의 구성 요소.
class SrsHls
{
private:
    SrsHlsController* controller;
private:
    SrsRequest* req;
    // Whether the HLS is enabled (publish 중이고 hls_enabled).
    bool enabled;
    SrsOriginHub* hub;
public:
    SrsHls();
    virtual ~SrsHls();
public:
    virtual srs_error_t initialize(SrsOriginHub* h, SrsRequest* r);
    virtual srs_error_t on_publish();
    virtual void on_unpublish();
    // When we get an audio/video message with parsed format.
    virtual srs_error_t on_audio(SrsSharedPtrMessage* shared_audio, SrsFormat* format);
    virtual srs_error_t on_video(SrsSharedPtrMessage* shared_video, SrsFormat* format);
};

#endif
