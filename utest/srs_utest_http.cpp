// srs_simple — S15: LL-HLS HTTP 서빙 테스트. 실소켓으로 SrsServer의 HTTP 리스너에
// 접속하는 간이 HTTP 클라이언트(MockRtmpClient 방식 — srs_utest_server.cpp)로
// keep-alive / 블로킹 리로드 / 400·404 경계 / 프리로드 힌트 홀드 / unpublish 기상을
// 검증한다 (TASKS.md S15).
#include "srs_utest.hpp"

#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <srs_app_config.hpp>
#include <srs_app_llhls.hpp>
#include <srs_app_server.hpp>
#include <srs_app_source.hpp>
#include <srs_app_st.hpp>
#include <srs_protocol_rtmp_stack.hpp>

// cond가 참이 될 때까지 최대 timeout_ms 만큼 1ms 단위로 폴링한다 (srs_utest_server.cpp와 동일).
#define HTTPHELPER_WAIT_UNTIL(cond, timeout_ms)          \
    for (int _w = 0; _w < (timeout_ms) && !(cond); _w++) \
    usleep(1000)

// 현재 시각(ms) — 홀드 응답의 소요 시간 검증용.
static int64_t mock_now_ms()
{
    timeval tv;
    ::gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 간이 HTTP/1.1 클라이언트 — 한 소켓으로 GET을 반복한다 (keep-alive 검증이 목적).
// Content-Length로 응답 경계를 끊고, 초과 수신분은 pending에 남겨 다음 응답에 쓴다.
class MockHttpClient
{
public:
    int fd;
    SrsStSocket* io;
    // 이전 read가 더 읽어 둔 다음 응답의 선행분.
    string pending;

public:
    MockHttpClient() : fd(-1), io(NULL)
    {
    }
    virtual ~MockHttpClient()
    {
        close();
    }
    void close()
    {
        srs_freep(io);
        if (fd != -1)
        {
            ::close(fd);
            fd = -1;
        }
    }

public:
    srs_error_t connect(int port)
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1)
        {
            return srs_error_new(ERROR_SOCKET_CREATE, "socket");
        }

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) == -1)
        {
            return srs_error_new(ERROR_SOCKET_CREATE, "connect port=%d", port);
        }

        io = new SrsStSocket(fd);
        // 홀드 타임아웃(기본 3×2s)보다 길게 — 서버가 홀드를 끝내고 응답할 때까지 기다린다.
        io->set_recv_timeout(10 * SRS_UTIME_SECONDS);
        io->set_send_timeout(5 * SRS_UTIME_SECONDS);
        return srs_success;
    }
    // GET 송신 후 응답 1개를 읽는다. code/body 반환, 헤더 원문은 pheader(옵션).
    srs_error_t get(const string& target, int& code, string& body, string* pheader = NULL)
    {
        srs_error_t err = srs_success;

        char req[512];
        snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n", target.c_str());
        if ((err = io->write(req, strlen(req), NULL)) != srs_success)
        {
            return srs_error_wrap(err, "write request");
        }

        // 헤더 끝까지 수신.
        size_t he;
        while ((he = pending.find("\r\n\r\n")) == string::npos)
        {
            char buf[4096];
            ssize_t nread = 0;
            if ((err = io->read(buf, sizeof(buf), &nread)) != srs_success)
            {
                return srs_error_wrap(err, "read header");
            }
            pending.append(buf, (size_t)nread);
        }
        string header = pending.substr(0, he + 4);
        pending.erase(0, he + 4);
        if (pheader)
        {
            *pheader = header;
        }

        // "HTTP/1.1 200 OK" — 상태 코드.
        if (header.length() < 12 || header.compare(0, 9, "HTTP/1.1 ") != 0)
        {
            return srs_error_new(ERROR_HTTP_PARSE_HEADER, "bad status line: %s", header.substr(0, 20).c_str());
        }
        code = ::atoi(header.c_str() + 9);

        // Content-Length만큼 본문 수신 — keep-alive의 응답 경계.
        size_t cl = header.find("Content-Length: ");
        if (cl == string::npos)
        {
            return srs_error_new(ERROR_HTTP_PARSE_HEADER, "no content-length");
        }
        int length = ::atoi(header.c_str() + cl + 16);

        while ((int)pending.length() < length)
        {
            char buf[4096];
            ssize_t nread = 0;
            if ((err = io->read(buf, sizeof(buf), &nread)) != srs_success)
            {
                return srs_error_wrap(err, "read body");
            }
            pending.append(buf, (size_t)nread);
        }
        body = pending.substr(0, (size_t)length);
        pending.erase(0, (size_t)length);
        return err;
    }
};

