// srs_simple — 원본: trunk/src/app/srs_app_http_conn.cpp의 극단적 축소판 (헤더 주석 참조).
// S15: LL-HLS 전용 — keep-alive + 인메모리 storage 조회 + 블로킹 서빙 (PLANS.md D4).
// 블로킹 조건식은 OME LLHlsStream::GetChunklist를 따른다:
//   요청 msn > 최신 msn, 또는 (msn ≥ 최신 && 요청 psn > 최신 psn)이면 홀드.
//   _HLS_part 생략 시 psn=0으로 취급 (Safari는 _HLS_msn만 보내 새 세그먼트의
//   첫 파트를 기다린다 — OME 주석).
#include <srs_app_http_conn.hpp>

#include <ctype.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <vector>

#include <srs_app_config.hpp>
#include <srs_app_llhls.hpp>
#include <srs_app_server.hpp>
#include <srs_app_source.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

using namespace std;

// The http request/response timeout (원본: SRS_HTTP_RECV_TIMEOUT류의 축소판).
// recv 타임아웃이 곧 keep-alive idle 타임아웃이다 — 요청 사이에 이만큼 조용하면 닫는다.
#define SRS_HTTP_RECV_TIMEOUT (15 * SRS_UTIME_SECONDS)
#define SRS_HTTP_SEND_TIMEOUT (20 * SRS_UTIME_SECONDS)

// The max size of http request header we accept.
#define SRS_HTTP_MAX_REQUEST_HEADER 4096

// 블로킹 리로드/프리로드 힌트의 홀드 상한 (PLANS.md S15 — 예: 3×segment).
// 타임아웃이면 그 시점 상태로 응답한다 (m3u8는 최신으로 200, 파트는 404).
#define SRS_HTTP_HOLD_TIMEOUT (3 * _srs_config->llhls_segment)

// 스펙의 블로킹 리로드 관례: 최신 msn + 2를 넘는 미래 요청은 400 (PLANS.md §0).
#define SRS_HTTP_MSN_AHEAD_MAX 2

// 쿼리스트링에서 key의 값을 찾는다. 예: "_HLS_msn=3&_HLS_part=1"에서 "_HLS_msn" → "3".
static bool srs_http_query_get(const string& query, const string& key, string& value)
{
    size_t pos = 0;
    while (pos < query.length()) {
        size_t amp = query.find('&', pos);
        string kv = query.substr(pos, (amp == string::npos ? query.length() : amp) - pos);
        pos = (amp == string::npos) ? query.length() : amp + 1;

        size_t eq = kv.find('=');
        if (eq != string::npos && kv.substr(0, eq) == key) {
            value = kv.substr(eq + 1);
            return true;
        }
    }
    return false;
}

// 10진수 정수(msn/psn) 파싱. 숫자가 아니면 false — 호출자는 400/404로 응답한다.
static bool srs_http_parse_int(const string& v, int64_t& out)
{
    if (v.empty()) {
        return false;
    }
    for (size_t i = 0; i < v.length(); i++) {
        if (v[i] < '0' || v[i] > '9') {
            return false;
        }
    }
    out = (int64_t)::strtoll(v.c_str(), NULL, 10);
    return true;
}

