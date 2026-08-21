// srs_simple — 원본: trunk/src/app/srs_app_http_conn.cpp + protocol/srs_protocol_http_stack.cpp
// (serve_file:420, MIME 맵:436 — .m3u8/.ts 타입은 원본과 동일)
#include <srs_app_http_conn.hpp>

#include <string.h>
#include <sys/socket.h>

#include <sstream>

#include <srs_app_config.hpp>
#include <srs_app_server.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_file.hpp>
#include <srs_kernel_log.hpp>

using namespace std;

// The http request/response timeout (원본: SRS_HTTP_RECV_TIMEOUT류의 축소판).
#define SRS_HTTP_RECV_TIMEOUT (15 * SRS_UTIME_SECONDS)
#define SRS_HTTP_SEND_TIMEOUT (20 * SRS_UTIME_SECONDS)

// The max size of http request header we accept.
#define SRS_HTTP_MAX_REQUEST_HEADER 4096

// The mime types (원본: srs_protocol_http_stack.cpp:436의 _mime 맵 서브셋).
static string srs_go_http_content_type(const string& path)
{
    size_t pos = path.rfind('.');
    string ext = (pos != string::npos) ? path.substr(pos) : "";

    if (ext == ".m3u8") {
        return "application/vnd.apple.mpegurl";
    }
    if (ext == ".ts") {
        return "video/MP2T";
    }
    if (ext == ".html") {
        return "text/html; charset=utf-8";
    }
    return "application/octet-stream";
}

SrsHttpConn::SrsHttpConn(SrsServer* svr, srs_netfd_t c, string cip, int cport)
{
    stfd = c;
    skt = new SrsStSocket(c);
    // 원본과 동일 — server가 ISrsResourceManager를 구현한다.
    manager = svr;
    ip = cip;
    port = cport;

    trd = new SrsSTCoroutine("http", this);
}

SrsHttpConn::~SrsHttpConn()
{
    trd->interrupt();
    // 블록된 recv/send를 깨운다 — SrsRtmpConn과 동일한 이디엄 (CLAUDE.md §5.1).
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

    // 헤더 끝("\r\n\r\n")까지 읽는다. 본문은 GET 전용이므로 없다.
    char buf[SRS_HTTP_MAX_REQUEST_HEADER + 1];
    int nb_read = 0;
    while (true) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "http conn");
        }

        if (nb_read >= SRS_HTTP_MAX_REQUEST_HEADER) {
            return srs_error_new(ERROR_HTTP_PARSE_HEADER, "request header too large");
        }

        ssize_t nread = 0;
        if ((err = skt->read(buf + nb_read, SRS_HTTP_MAX_REQUEST_HEADER - nb_read, &nread)) != srs_success) {
            return srs_error_wrap(err, "read request");
        }
        nb_read += (int)nread;
        buf[nb_read] = '\0';

        if (strstr(buf, "\r\n\r\n") != NULL) {
            break;
        }
    }

    // request line 파싱: "GET /live/livestream.m3u8 HTTP/1.1"
    string request(buf);
    size_t eol = request.find("\r\n");
    string request_line = request.substr(0, eol);

    size_t sp1 = request_line.find(' ');
    size_t sp2 = (sp1 == string::npos) ? string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == string::npos || sp2 == string::npos) {
        return srs_error_new(ERROR_HTTP_PARSE_HEADER, "invalid request line: %s", request_line.c_str());
    }

    string method = request_line.substr(0, sp1);
    string path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    if (method != "GET") {
        srs_trace("HTTP %s %s 405", method.c_str(), path.c_str());
        return write_response(405, "Method Not Allowed", "405 method not allowed\n");
    }

    // strip query string.
    size_t qs = path.find('?');
    if (qs != string::npos) {
        path = path.substr(0, qs);
    }

    // 경로 탈출 방지 — 서빙 루트 밖의 파일을 읽지 못하게.
    if (path.empty() || path[0] != '/' || path.find("..") != string::npos) {
        srs_trace("HTTP GET %s 404 (invalid path)", path.c_str());
        return write_response(404, "Not Found", "404 not found\n");
    }

    // "/"는 HLS 플레이어 페이지로 (www/index.html).
    if (path == "/") {
        path = "/index.html";
    }

    // hls_path(.m3u8/.ts)에서 먼저 찾고, 없으면 http_dir(정적 페이지)에서 찾는다.
    string fullpath = _srs_config->hls_path + path;
    if (!srs_path_exists(fullpath)) {
        fullpath = _srs_config->http_dir + path;
    }
    if (!srs_path_exists(fullpath)) {
        srs_trace("HTTP GET %s 404", path.c_str());
        return write_response(404, "Not Found", "404 not found\n");
    }

    if ((err = serve_file(fullpath)) != srs_success) {
        return srs_error_wrap(err, "serve %s", fullpath.c_str());
    }

    srs_trace("HTTP GET %s 200", path.c_str());

    return err;
}

srs_error_t SrsHttpConn::serve_file(string fullpath)
{
    srs_error_t err = srs_success;

    SrsFileReader fs;
    if ((err = fs.open(fullpath)) != srs_success) {
        return srs_error_wrap(err, "open file");
    }

    int64_t length = fs.filesize();

    // response header. CORS 허용 — hls.js 같은 브라우저 플레이어용.
    std::stringstream ss;
    ss << "HTTP/1.1 200 OK\r\n"
        << "Server: " << RTMP_SIG_SRS_SERVER << "\r\n"
        << "Content-Type: " << srs_go_http_content_type(fullpath) << "\r\n"
        << "Content-Length: " << length << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    string header = ss.str();
    if ((err = skt->write((void*)header.data(), header.length(), NULL)) != srs_success) {
        return srs_error_wrap(err, "write header");
    }

    // response body: 64KB씩 스트리밍.
    char buf[65536];
    int64_t left = length;
    while (left > 0) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "http conn");
        }

        ssize_t nread = 0;
        if ((err = fs.read(buf, sizeof(buf), &nread)) != srs_success) {
            return srs_error_wrap(err, "read file");
        }
        if (nread <= 0) {
            break;
        }

        if ((err = skt->write(buf, nread, NULL)) != srs_success) {
            return srs_error_wrap(err, "write body");
        }
        left -= (int64_t)nread;
    }

    return err;
}

srs_error_t SrsHttpConn::write_response(int code, string status, string body)
{
    srs_error_t err = srs_success;

    std::stringstream ss;
    ss << "HTTP/1.1 " << code << " " << status << "\r\n"
        << "Server: " << RTMP_SIG_SRS_SERVER << "\r\n"
        << "Content-Type: text/plain; charset=utf-8\r\n"
        << "Content-Length: " << body.length() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    string response = ss.str();

    if ((err = skt->write((void*)response.data(), response.length(), NULL)) != srs_success) {
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

    // Notify manager to remove it.
    // 자기 스레드에서 delete this 금지 — 매니저가 다른 스레드에서 해제한다 (CLAUDE.md §5.3).
    manager->remove(this);

    // success.
    if (err == srs_success) {
        return err;
    }

    // client close peer.
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
