// srs_simple — 원본: trunk/src/app/srs_app_server.hpp (1,628줄 → accept→conn 생성만)
// 원본 대비 제거: HTTP API/서버, signal/pid/hourglass/ingest/reload, 다중 리스너 — CLAUDE.md §5.6.
#ifndef SRS_APP_SERVER_HPP
#define SRS_APP_SERVER_HPP

#include <srs_core.hpp>

#include <string>

#include <srs_app_conn.hpp>
#include <srs_app_listener.hpp>

// SRS RTMP server, initialize and listen, start connection service thread, destroy client.
class SrsServer : public ISrsResourceManager, public ISrsTcpHandler
{
private:
    // 종료된 연결을 다른 스레드에서 해제하는 reaper (CLAUDE.md §5.3).
    SrsResourceManager* conn_manager;
    // RTMP stream listener, over TCP.
    SrsTcpListener* rtmp_listener_;
    // HTTP stream listener (S10: HLS 파일 서빙), over TCP.
    SrsTcpListener* http_listener_;
public:
    SrsServer();
    virtual ~SrsServer();
// server startup workflow
public:
    // Start the conn manager coroutine.
    virtual srs_error_t initialize();
    // Listen the RTMP port and start the accept coroutine.
    virtual srs_error_t listen();
// Interface ISrsTcpHandler
public:
    virtual srs_error_t on_tcp_client(ISrsListener* listener, srs_netfd_t stfd);
private:
    virtual srs_error_t do_on_tcp_client(ISrsListener* listener, srs_netfd_t& stfd);
// Interface ISrsResourceManager
public:
    // A callback for connection to remove itself.
    // When connection thread cycle terminated, callback this to delete connection.
    virtual void remove(ISrsResource* c);
};

#endif