SrsHttpConn::SrsHttpConn(SrsServer* svr, srs_netfd_t c, string cip, int cport)
{
    stfd = c;
    skt = new SrsStSocket(c);
    // 원본과 동일 — server가 ISrsResourceManager를 구현한다.
    manager = svr;
    ip = cip;
    port = cport;
    conn_close_ = false;

    // TCP_NODELAY — LL-HLS는 0.5초마다 작은 응답(m3u8, 수 KB)을 보낸다. Nagle이
    // 직전 세그먼트의 ACK(클라이언트 delayed-ACK 최대 수십 ms)까지 꼬리를 붙들면
    // 그 지연이 파트 주기마다 재생 지연에 더해진다 (S18. RTMP 쪽은 원본 기본값대로 off).
    if (stfd != SRS_NETFD_INVALID) {
        int v = 1;
        ::setsockopt(stfd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
    }

    trd = new SrsSTCoroutine("http", this);
}

SrsHttpConn::~SrsHttpConn()
{
    trd->interrupt();
    // 블록된 recv/send를 깨운다 — SrsRtmpConn과 동일한 이디엄 (CLAUDE.md §5.1).
    // 홀드 중(storage cond_wait)인 스레드는 100ms 슬라이스의 pull()에서 인터럽트를
    // 발견한다 (hold 참조) — 소켓 shutdown이 cond를 깨울 수는 없기 때문.
    if (stfd != SRS_NETFD_INVALID) {
        ::shutdown(stfd, SHUT_RDWR);
    }
    srs_freep(trd);

    srs_freep(skt);
    srs_close_stfd(stfd);
}

string SrsHttpConn::desc()
{
    return "HttpConn";
}

srs_error_t SrsHttpConn::do_cycle()
{
    srs_error_t err = srs_success;

    skt->set_recv_timeout(SRS_HTTP_RECV_TIMEOUT);
    skt->set_send_timeout(SRS_HTTP_SEND_TIMEOUT);

    // keep-alive 루프 — LL-HLS는 파트(0.5s)마다 요청이 오므로 연결을 재사용한다 (D4).
    while (true) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "http conn");
        }

        string header;
        bool eof = false;
        if ((err = read_request(header, eof)) != srs_success) {
            return srs_error_wrap(err, "read request");
        }
        if (eof) {
            // idle 타임아웃 또는 요청 사이의 FIN — 정상 종료.
            return srs_success;
        }

        // request line 파싱: "GET /live/livestream.m3u8?_HLS_msn=3 HTTP/1.1"
        size_t eol = header.find("\r\n");
        string request_line = header.substr(0, eol);

        size_t sp1 = request_line.find(' ');
        size_t sp2 = (sp1 == string::npos) ? string::npos : request_line.find(' ', sp1 + 1);
        if (sp1 == string::npos || sp2 == string::npos) {
            return srs_error_new(ERROR_HTTP_PARSE_HEADER, "invalid request line: %s", request_line.c_str());
        }

        string method = request_line.substr(0, sp1);
        string path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

        // Connection: close 요청이면 이번 응답 후 닫는다 (HTTP/1.1 기본은 keep-alive).
        string lower = header;
        for (size_t i = 0; i < lower.length(); i++) {
            lower[i] = (char)tolower(lower[i]);
        }
        conn_close_ = lower.find("connection: close") != string::npos;

        // split query string.
        string query;
        size_t qs = path.find('?');
        if (qs != string::npos) {
            query = path.substr(qs + 1);
            path = path.substr(0, qs);
        }

        if (method != "GET") {
            srs_trace("HTTP %s %s 405", method.c_str(), path.c_str());
            if ((err = write_response(405, "Method Not Allowed", "text/plain; charset=utf-8", "no-cache", "405 method not allowed\n")) != srs_success) {
                return srs_error_wrap(err, "write 405");
            }
        } else if (path.empty() || path[0] != '/' || path.find("..") != string::npos) {
            srs_trace("HTTP GET %s 404 (invalid path)", path.c_str());
            if ((err = write_response(404, "Not Found", "text/plain; charset=utf-8", "no-cache", "404 not found\n")) != srs_success) {
                return srs_error_wrap(err, "write 404");
            }
        } else {
            if ((err = serve_llhls(path, query)) != srs_success) {
                return srs_error_wrap(err, "serve %s", path.c_str());
            }
        }

        if (conn_close_) {
            return srs_success;
        }
    }

    return err;
}

srs_error_t SrsHttpConn::read_request(string& header, bool& eof)
{
    srs_error_t err = srs_success;

    // 헤더 끝("\r\n\r\n")까지 읽는다. 본문은 GET 전용이므로 없다.
    // inbuf에는 이전 read가 더 읽어 둔 다음 요청의 선행분이 남아 있을 수 있다.
    while (true) {
        size_t pos = inbuf.find("\r\n\r\n");
        if (pos != string::npos) {
            header = inbuf.substr(0, pos + 4);
            inbuf.erase(0, pos + 4);
            return err;
        }

        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "http conn");
        }

        if (inbuf.length() >= SRS_HTTP_MAX_REQUEST_HEADER) {
            return srs_error_new(ERROR_HTTP_PARSE_HEADER, "request header too large");
        }

        char buf[1024];
        ssize_t nread = 0;
        err = skt->read(buf, sizeof(buf), &nread);
        if (err != srs_success) {
            // 요청 사이의 타임아웃/FIN은 keep-alive의 정상 종료다. 요청 도중이면 에러.
            if (inbuf.empty() && (srs_error_code(err) == ERROR_SOCKET_TIMEOUT || srs_is_client_gracefully_close(err))) {
                srs_freep(err);
                eof = true;
                return srs_success;
            }
            return srs_error_wrap(err, "read request");
        }

        inbuf.append(buf, (size_t)nread);
    }
}