// 서버를 임시 포트(RTMP/HTTP 모두 0)에 띄우고 HTTP 리스너의 실제 포트를 얻는다.
static srs_error_t mock_http_server_listen(SrsServer* server, int* phttp_port)
{
    srs_error_t err = srs_success;

    _srs_config->rtmp_listen_port = 0;
    _srs_config->llhls_http_port = 0;
    if ((err = server->initialize()) != srs_success)
    {
        return srs_error_wrap(err, "initialize");
    }
    if ((err = server->listen()) != srs_success)
    {
        return srs_error_wrap(err, "listen");
    }

    sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    if (getsockname(server->http_listener_->lfd, (sockaddr*)&addr, &alen) == -1)
    {
        return srs_error_new(ERROR_SOCKET_LISTEN, "getsockname");
    }
    *phttp_port = ntohs(addr.sin_port);

    return err;
}

// 스트림의 storage를 준비한다 — HTTP 라우팅이 _srs_sources->fetch로 찾는 그 경로.
// (소스는 전역 풀에 프로세스 종료까지 남으므로 테스트마다 고유한 stream을 쓴다)
static srs_error_t mock_llhls_storage(const string& stream, SrsLlHlsStorage** pps)
{
    srs_error_t err = srs_success;

    SrsRequest r;
    r.vhost = SRS_CONSTS_RTMP_DEFAULT_VHOST;
    r.app = "live";
    r.stream = stream;

    SrsLiveSource* source = NULL;
    if ((err = _srs_sources->fetch_or_create(&r, &source)) != srs_success)
    {
        return srs_error_wrap(err, "fetch source");
    }

    SrsLlHlsStorage* storage = source->llhls_storage();
    storage->update_config(3, stream, 500 * SRS_UTIME_MILLISECONDS, 2 * SRS_UTIME_SECONDS);
    storage->on_publish();

    *pps = storage;
    return err;
}

// 지연 후 파트를 게시/unpublish하는 백그라운드 스레드 — 홀드 중 게시 도착을 흉내 낸다.
struct MockDelayedPublish
{
    SrsLlHlsStorage* storage;
    int delay_ms;
    // 게시할 파트 (unpub이 아니면).
    string payload;
    bool close_segment;
    // true면 파트 게시 대신 on_unpublish.
    bool unpub;
};

static void* mock_delayed_publish_run(void* arg)
{
    MockDelayedPublish* p = (MockDelayedPublish*)arg;
    usleep(p->delay_ms * 1000);
    if (p->unpub)
    {
        p->storage->on_unpublish();
    }
    else
    {
        p->storage->append_part(p->payload, 480 * SRS_UTIME_MILLISECONDS, true, p->close_segment);
    }
    return NULL;
}

