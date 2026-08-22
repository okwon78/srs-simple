// srs_simple — 원본: trunk/src/app/srs_app_http_conn.hpp (SrsHttpConn)의 극단적 축소판.
// S10에서 HLS 정적 파일 서버로 만들었다가 S11에서 nginx로 대체하며 삭제한 것을,
// S15에서 LL-HLS 서빙용으로 부활시켰다 (CLAUDE.md §5.6 S11/S15, PLANS.md D4).
// 정적 파일은 더 이상 서빙하지 않는다 — 인메모리 SrsLlHlsStorage만 조회한다.
//
// S10과 달라진 점 (PLANS.md D4):
//   - keep-alive 루프: LL-HLS는 파트(0.5s)마다 요청이 온다 — Connection: close면
//     연결 폭주. 요청 파싱 → 응답을 idle 타임아웃까지 반복한다
//   - 블로킹 서빙: m3u8의 _HLS_msn/_HLS_part(블로킹 리로드)와 아직 없는 파트 GET
//     (프리로드 힌트)을 storage->wait_for로 홀드한다. 1-connection-1-thread(§5.1)라
//     OME의 비동기 pending 큐가 필요 없다 — HTTP 스레드가 cond_wait하면 그것이
//     곧 블로킹 리로드다
//
// 연결 수명주기(스레드/reaper)는 SrsRtmpConn과 동일한 이디엄이다 (CLAUDE.md §5.3).
#ifndef SRS_APP_HTTP_CONN_HPP
#define SRS_APP_HTTP_CONN_HPP

#include <srs_core.hpp>

#include <string>

#include <srs_app_conn.hpp>
#include <srs_app_st.hpp>

class SrsServer;
class SrsLlHlsStorage;

// The http connection which serves the in-memory LL-HLS resources.
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
private:
    // keep-alive 수신 버퍼 — 다음 요청의 선행 수신분(파이프라이닝)을 요청 사이에 보관.
    std::string inbuf;
    // Whether client requested Connection: close.
    bool conn_close_;
public:
    SrsHttpConn(SrsServer* svr, srs_netfd_t c, std::string cip, int cport);
    virtual ~SrsHttpConn();
// Interface ISrsResource.
public:
    virtual std::string desc();
protected:
    // keep-alive 루프: 요청 파싱 → 라우팅/응답 → 반복. idle 타임아웃이면 정상 종료.
    virtual srs_error_t do_cycle();
private:
    // 요청 헤더 전체("\r\n\r\n"까지)를 읽는다. idle(요청 사이) 타임아웃이면 eof=true.
    virtual srs_error_t read_request(std::string& header, bool& eof);
    // 라우팅: /{app}/{stream}.m3u8 | /{app}/{stream}/init.mp4 |
    //         /{app}/{stream}/{msn}.{psn}.m4s | /{app}/{stream}/{msn}.m4s
    virtual srs_error_t serve_llhls(std::string path, std::string query);
    // m3u8 + 블로킹 리로드 (_HLS_msn=N[&_HLS_part=P] — 미래면 홀드, 초과면 400).
    virtual srs_error_t serve_playlist(SrsLlHlsStorage* storage, std::string query);
    // 파트 — 아직 없으면(프리로드 힌트) 게시까지 홀드 후 200.
    virtual srs_error_t serve_part(SrsLlHlsStorage* storage, int64_t msn, int psn);
    // 게시까지 100ms 슬라이스로 대기 — 사이마다 pull()로 종료를 확인한다 (§5.1 이디엄).
    // 게시/unpublish/타임아웃 어느 쪽이든 호출자는 그 시점 상태로 응답한다.
    virtual srs_error_t hold(SrsLlHlsStorage* storage, int64_t msn, int psn, srs_utime_t timeout);
    // Write a response with body, Content-Length exact, keep-alive.
    virtual srs_error_t write_response(int code, std::string status, std::string content_type,
        std::string cache_control, const std::string& body);
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