srs_error_t SrsHttpConn::serve_llhls(string path, string query)
{
    srs_error_t err = srs_success;

    // path를 '/' 세그먼트로 나눈다: "/live/livestream.m3u8" → {"live", "livestream.m3u8"}.
    vector<string> segs;
    size_t pos = 1;
    while (pos <= path.length()) {
        size_t slash = path.find('/', pos);
        if (slash == string::npos) {
            slash = path.length();
        }
        if (slash > pos) {
            segs.push_back(path.substr(pos, slash - pos));
        }
        pos = slash + 1;
    }

    // 라우팅 — m3u8과 미디어 URI는 S14의 URI 헬퍼와 같은 규칙이다 (srs_app_llhls.hpp):
    //   /{app}/{stream}.m3u8 | /{app}/{stream}/init.mp4 |
    //   /{app}/{stream}/{msn}.{psn}.m4s | /{app}/{stream}/{msn}.m4s
    string app, stream, file;
    if (segs.size() == 2 && segs[1].length() > 5 && segs[1].rfind(".m3u8") == segs[1].length() - 5) {
        app = segs[0];
        stream = segs[1].substr(0, segs[1].length() - 5);
    } else if (segs.size() == 3) {
        app = segs[0];
        stream = segs[1];
        file = segs[2];
    }

    if (app.empty() || stream.empty()) {
        srs_trace("HTTP GET %s 404", path.c_str());
        return write_response(404, "Not Found", "text/plain; charset=utf-8", "no-cache", "404 not found\n");
    }

    // 스트림의 storage를 찾는다. source는 프로세스 종료까지 살고(§5.6 S8) 포인터가
    // 불변이라 HTTP 스레드는 manager lock과 storage lock만 잡는다 — source lock 금지.
    // HTTP 경로에는 vhost가 없으므로 app/stream만으로 조회한다 (fetch 주석 참조).
    SrsLiveSource* source = _srs_sources->fetch(app, stream);
    if (!source) {
        srs_trace("HTTP GET %s 404 (no source)", path.c_str());
        return write_response(404, "Not Found", "text/plain; charset=utf-8", "no-cache", "404 not found\n");
    }
    SrsLlHlsStorage* storage = source->llhls_storage();

    // m3u8 — 블로킹 리로드 포함.
    if (file.empty()) {
        return serve_playlist(storage, query);
    }

    // init.mp4
    if (file == "init.mp4") {
        string v;
        if (!storage->get_init(v)) {
            srs_trace("HTTP GET %s 404 (no init)", path.c_str());
            return write_response(404, "Not Found", "text/plain; charset=utf-8", "no-cache", "404 not found\n");
        }
        srs_trace("HTTP GET %s 200 %dB", path.c_str(), (int)v.length());
        return write_response(200, "OK", "video/mp4", "max-age=3600", v);
    }

    // {msn}.{psn}.m4s(파트) / {msn}.m4s(완결 세그먼트 = 파트 연결).
    if (file.length() > 4 && file.rfind(".m4s") == file.length() - 4) {
        string base = file.substr(0, file.length() - 4);
        size_t dot = base.find('.');

        int64_t msn = -1, psn = -1;
        if (dot == string::npos) {
            if (srs_http_parse_int(base, msn)) {
                if ((err = serve_segment(storage, msn)) != srs_success) {
                    return srs_error_wrap(err, "segment msn=%d", (int)msn);
                }
                return err;
            }
        } else {
            if (srs_http_parse_int(base.substr(0, dot), msn) && srs_http_parse_int(base.substr(dot + 1), psn)) {
                if ((err = serve_part(storage, msn, (int)psn)) != srs_success) {
                    return srs_error_wrap(err, "part msn=%d psn=%d", (int)msn, (int)psn);
                }
                return err;
            }
        }
    }

    srs_trace("HTTP GET %s 404", path.c_str());
    return write_response(404, "Not Found", "text/plain; charset=utf-8", "no-cache", "404 not found\n");
}

