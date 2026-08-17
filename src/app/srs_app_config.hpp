// srs_simple — 원본: trunk/src/app/srs_app_config.{hpp,cpp} (10,138줄)
// 설정 파일 파서/reload 시스템 전체를 상수 구조체 하나로 대체한다 (CLAUDE.md §5.4).
// 원본의 `_srs_config->get_xxx(vhost)` 호출은 여기서는 `_srs_config->xxx` 멤버 접근으로 읽는다.
#ifndef SRS_APP_CONFIG_HPP
#define SRS_APP_CONFIG_HPP

#include <srs_core.hpp>

// 라이브 경로(SrsRtmpConn/SrsLiveSource)가 실제로 읽는 값만 유지.
// 원본 conf/full.conf의 기본값과 동일하다.
struct SrsSimpleConfig
{
    // The port to listen (원본: listen)
    int listen_port = 1935;
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
};

// @global The config object (원본의 _srs_config 전역과 동일한 이름).
extern SrsSimpleConfig* _srs_config;

#endif