// 완료 조건: 한 커넥션으로 init/m3u8/파트/세그먼트/404를 연속 요청해도 응답 경계
// (Content-Length)가 정확해 다음 요청이 성립한다 — keep-alive (PLANS.md D4).
VOID TEST(AppHttpTest, KeepAliveContentLength)
{
    srs_error_t err = srs_success;

    ::signal(SIGPIPE, SIG_IGN);
    SrsSimpleConfig saved = *_srs_config;

    SrsServer server;
    int port = 0;
    HELPER_ASSERT_SUCCESS(mock_http_server_listen(&server, &port));

    SrsLlHlsStorage* storage = NULL;
    HELPER_ASSERT_SUCCESS(mock_llhls_storage("http1", &storage));
    storage->set_init("INITBYTES");
    storage->append_part("P0", 480 * SRS_UTIME_MILLISECONDS, true, false);
    storage->append_part("P1", 480 * SRS_UTIME_MILLISECONDS, false, true);

    MockHttpClient client;
    HELPER_ASSERT_SUCCESS(client.connect(port));

    // 같은 커넥션으로 연속 요청 — 사이사이 재접속이 없다.
    int code = 0;
    string body, header;

    HELPER_ASSERT_SUCCESS(client.get("/live/http1/init.mp4", code, body, &header));
    EXPECT_EQ(200, code);
    EXPECT_EQ("INITBYTES", body);
    EXPECT_NE(string::npos, header.find("Content-Type: video/mp4"));
    EXPECT_NE(string::npos, header.find("Connection: keep-alive"));
    EXPECT_NE(string::npos, header.find("Access-Control-Allow-Origin: *"));

    HELPER_ASSERT_SUCCESS(client.get("/live/http1.m3u8", code, body, &header));
    EXPECT_EQ(200, code);
    EXPECT_EQ(0, (int)body.find("#EXTM3U"));
    EXPECT_NE(string::npos, body.find("http1/0.0.m4s"));
    EXPECT_NE(string::npos, header.find("Content-Type: application/vnd.apple.mpegurl"));
    EXPECT_NE(string::npos, header.find("Cache-Control: no-cache"));

    HELPER_ASSERT_SUCCESS(client.get("/live/http1/0.0.m4s", code, body));
    EXPECT_EQ(200, code);
    EXPECT_EQ("P0", body);

    // 완결 세그먼트 = 파트 연결.
    HELPER_ASSERT_SUCCESS(client.get("/live/http1/0.m4s", code, body));
    EXPECT_EQ(200, code);
    EXPECT_EQ("P0P1", body);

    // 404들 — 미지 경로 / 없는 스트림. 404 후에도 커넥션은 계속 쓸 수 있다.
    HELPER_ASSERT_SUCCESS(client.get("/live/http1/unknown.txt", code, body));
    EXPECT_EQ(404, code);
    HELPER_ASSERT_SUCCESS(client.get("/live/nosuchstream.m3u8", code, body));
    EXPECT_EQ(404, code);
    HELPER_ASSERT_SUCCESS(client.get("/live/http1/init.mp4", code, body));
    EXPECT_EQ(200, code);
    EXPECT_EQ("INITBYTES", body);

    // 커넥션은 서버에 1개뿐이다 — keep-alive가 실제로 재사용됐다는 증거.
    EXPECT_EQ(1, (int)server.conn_manager->size());

    client.close();
    HTTPHELPER_WAIT_UNTIL(server.conn_manager->empty(), 3000);
    EXPECT_TRUE(server.conn_manager->empty());

    *_srs_config = saved;
}

// 완료 조건: 미래 msn의 블로킹 리로드가 게시까지 홀드되고, 깨어난 응답에는 방금
// 게시된 파트가 실려 있다 (storage 재생성이 notify_all보다 먼저 — S14).
VOID TEST(AppHttpTest, BlockingReloadWakesOnPublish)
{
    srs_error_t err = srs_success;

    ::signal(SIGPIPE, SIG_IGN);
    SrsSimpleConfig saved = *_srs_config;

    SrsServer server;
    int port = 0;
    HELPER_ASSERT_SUCCESS(mock_http_server_listen(&server, &port));

    SrsLlHlsStorage* storage = NULL;
    HELPER_ASSERT_SUCCESS(mock_llhls_storage("http2", &storage));
    storage->set_init("INIT");
    // 세그먼트 0 완결 — 최신 msn=0. 클라이언트는 다음 세그먼트(msn=1)를 기다린다.
    storage->append_part("S0", 480 * SRS_UTIME_MILLISECONDS, true, true);

    // 200ms 뒤 msn=1의 첫 파트 게시.
    MockDelayedPublish pub = {storage, 200, "N0", false, false};
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, mock_delayed_publish_run, &pub));

    MockHttpClient client;
    HELPER_ASSERT_SUCCESS(client.connect(port));

    int code = 0;
    string body;
    int64_t starttime = mock_now_ms();
    HELPER_ASSERT_SUCCESS(client.get("/live/http2.m3u8?_HLS_msn=1&_HLS_part=0", code, body));
    int64_t elapsed = mock_now_ms() - starttime;

    // 응답은 게시(200ms) 이후에 왔고, 방금 게시된 파트가 이미 실려 있다.
    EXPECT_EQ(200, code);
    EXPECT_GE(elapsed, 150);
    EXPECT_LT(elapsed, 3000);
    EXPECT_NE(string::npos, body.find("http2/1.0.m4s"));

    pthread_join(t, NULL);
    *_srs_config = saved;
}