srs_error_t SrsHttpConn::serve_playlist(SrsLlHlsStorage* storage, string query)
{
    srs_error_t err = srs_success;

    // 블로킹 리로드: _HLS_msn=N[&_HLS_part=P]. 스펙 — 이미 있으면 즉시, 곧 나올
    // 것이면 홀드, 너무 먼 미래면 400. _HLS_part만 있는 요청도 400 (스펙).
    string v_msn, v_psn;
    bool has_msn = srs_http_query_get(query, "_HLS_msn", v_msn);
    bool has_psn = srs_http_query_get(query, "_HLS_part", v_psn);

    if (has_msn) {
        int64_t msn = -1, psn = -1;
        bool ok = srs_http_parse_int(v_msn, msn);
        if (ok && has_psn) {
            ok = srs_http_parse_int(v_psn, psn);
        }
        if (!ok) {
            srs_trace("HTTP GET m3u8 400 (bad directives: %s)", query.c_str());
            return write_response(400, "Bad Request", "text/plain; charset=utf-8", "no-cache", "400 bad request\n");
        }

        // 최신 + 2를 넘는 미래는 홀드하지 않는다 (스펙 관례 — PLANS.md §0).
        int64_t latest = storage->latest_msn();
        if (msn > latest + SRS_HTTP_MSN_AHEAD_MAX) {
            srs_trace("HTTP GET m3u8 400 (msn=%d too far, latest=%d)", (int)msn, (int)latest);
            return write_response(400, "Bad Request", "text/plain; charset=utf-8", "no-cache", "400 bad request\n");
        }

        // 게시까지 홀드 — 타임아웃이면 그 시점 최신으로 200 (PLANS.md S15).
        if ((err = hold(storage, msn, (int)psn, SRS_HTTP_HOLD_TIMEOUT)) != srs_success) {
            return srs_error_wrap(err, "hold m3u8 msn=%d", (int)msn);
        }
    } else if (has_psn) {
        srs_trace("HTTP GET m3u8 400 (_HLS_part without _HLS_msn)");
        return write_response(400, "Bad Request", "text/plain; charset=utf-8", "no-cache", "400 bad request\n");
    }

    string v;
    if (!storage->get_playlist(v)) {
        srs_trace("HTTP GET m3u8 404 (no playlist)");
        return write_response(404, "Not Found", "text/plain; charset=utf-8", "no-cache", "404 not found\n");
    }

    srs_trace("HTTP GET m3u8 200 %dB%s%s", (int)v.length(), has_msn ? " " : "", has_msn ? query.c_str() : "");
    return write_response(200, "OK", "application/vnd.apple.mpegurl", "no-cache", v);
}

srs_error_t SrsHttpConn::serve_part(SrsLlHlsStorage* storage, int64_t msn, int psn)
{
    srs_error_t err = srs_success;

    // 파트는 shared_ptr로 받아 락 밖에서 소켓에 쓴다 — 페이로드 복사 없음 (S18).
    // 이 참조가 살아 있는 동안은 윈도우에서 밀려나도 바이트가 해제되지 않는다.
    SrsLlHlsPartPtr part;
    if (storage->get_part(msn, psn, part)) {
        srs_trace("HTTP GET part %d.%d 200 %dB", (int)msn, psn, (int)part->payload.length());
        vector<iovec> body(1);
        body[0].iov_base = (void*)part->payload.data();
        body[0].iov_len = part->payload.length();
        return write_response(200, "OK", "video/mp4", "max-age=3600", body);
    }

    // 아직 없는 파트 — 프리로드 힌트의 GET은 리소스가 생길 때까지 홀드 후 200 (스펙).
    // 그럴듯한 미래(최신+2 이내)만 홀드한다. 만료된 과거는 hold가 즉시 돌아와 404.
    if (msn <= storage->latest_msn() + SRS_HTTP_MSN_AHEAD_MAX) {
        if ((err = hold(storage, msn, psn, SRS_HTTP_HOLD_TIMEOUT)) != srs_success) {
            return srs_error_wrap(err, "hold part");
        }
        if (storage->get_part(msn, psn, part)) {
            srs_trace("HTTP GET part %d.%d 200 %dB (held)", (int)msn, psn, (int)part->payload.length());
            vector<iovec> body(1);
            body[0].iov_base = (void*)part->payload.data();
            body[0].iov_len = part->payload.length();
            return write_response(200, "OK", "video/mp4", "max-age=3600", body);
        }
    }

    srs_trace("HTTP GET part %d.%d 404", (int)msn, psn);
    return write_response(404, "Not Found", "text/plain; charset=utf-8", "no-cache", "404 not found\n");
}

srs_error_t SrsHttpConn::hold(SrsLlHlsStorage* storage, int64_t msn, int psn, srs_utime_t timeout)
{
    srs_error_t err = srs_success;

    // 100ms 슬라이스 cond_wait — 사이마다 pull()로 인터럽트(연결/서버 종료)를 확인한다.
    // 리스너 poll/consumer wait와 같은 이디엄 (CLAUDE.md §5.1). 게시는 notify_all로
    // 즉시 깨어나고, unpublish면 더 올 것이 없으므로 그 시점 상태로 응답하러 돌아간다.
    const srs_utime_t slice = 100 * SRS_UTIME_MILLISECONDS;
    for (srs_utime_t elapsed = 0; elapsed < timeout; elapsed += slice) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "http hold");
        }
        if (storage->wait_for(msn, psn, slice)) {
            return err;
        }
        if (!storage->active()) {
            return err;
        }
    }

    return err;
}

