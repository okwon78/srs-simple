// srs_simple — S2: 코루틴(pthread)/실소켓/TCP 리스너 스모크 테스트.
// 원본 utest에는 ST 스레드 테스트(srs_utest_service.cpp 등)가 대응한다.
#include <srs_utest.hpp>

#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <srs_app_listener.hpp>
#include <srs_app_st.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

// cond가 참이 될 때까지 최대 timeout_ms 만큼 1ms 단위로 폴링한다.
#define HELPER_WAIT_UNTIL(cond, timeout_ms) \
    for (int _w = 0; _w < (timeout_ms) && !(cond); _w++) usleep(1000)

// pull()이 실패할 때까지 도는 전형적인 수명주기 핸들러 (srs_app_st.hpp 주석의 두 번째 패턴).
class MockCycleHandler : public ISrsCoroutineHandler
{
public:
    SrsCoroutine* trd;
    volatile int count;
    SrsContextId cid_in_cycle;
public:
    MockCycleHandler() : trd(NULL), count(0) {
    }
    virtual ~MockCycleHandler() {
    }
public:
    virtual srs_error_t cycle() {
        srs_error_t err = srs_success;
        cid_in_cycle = _srs_context->get_id();
        while ((err = trd->pull()) == srs_success) {
            count = count + 1;
            usleep(1000);
        }
        return err;
    }
};

// 한 번 일하고 정상 종료하는 핸들러 (첫 번째 패턴).
class MockOneCycleHandler : public ISrsCoroutineHandler
{
public:
    volatile bool done;
    srs_error_t err_to_return;
public:
    MockOneCycleHandler() : done(false), err_to_return(srs_success) {
    }
    virtual ~MockOneCycleHandler() {
    }
public:
    virtual srs_error_t cycle() {
        done = true;
        return err_to_return;
    }
};

VOID TEST(AppCoroutineTest, StartInterruptStop)
{
    srs_error_t err = srs_success;

    MockCycleHandler h;
    SrsSTCoroutine trd("test", &h);
    h.trd = &trd;

    HELPER_EXPECT_SUCCESS(trd.start());
    HELPER_WAIT_UNTIL(h.count > 0, 2000);
    EXPECT_GT((int)h.count, 0);

    // 실행 중에는 pull()이 성공한다.
    HELPER_EXPECT_SUCCESS(trd.pull());

    // interrupt 후 pull()은 ERROR_THREAD_INTERRUPED를 돌려주고, 워커는 이를 보고 종료한다.
    trd.interrupt();
    err = trd.pull();
    EXPECT_EQ(ERROR_THREAD_INTERRUPED, srs_error_code(err));
    srs_freep(err);

    trd.stop();
}

VOID TEST(AppCoroutineTest, CycleDoneNormally)
{
    srs_error_t err = srs_success;

    MockOneCycleHandler h;
    SrsSTCoroutine trd("test", &h);

    HELPER_EXPECT_SUCCESS(trd.start());
    // cycle_done까지 대기 — utest의 #define private public 트릭으로 내부 상태를 본다.
    HELPER_WAIT_UNTIL(trd.cycle_done, 2000);
    EXPECT_TRUE(h.done);

    // 정상 완료된 코루틴은 stop 후에도 pull()이 성공이다 (ERROR_THREAD_TERMINATED가 아님).
    trd.stop();
    HELPER_EXPECT_SUCCESS(trd.pull());
}

VOID TEST(AppCoroutineTest, CycleReturnsError)
{
    srs_error_t err = srs_success;

    MockOneCycleHandler h;
    h.err_to_return = srs_error_new(ERROR_RTMP_HANDSHAKE, "mock failed");
    SrsSTCoroutine trd("test", &h);

    HELPER_EXPECT_SUCCESS(trd.start());

    // 워커의 에러가 pull()로 전파된다. wrap이므로 코드는 루트 원인을 유지한다.
    srs_error_t perr = srs_success;
    HELPER_WAIT_UNTIL((perr = trd.pull()) != srs_success, 2000);
    EXPECT_EQ(ERROR_RTMP_HANDSHAKE, srs_error_code(perr));
    srs_freep(perr);

    trd.stop();
}