// 완료 조건: 스펙 경계 — 최신+3 이상은 400, 만료 msn은 (홀드 없이) 404,
// 오지 않는 미래는 홀드 타임아웃 후 그 시점 최신으로 200.
VOID TEST(AppHttpTest, BlockingBoundaries)
{
    srs_error_t err = srs_success;

    ::signal(SIGPIPE, SIG_IGN);
    SrsSimpleConfig saved = *_srs_config;
    // 홀드 타임아웃(3×segment)을 600ms로 줄인다 — 타임아웃 경로를 빠르게 검증.
    _srs_config->llhls_segment = 200 * SRS_UTIME_MILLISECONDS;

    SrsServer server;
    int port = 0;
    HELPER_ASSERT_SUCCESS(mock_http_server_listen(&server, &port));

    SrsLlHlsStorage* storage = NULL;
    HELPER_ASSERT_SUCCESS(mock_llhls_storage("http3", &storage));
    storage->set_init("INIT");
    // 세그먼트 0~3 완결, 윈도우 3 → msn 0은 만료. 최신 msn=3.
    for (int i = 0; i < 4; i++)
    {
        storage->append_part("X", 480 * SRS_UTIME_MILLISECONDS, true, true);
    }
    ASSERT_EQ(1, (int)storage->first_msn());
    ASSERT_EQ(3, (int)storage->latest_msn());

    MockHttpClient client;
    HELPER_ASSERT_SUCCESS(client.connect(port));

    int code = 0;
    string body;

    // 최신(3)+3=6 — 너무 먼 미래는 홀드하지 않고 400 (스펙 관례).
    HELPER_ASSERT_SUCCESS(client.get("/live/http3.m3u8?_HLS_msn=6", code, body));
    EXPECT_EQ(400, code);

    // _HLS_part만 있는 요청도 400 (스펙).
    HELPER_ASSERT_SUCCESS(client.get("/live/http3.m3u8?_HLS_part=0", code, body));
    EXPECT_EQ(400, code);

    // 만료된 msn의 파트 — 기다릴 것이 없으므로 즉시 404.
    int64_t starttime = mock_now_ms();
    HELPER_ASSERT_SUCCESS(client.get("/live/http3/0.0.m4s", code, body));
    EXPECT_EQ(404, code);
    EXPECT_LT(mock_now_ms() - starttime, 500);

    // 미완결/없는 세그먼트 전체도 404.
    HELPER_ASSERT_SUCCESS(client.get("/live/http3/9.m4s", code, body));
    EXPECT_EQ(404, code);

    // 오지 않는 미래(최신+1) — 홀드 타임아웃(600ms) 후 그 시점 최신으로 200.
    starttime = mock_now_ms();
    HELPER_ASSERT_SUCCESS(client.get("/live/http3.m3u8?_HLS_msn=4", code, body));
    int64_t elapsed = mock_now_ms() - starttime;
    EXPECT_EQ(200, code);
    EXPECT_GE(elapsed, 500);
    EXPECT_NE(string::npos, body.find("http3/3.0.m4s")); // 최신 상태의 플레이리스트

    *_srs_config = saved;
}