srs_error_t SrsHttpConn::serve_segment(SrsLlHlsStorage* storage, int64_t msn)
{
    // 완결 세그먼트 = 파트 페이로드의 연결. 파트 포인터 목록만 락 안에서 받고,
    // 바이트는 writev가 파트별 iov로 이어 보낸다 — 세그먼트 크기(~수백 KB)의 임시
    // 문자열을 만들지 않는다 (S18).
    vector<SrsLlHlsPartPtr> parts;
    if (!storage->get_segment(msn, parts)) {
        srs_trace("HTTP GET segment %d 404 (no segment)", (int)msn);
        return write_response(404, "Not Found", "text/plain; charset=utf-8", "no-cache", "404 not found\n");
    }

    vector<iovec> body(parts.size());
    size_t total = 0;
    for (size_t i = 0; i < parts.size(); i++) {
        body[i].iov_base = (void*)parts[i]->payload.data();
        body[i].iov_len = parts[i]->payload.length();
        total += body[i].iov_len;
    }

    srs_trace("HTTP GET segment %d 200 %dB (%d parts)", (int)msn, (int)total, (int)parts.size());
    return write_response(200, "OK", "video/mp4", "max-age=3600", body);
}

srs_error_t SrsHttpConn::write_response(int code, const string& status, const string& content_type, const string& cache_control, const string& body)
{
    vector<iovec> iovs;
    if (!body.empty()) {
        iovec iov;
        iov.iov_base = (void*)body.data();
        iov.iov_len = body.length();
        iovs.push_back(iov);
    }
    return write_response(code, status, content_type, cache_control, iovs);
}

srs_error_t SrsHttpConn::write_response(int code, const string& status, const string& content_type, const string& cache_control, const vector<iovec>& body)
{
    srs_error_t err = srs_success;

    size_t content_length = 0;
    for (size_t i = 0; i < body.size(); i++) {
        content_length += body[i].iov_len;
    }

    // CORS 허용 — hls.js 같은 브라우저 플레이어용. Content-Length는 keep-alive의
    // 전제다 — 정확해야 클라이언트가 응답 경계를 안다 (PLANS.md D4).
    char header[512];
    int nb_header = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: %s\r\n"
        "\r\n",
        code, status.c_str(), RTMP_SIG_SRS_SERVER, content_type.c_str(), (int)content_length,
        cache_control.c_str(), conn_close_ ? "close" : "keep-alive");
    if (nb_header <= 0 || nb_header >= (int)sizeof(header)) {
        return srs_error_new(ERROR_HTTP_PARSE_HEADER, "response header too large");
    }

    // 헤더와 본문을 한 writev로 — 한 번의 커널 진입, 그리고 헤더만 먼저 나가
    // Nagle이 본문 꼬리를 붙드는 일이 없다 (S18. 생성자의 TCP_NODELAY와 짝).
    vector<iovec> iovs;
    iovs.reserve(body.size() + 1);
    iovec h;
    h.iov_base = header;
    h.iov_len = (size_t)nb_header;
    iovs.push_back(h);
    iovs.insert(iovs.end(), body.begin(), body.end());

    if ((err = skt->writev(&iovs[0], (int)iovs.size(), NULL)) != srs_success) {
        return srs_error_wrap(err, "write response");
    }

    return err;
}

srs_error_t SrsHttpConn::start()
{
    srs_error_t err = srs_success;

    if ((err = trd->start()) != srs_success) {
        return srs_error_wrap(err, "coroutine");
    }

    return err;
}

srs_error_t SrsHttpConn::cycle()
{
    srs_error_t err = do_cycle();

    // Notify the manager to remove it.
    // 자기 스레드에서 delete this 금지 — 매니저가 다른 스레드에서 해제한다 (CLAUDE.md §5.3).
    manager->remove(this);

    // success.
    if (err == srs_success) {
        return err;
    }

    // the client closed the connection.
    if (srs_is_client_gracefully_close(err)) {
        srs_warn("http client disconnect peer. ret=%d", srs_error_code(err));
    } else {
        srs_error("http serve error %s", srs_error_desc(err).c_str());
    }

    srs_freep(err);
    return srs_success;
}

string SrsHttpConn::remote_ip()
{
    return ip;
}

const SrsContextId& SrsHttpConn::get_id()
{
    return trd->cid();
}
