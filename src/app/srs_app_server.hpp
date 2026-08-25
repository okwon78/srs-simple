// srs_simple — 원본: trunk/src/app/srs_app_server.hpp (1,628줄 → accept→conn 생성만)
// 원본 대비 제거: HTTP API, signal/pid/hourglass/ingest/reload — CLAUDE.md §5.6.
// 리스너는 RTMP + LL-HLS용 HTTP 2개만 유지 (S15 — CLAUDE.md §5.6 S11/S15).
#ifndef SRS_APP_SERVER_HPP
#define SRS_APP_SERVER_HPP

#include <srs_core.hpp>

#include <string>

#include <srs_app_conn.hpp>
#include <srs_app_listener.hpp>

// SRS RTMP server: initialize and listen, start the connection service thread, destroy clients.
class SrsServer : public ISrsResourceManager, public ISrsTcpHandler
{
private:
    // 종료된 연결을 다른 스레드에서 해제하는 reaper (CLAUDE.md §5.3).
    SrsResourceManager* conn_manager;
    // RTMP stream listener, over TCP.
    SrsTcpListener* rtmp_listener_;
    // HTTP listener for LL-HLS blocking serving (S15 — S11에서 제거했던 분기 복원.
    // TS-HLS 파일 서빙은 여전히 외부 nginx — CLAUDE.md §5.6 S11).
    SrsTcpListener* http_listener_;
public:
    SrsServer();
    virtual ~SrsServer();
// server startup workflow
public:
    // Start the conn manager coroutine.
    virtual srs_error_t initialize();
    // Listen on the RTMP/HTTP ports and start the accept coroutines.
    virtual srs_error_t listen();
// Interface ISrsTcpHandler
public:
    virtual srs_error_t on_tcp_client(ISrsListener* listener, srs_netfd_t stfd);
private:
    virtual srs_error_t do_on_tcp_client(ISrsListener* listener, srs_netfd_t& stfd);
// Interface ISrsResourceManager
public:
    // A callback for a connection to remove itself.
    // When the connection thread cycle terminates, call this back to delete the connection.
    virtual void remove(ISrsResource* c);
};

#endif