// 완료 조건: 힌트된(아직 없는) 파트 GET이 게시까지 홀드되고, 게시된 바이트가
// 그대로 온다 (스펙 — PRELOAD-HINT URI의 GET은 리소스가 생길 때까지 홀드 후 200).
VOID TEST(AppHttpTest, PreloadHintPartHold)
{
    srs_error_t err = srs_success;

    ::signal(SIGPIPE, SIG_IGN);
    SrsSimpleConfig saved = *_srs_config;

    SrsServer server;
    int port = 0;
    HELPER_ASSERT_SUCCESS(mock_http_server_listen(&server, &port));

    SrsLlHlsStorage* storage = NULL;
    HELPER_ASSERT_SUCCESS(mock_llhls_storage("http4", &storage));
    storage->set_init("INIT");
    // 진행 중 세그먼트 0의 파트 0 — m3u8은 0.1을 힌트하는 상태다.
    storage->append_part("A0", 480 * SRS_UTIME_MILLISECONDS, true, false);

    // 150ms 뒤 힌트된 파트(0.1) 게시.
    MockDelayedPublish pub = {storage, 150, "A1-PAYLOAD", false, false};
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, mock_delayed_publish_run, &pub));

    MockHttpClient client;
    HELPER_ASSERT_SUCCESS(client.connect(port));

    int code = 0;
    string body;
    int64_t starttime = mock_now_ms();
    HELPER_ASSERT_SUCCESS(client.get("/live/http4/0.1.m4s", code, body));
    int64_t elapsed = mock_now_ms() - starttime;

    EXPECT_EQ(200, code);
    EXPECT_GE(elapsed, 100);
    EXPECT_EQ("A1-PAYLOAD", body); // 게시된 바이트와 일치

    pthread_join(t, NULL);
    *_srs_config = saved;
}

// 완료 조건: 홀드 중 unpublish가 오면 타임아웃을 기다리지 않고 깨어나 그 시점
// 상태로 응답하고(m3u8 200 — PRELOAD-HINT 없음), 연결 정리에 누수가 없다.
VOID TEST(AppHttpTest, UnpublishWakesHold)
{
    srs_error_t err = srs_success;

    ::signal(SIGPIPE, SIG_IGN);
    SrsSimpleConfig saved = *_srs_config;

    SrsServer server;
    int port = 0;
    HELPER_ASSERT_SUCCESS(mock_http_server_listen(&server, &port));

    SrsLlHlsStorage* storage = NULL;
    HELPER_ASSERT_SUCCESS(mock_llhls_storage("http5", &storage));
    storage->set_init("INIT");
    storage->append_part("S0", 480 * SRS_UTIME_MILLISECONDS, true, true);

    // 150ms 뒤 unpublish — 홀드 전원 기상 (PLANS.md 리스크 6).
    MockDelayedPublish pub = {storage, 150, "", false, true};
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, mock_delayed_publish_run, &pub));

    MockHttpClient client;
    HELPER_ASSERT_SUCCESS(client.connect(port));

    int code = 0;
    string body;
    int64_t starttime = mock_now_ms();
    HELPER_ASSERT_SUCCESS(client.get("/live/http5.m3u8?_HLS_msn=1", code, body));
    int64_t elapsed = mock_now_ms() - starttime;

    // 홀드 타임아웃(6s)이 아니라 unpublish(150ms) 직후 응답. 힌트는 사라졌다.
    EXPECT_EQ(200, code);
    EXPECT_GE(elapsed, 100);
    EXPECT_LT(elapsed, 3000);
    EXPECT_EQ(string::npos, body.find("PRELOAD-HINT"));

    pthread_join(t, NULL);

    // 홀드가 끝난 커넥션이 정상 종료되고 reap된다 — 스레드 join 누수 없음.
    client.close();
    HTTPHELPER_WAIT_UNTIL(server.conn_manager->empty(), 3000);
    EXPECT_TRUE(server.conn_manager->empty());

    *_srs_config = saved;
}
