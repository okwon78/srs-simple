// srs_simple — 원본: trunk/src/app/srs_app_config.{hpp,cpp} (10,138줄)
// 설정 파일 파서/reload 시스템 전체를 상수 구조체 하나로 대체한다 (CLAUDE.md §5.4).
// 원본의 `_srs_config->get_xxx(vhost)` 호출은 여기서는 `_srs_config->xxx` 멤버 접근으로 읽는다.
#ifndef SRS_APP_CONFIG_HPP
#define SRS_APP_CONFIG_HPP

#include <srs_core.hpp>

#include <string>

// 라이브 경로(SrsRtmpConn/SrsLiveSource)가 실제로 읽는 값만 유지.
// 원본 conf/full.conf의 기본값과 동일하다.
struct SrsSimpleConfig
{
    // The port to listen (원본: listen)
    int rtmp_listen_port = 1935;
    // The outbound chunk size (원본: chunk_size). connect의 _result보다 먼저 송신 — 이슈 #454.
    int chunk_size = 60000;
    // The outbound WindowAckSize (원본: out_ack_size)
    int window_ack_size = 2500000;
    // The SetPeerBandwidth value, dynamic type (원본은 2.5M 하드코딩)
    int peer_bandwidth = 2500000;
    // Whether enable the gop cache (S8, 원본: gop_cache)
    bool gop_cache = true;
    // The max frames in gop cache (S8, 원본: gop_cache_max_frames)
    int gop_cache_max_frames = 2500;
    // The max queue length of consumer (S8, 원본: queue_length)
    srs_utime_t queue_length = 30 * SRS_UTIME_SECONDS;
    // The max messages to send by one writev (S8, 원본: mw_msgs)
    int mw_msgs = 128;

    // ---- S10 HLS (원본: vhost.hls.*) ----
    // Whether enable HLS. 원본 기본값은 off — 데모 편의로 on (CLAUDE.md §5.6 S10).
    bool hls_enabled = true;
    // The duration of one ts segment (원본: hls_fragment, 기본 10s)
    srs_utime_t hls_fragment = 10 * SRS_UTIME_SECONDS;
    // The total duration of m3u8 rolling window (원본: hls_window, 기본 60s)
    srs_utime_t hls_window = 60 * SRS_UTIME_SECONDS;
    // pure audio에서 hls_aof_ratio*hls_fragment를 넘으면 강제로 세그먼트를 자른다
    // (키프레임이 없어 잘릴 기회가 없으므로 — 원본: hls_aof_ratio)
    double hls_aof_ratio = 2.1;
    // Whether cut segment at keyframe only (원본: hls_wait_keyframe)
    bool hls_wait_keyframe = true;
    // Whether delete the expired ts file (원본: hls_cleanup)
    bool hls_cleanup = true;
    // The dir to store the ts/m3u8 files. 원본 기본값 ./objs/nginx/html — 단순화 (§5.6).
    std::string hls_path = "./objs/hls";
    // The m3u8/ts file name template ([vhost],[app],[stream],[seq] 치환)
    std::string hls_m3u8_file = "[app]/[stream].m3u8";
    std::string hls_ts_file = "[app]/[stream]-[seq].ts";

    // NOTE: hls_path의 m3u8/ts와 플레이어 페이지(www/hls.html)의 HTTP 서빙은
    // 외부 nginx가 담당한다 — conf/nginx.conf 참조 (CLAUDE.md §5.6 S11).

    // ---- S13 LL-HLS (원본 SRS에 대응물 없음 — PLANS.md D5, OME/llhls-streaming 참조) ----
    // Whether enable LL-HLS (fMP4 파트 + 인메모리 + 블로킹 서빙).
    bool llhls_enabled = true;
    // The duration of one segment (EXTINF). 전제: 인코더 GOP ≤ 이 값 — README 참조.
    srs_utime_t llhls_segment = 2 * SRS_UTIME_SECONDS;
    // The target duration of one part (EXT-X-PARTINF의 PART-TARGET).
    srs_utime_t llhls_part = 500 * SRS_UTIME_MILLISECONDS;
    // The rolling window, in segments (= 20초). 파트는 부모 세그먼트와 함께 만료된다.
    int llhls_segment_count = 10;
    // The port of builtin HTTP server for LL-HLS (S15 — nginx 8080과 분리).
    int llhls_http_port = 8081;
};

// @global The config object (원본의 _srs_config 전역과 동일한 이름).
extern SrsSimpleConfig *_srs_config;

#endif
