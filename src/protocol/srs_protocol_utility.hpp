// srs_simple — 원본: trunk/src/protocol/srs_protocol_utility.hpp
// tcUrl 파싱 유틸. 원본은 SrsHttpUri(HTTP 스택)로 파싱하지만 HTTP 계층이 없으므로 손 파싱 — CLAUDE.md §5.6.
// 문자열 헬퍼(srs_string_*)는 원본 kernel/srs_kernel_utility.hpp 소속이나,
// utility 파일을 따로 만들지 않으므로 여기로 병합.
#ifndef SRS_PROTOCOL_UTILITY_HPP
#define SRS_PROTOCOL_UTILITY_HPP

#include <srs_core.hpp>

#include <string>

// Parse the tcUrl, output the schema, host, vhost, app and port.
// @param tcUrl, the input tcUrl, for example,
//       rtmp://192.168.1.10:19350/live?vhost=vhost.ossrs.net
// @param schema, for example, rtmp
// @param host, for example, 192.168.1.10
// @param vhost, for example, vhost.ossrs.net.
//       vhost default to host, when user not set vhost in query of app.
// @param app, for example, live
// @param stream, for example, livestream. 입력값이 있으면 URL에 합쳐서 재파싱한다
//       (stream에 붙은 ?k=v 파라미터를 param으로 분리하는 용도).
// @param port, for example, 19350, default to 1935.
// @param param, for example, ?vhost=vhost.ossrs.net
extern void srs_discovery_tc_url(std::string tcUrl, std::string& schema, std::string& host, std::string& vhost,
    std::string& app, std::string& stream, int& port, std::string& param);

// Generate the stream url from vhost/app/stream. ("vhost/app/stream", 기본 vhost면 "/app/stream")
// SrsLiveSourceManager(S8)의 소스 맵 키가 된다.
extern std::string srs_generate_stream_url(std::string vhost, std::string app, std::string stream);

// Build the path from template, replace [vhost],[app],[stream].
// S10 HLS의 m3u8/ts 파일명 템플릿 치환. (원본: kernel/srs_kernel_utility.cpp)
extern std::string srs_path_build_stream(std::string template_path, std::string vhost, std::string app, std::string stream);

// Socket address helpers (원본도 이 파일 소속). IPv4 전용으로 단순화.
// Get the peer ip/port of the connected fd. ip는 실패 시 빈 문자열.
extern std::string srs_get_peer_ip(int fd);
extern int srs_get_peer_port(int fd);
// Get the local ip of the connected fd (connect 응답의 srs_server_ip 필드용).
extern std::string srs_get_local_ip(int fd);

// String helpers (원본: kernel/srs_kernel_utility.hpp)
// replace old_str to new_str of str
extern std::string srs_string_replace(std::string str, std::string old_str, std::string new_str);
// trim char in trim_chars of str
extern std::string srs_string_trim_end(std::string str, std::string trim_chars);
// trim char in trim_chars of str
extern std::string srs_string_trim_start(std::string str, std::string trim_chars);
// remove char in remove_chars of str
extern std::string srs_string_remove(std::string str, std::string remove_chars);

#endif
