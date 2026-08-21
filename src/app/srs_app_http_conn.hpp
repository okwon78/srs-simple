// srs_simple — 원본: trunk/src/app/srs_app_http_conn.hpp (SrsHttpConn)
//            + app/srs_app_http_static.cpp(SrsVodStream)
//            + protocol/srs_protocol_http_stack.cpp(SrsHttpFileServer::serve_file)의 극단적 축소판.
// HLS 파일(.m3u8/.ts)을 hls_path에서 서빙하는 GET 전용 HTTP/1.1 정적 서버.
// 원본의 HTTP 스택(SrsHttpServeMux/SrsHttpMessage/SrsHttpParser/http-parser 라이브러리)을
// 손 파싱으로 대체 — 요청 1개 처리 후 Connection: close (CLAUDE.md §5.6 S10).
// 연결 수명주기(스레드/reaper)는 SrsRtmpConn과 동일한 이디엄이다 (CLAUDE.md §5.3).
#ifndef SRS_APP_HTTP_CONN_HPP
#define SRS_APP_HTTP_CONN_HPP

#include <srs_core.hpp>

#include <string>

#include <srs_app_conn.hpp>
#include <srs_app_st.hpp>

class SrsServer;

// The http connection which serves the static HLS files.
class SrsHttpConn : public ISrsConnection, public ISrsStartable, public ISrsCoroutineHandler
{
private:
    srs_netfd_t stfd;
    SrsStSocket* skt;
    // Each connection start a green thread,
    // when thread stop, the connection will be delete by server.
    SrsCoroutine* trd;
    // The manager object to manage the connection.
    ISrsResourceManager* manager;
    // The ip and port of client.
    std::string ip;
    int port;
public:
    SrsHttpConn(SrsServer* svr, srs_netfd_t c, std::string cip, int cport);
    virtual ~SrsHttpConn();
// Interface ISrsResource.
public:
    virtual std::string desc();
protected:
    virtual srs_error_t do_cycle();
private:
    // Serve the file over HTTP: 헤더(Content-Type/Length) + 본문 스트리밍.
    virtual srs_error_t serve_file(std::string fullpath);
    // Write a simple response with status line and small body.
    virtual srs_error_t write_response(int code, std::string status, std::string body);
// Interface ISrsStartable
public:
    virtual srs_error_t start();
// Interface ISrsCoroutineHandler
public:
    virtual srs_error_t cycle();
// Interface ISrsConnection.
public:
    virtual std::string remote_ip();
    virtual const SrsContextId& get_id();
};

#endif