VOID TEST(AppCoroutineTest, StopBeforeCycleRuns)
{
    srs_error_t err = srs_success;

    // 시작하지 않고 stop — pull은 ERROR_THREAD_TERMINATED.
    MockOneCycleHandler h;
    SrsSTCoroutine trd("test", &h);
    trd.stop();

    err = trd.pull();
    EXPECT_EQ(ERROR_THREAD_TERMINATED, srs_error_code(err));
    srs_freep(err);

    // disposed 후 start는 거부된다.
    err = trd.start();
    EXPECT_EQ(ERROR_THREAD_DISPOSED, srs_error_code(err));
    srs_freep(err);
}

VOID TEST(AppCoroutineTest, DoubleStart)
{
    srs_error_t err = srs_success;

    MockCycleHandler h;
    SrsSTCoroutine trd("test", &h);
    h.trd = &trd;

    HELPER_EXPECT_SUCCESS(trd.start());
    err = trd.start();
    EXPECT_EQ(ERROR_THREAD_STARTED, srs_error_code(err));
    srs_freep(err);

    trd.stop();
}

VOID TEST(AppCoroutineTest, ContextIdPerCoroutine)
{
    srs_error_t err = srs_success;

    // 각 코루틴은 자기 cid를 만들고 스레드 컨텍스트에 설정한다 — 연결 단위 로그 추적의 근간.
    MockCycleHandler h;
    SrsSTCoroutine trd("test", &h);
    h.trd = &trd;

    HELPER_EXPECT_SUCCESS(trd.start());
    HELPER_WAIT_UNTIL(h.count > 0, 2000);

    EXPECT_FALSE(trd.cid().empty());
    EXPECT_EQ(trd.cid(), h.cid_in_cycle);
    // utest 메인 스레드의 cid와는 다르다.
    EXPECT_NE(_srs_context->get_id(), h.cid_in_cycle);

    trd.stop();
}

VOID TEST(AppCoroutineTest, DummyCoroutine)
{
    srs_error_t err = srs_success;

    SrsDummyCoroutine dummy;
    err = dummy.start();
    EXPECT_EQ(ERROR_THREAD_DUMMY, srs_error_code(err));
    srs_freep(err);

    err = dummy.pull();
    EXPECT_EQ(ERROR_THREAD_DUMMY, srs_error_code(err));
    srs_freep(err);
}

VOID TEST(AppStSocketTest, ReadWriteFully)
{
    srs_error_t err = srs_success;

    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    SrsStSocket a(fds[0]);
    SrsStSocket b(fds[1]);

    ssize_t nn = 0;
    HELPER_EXPECT_SUCCESS(a.write((void*)"Hello, world!", 13, &nn));
    EXPECT_EQ(13, (int)nn);
    EXPECT_EQ(13, (int)a.get_send_bytes());

    char buf[16];
    HELPER_EXPECT_SUCCESS(b.read_fully(buf, 13, &nn));
    EXPECT_EQ(13, (int)nn);
    EXPECT_EQ(0, memcmp(buf, "Hello, world!", 13));
    EXPECT_EQ(13, (int)b.get_recv_bytes());

    ::close(fds[0]);
    ::close(fds[1]);
}

VOID TEST(AppStSocketTest, Writev)
{
    srs_error_t err = srs_success;

    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    SrsStSocket a(fds[0]);
    SrsStSocket b(fds[1]);

    iovec iovs[2];
    iovs[0].iov_base = (void*)"Hello";
    iovs[0].iov_len = 5;
    iovs[1].iov_base = (void*)"World";
    iovs[1].iov_len = 5;

    ssize_t nn = 0;
    HELPER_EXPECT_SUCCESS(a.writev(iovs, 2, &nn));
    EXPECT_EQ(10, (int)nn);

    char buf[16];
    HELPER_EXPECT_SUCCESS(b.read_fully(buf, 10, NULL));
    EXPECT_EQ(0, memcmp(buf, "HelloWorld", 10));

    ::close(fds[0]);
    ::close(fds[1]);
}

VOID TEST(AppStSocketTest, RecvTimeout)
{
    srs_error_t err = srs_success;

    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    // 완료 조건: 타임아웃은 ERROR_SOCKET_TIMEOUT — 연결이 살아있는, 재시도 가능한 에러.
    SrsStSocket b(fds[1]);
    b.set_recv_timeout(50 * SRS_UTIME_MILLISECONDS);
    EXPECT_EQ(50 * SRS_UTIME_MILLISECONDS, b.get_recv_timeout());

    char buf[16];
    err = b.read(buf, sizeof(buf), NULL);
    EXPECT_EQ(ERROR_SOCKET_TIMEOUT, srs_error_code(err));
    srs_freep(err);

    err = b.read_fully(buf, sizeof(buf), NULL);
    EXPECT_EQ(ERROR_SOCKET_TIMEOUT, srs_error_code(err));
    srs_freep(err);

    ::close(fds[0]);
    ::close(fds[1]);
}

VOID TEST(AppStSocketTest, ReadEOF)
{
    srs_error_t err = srs_success;

    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    // 상대가 닫으면 read는 ERROR_SOCKET_READ (원본과 동일하게 errno=ECONNRESET).
    ::close(fds[0]);

    SrsStSocket b(fds[1]);
    char buf[16];
    err = b.read(buf, sizeof(buf), NULL);
    EXPECT_EQ(ERROR_SOCKET_READ, srs_error_code(err));
    srs_freep(err);

    ::close(fds[1]);
}

class MockTcpHandler : public ISrsTcpHandler
{
public:
    volatile int nn_clients;
    srs_netfd_t last_fd;
public:
    MockTcpHandler() : nn_clients(0), last_fd(SRS_NETFD_INVALID) {
    }
    virtual ~MockTcpHandler() {
        srs_close_stfd(last_fd);
    }
public:
    virtual srs_error_t on_tcp_client(ISrsListener* listener, srs_netfd_t stfd) {
        srs_close_stfd(last_fd);
        last_fd = stfd;
        nn_clients = nn_clients + 1;
        return srs_success;
    }
};

// 완료 조건: 에코 수준의 리스너 스모크 테스트 (스레드 생성·종료·interrupt).
VOID TEST(AppListenerTest, AcceptClient)
{
    srs_error_t err = srs_success;

    MockTcpHandler h;
    SrsTcpListener l(&h);

    // 포트 0 = OS가 임시 포트를 배정 (테스트 간 충돌 방지).
    l.set_endpoint("127.0.0.1", 0);
    HELPER_ASSERT_SUCCESS(l.listen());

    // 실제 바인딩된 포트는 리슨 fd에서 조회한다.
    sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    ASSERT_EQ(0, getsockname(l.lfd, (sockaddr*)&addr, &alen));
    int port = ntohs(addr.sin_port);
    EXPECT_GT(port, 0);

    // 클라이언트 접속 → handler 콜백이 불린다.
    int c = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(-1, c);
    ASSERT_EQ(0, ::connect(c, (sockaddr*)&addr, sizeof(addr)));

    HELPER_WAIT_UNTIL(h.nn_clients > 0, 2000);
    EXPECT_EQ(1, (int)h.nn_clients);
    EXPECT_NE(SRS_NETFD_INVALID, h.last_fd);

    // accept된 fd로 실제 데이터가 흐른다 (에코 수준 확인).
    SrsStSocket skt(h.last_fd);
    ASSERT_EQ(5, ::send(c, "Hello", 5, 0));
    char buf[8];
    HELPER_EXPECT_SUCCESS(skt.read_fully(buf, 5, NULL));
    EXPECT_EQ(0, memcmp(buf, "Hello", 5));

    // 두 번째 클라이언트도 받는다.
    int c2 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_EQ(0, ::connect(c2, (sockaddr*)&addr, sizeof(addr)));
    HELPER_WAIT_UNTIL(h.nn_clients > 1, 2000);
    EXPECT_EQ(2, (int)h.nn_clients);

    ::close(c);
    ::close(c2);

    // close는 accept 스레드를 interrupt하고 join한다.
    l.close();
}

VOID TEST(AppListenerTest, CloseWithoutClient)
{
    srs_error_t err = srs_success;

    MockTcpHandler h;
    SrsTcpListener l(&h);
    l.set_endpoint("127.0.0.1", 0);
    HELPER_ASSERT_SUCCESS(l.listen());

    // 클라이언트 없이 accept 대기 중인 스레드도 타임아웃으로 깨어나 종료된다.
    l.close();
    EXPECT_EQ(SRS_NETFD_INVALID, l.lfd);
    EXPECT_EQ(0, (int)h.nn_clients);
}

VOID TEST(AppListenerTest, ListenNotConfigured)
{
    srs_error_t err = srs_success;

    // endpoint 미설정이면 listen은 조용히 무시된다 (원본과 동일).
    MockTcpHandler h;
    SrsTcpListener l(&h);
    HELPER_EXPECT_SUCCESS(l.listen());
    EXPECT_EQ(SRS_NETFD_INVALID, l.lfd);
}
