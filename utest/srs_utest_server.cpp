// srs_simple — S7: 연결 수명주기 테스트.
// 리소스 매니저의 비동기 reap과, 실소켓으로 SrsServer에 접속하는 RTMP 클라이언트
// 통합 테스트 (핸드셰이크 → connect → publish/play → 종료 후 서버 생존).
// 원본 utest에는 없는 구성 — 원본은 ST 의존이라 실소켓 통합 테스트가 없다.
#include <srs_utest.hpp>

#include <netinet/in.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <srs_app_config.hpp>
#include <srs_app_conn.hpp>
#include <srs_app_rtmp_conn.hpp>
#include <srs_app_server.hpp>
#include <srs_app_st.hpp>
#include <srs_kernel_buffer.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_protocol_amf0.hpp>
#include <srs_protocol_rtmp_stack.hpp>

// cond가 참이 될 때까지 최대 timeout_ms 만큼 1ms 단위로 폴링한다 (srs_utest_app.cpp와 동일).
#define SRVHELPER_WAIT_UNTIL(cond, timeout_ms) \
    for (int _w = 0; _w < (timeout_ms) && !(cond); _w++) usleep(1000)

// 소멸 시 플래그를 세우는 목 리소스 — 매니저의 비동기 해제를 관찰한다.
class MockGcResource : public ISrsResource
{
public:
    volatile bool* destroyed;
    SrsContextId cid_;
public:
    MockGcResource(volatile bool* d) : destroyed(d) {
    }
    virtual ~MockGcResource() {
        *destroyed = true;
    }
public:
    virtual const SrsContextId& get_id() {
        return cid_;
    }
    virtual std::string desc() {
        return "MockGc";
    }
};

// 완료 조건: remove()는 등록만 하고, 해제는 매니저 스레드가 한다 (CLAUDE.md §5.3).
VOID TEST(AppResourceManagerTest, AsyncReap)
{
    srs_error_t err = srs_success;

    SrsResourceManager mgr("test");
    HELPER_ASSERT_SUCCESS(mgr.start());

    volatile bool destroyed = false;
    MockGcResource* r = new MockGcResource(&destroyed);
    mgr.add(r);
    EXPECT_EQ(1, (int)mgr.size());

    // 연결 스레드가 종료 시 부르는 것과 같은 경로.
    mgr.remove(r);
    EXPECT_EQ(0, (int)mgr.size());

    // 다른 스레드(매니저)가 곧 해제한다.
    SRVHELPER_WAIT_UNTIL(destroyed, 2000);
    EXPECT_TRUE(destroyed);
}

VOID TEST(AppResourceManagerTest, DestructorFreesRemaining)
{
    srs_error_t err = srs_success;

    volatile bool destroyed = false;
    if (true) {
        SrsResourceManager mgr("test");
        HELPER_ASSERT_SUCCESS(mgr.start());
        mgr.add(new MockGcResource(&destroyed));
        // remove 없이 소멸 — 매니저가 남은 리소스를 해제한다.
    }
    EXPECT_TRUE(destroyed);
}

// ---- 서버 통합 테스트 헬퍼 ----

// 서버를 임시 포트에 띄우고, 실소켓 클라이언트로 심플 핸드셰이크 + connect까지 수행한다.
class MockRtmpClient
{
public:
    int fd;
    SrsStSocket* io;
    SrsProtocol* protocol;
public:
    MockRtmpClient() : fd(-1), io(NULL), protocol(NULL) {
    }
    virtual ~MockRtmpClient() {
        close();
    }
    void close() {
        srs_freep(protocol);
        srs_freep(io);
        if (fd != -1) {
            ::close(fd);
            fd = -1;
        }
    }
public:
    srs_error_t connect(int port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
            return srs_error_new(ERROR_SOCKET_CREATE, "socket");
        }

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) == -1) {
            return srs_error_new(ERROR_SOCKET_CREATE, "connect port=%d", port);
        }

        io = new SrsStSocket(fd);
        io->set_recv_timeout(5 * SRS_UTIME_SECONDS);
        io->set_send_timeout(5 * SRS_UTIME_SECONDS);
        protocol = new SrsProtocol(io);
        return srs_success;
    }
    // 클라이언트 측 심플 핸드셰이크: C0C1 → S0S1S2 수신 → C2(=S1) 송신.
    srs_error_t handshake() {
        srs_error_t err = srs_success;

        char c0c1[1537];
        memset(c0c1, 0x0f, sizeof(c0c1));
        c0c1[0] = 0x03;
        if ((err = io->write(c0c1, 1537, NULL)) != srs_success) {
            return srs_error_wrap(err, "write c0c1");
        }

        char s0s1s2[3073];
        if ((err = io->read_fully(s0s1s2, 3073, NULL)) != srs_success) {
            return srs_error_wrap(err, "read s0s1s2");
        }
        if (s0s1s2[0] != 0x03) {
            return srs_error_new(ERROR_RTMP_HANDSHAKE, "s0=%#x", s0s1s2[0]);
        }
        // S2는 C1의 복사본이어야 한다 (CLAUDE.md §2.1).
        if (memcmp(s0s1s2 + 1537, c0c1 + 1, 1536) != 0) {
            return srs_error_new(ERROR_RTMP_HANDSHAKE, "s2 != c1");
        }

        // C2 = S1 복사.
        if ((err = io->write(s0s1s2 + 1, 1536, NULL)) != srs_success) {
            return srs_error_wrap(err, "write c2");
        }
        return err;
    }
    srs_error_t connect_app(int port) {
        srs_error_t err = srs_success;

        char tcUrl[128];
        snprintf(tcUrl, sizeof(tcUrl), "rtmp://127.0.0.1:%d/live", port);

        SrsConnectAppPacket* pkt = new SrsConnectAppPacket();
        pkt->command_object->set("app", SrsAmf0Any::str("live"));
        pkt->command_object->set("tcUrl", SrsAmf0Any::str(tcUrl));
        pkt->command_object->set("objectEncoding", SrsAmf0Any::number(0));
        if ((err = protocol->send_and_free_packet(pkt, 0)) != srs_success) {
            return srs_error_wrap(err, "send connect");
        }

        SrsCommonMessage* msg = NULL;
        SrsConnectAppResPacket* res = NULL;
        if ((err = protocol->expect_message<SrsConnectAppResPacket>(&msg, &res)) != srs_success) {
            return srs_error_wrap(err, "expect connect response");
        }
        srs_freep(msg);
        srs_freep(res);
        return err;
    }
    // onStatus(code)가 올 때까지 커맨드 메시지를 소비한다 (onFCPublish 등은 건너뜀).
    srs_error_t expect_on_status(std::string expect_code) {
        srs_error_t err = srs_success;

        while (true) {
            SrsCommonMessage* msg = NULL;
            if ((err = protocol->recv_message(&msg)) != srs_success) {
                return srs_error_wrap(err, "recv");
            }
            if (!msg->header.is_amf0_command()) {
                srs_freep(msg);
                continue;
            }

            SrsBuffer b(msg->payload, msg->size);
            std::string cmd;
            if ((err = srs_amf0_read_string(&b, cmd)) != srs_success) {
                srs_freep(msg);
                return srs_error_wrap(err, "read command name");
            }
            if (cmd != "onStatus") {
                srs_freep(msg);
                continue;
            }

            double tid = 0;
            if ((err = srs_amf0_read_number(&b, tid)) != srs_success) {
                srs_freep(msg);
                return srs_error_wrap(err, "read tid");
            }
            if ((err = srs_amf0_read_null(&b)) != srs_success) {
                srs_freep(msg);
                return srs_error_wrap(err, "read null");
            }
            SrsAmf0Any* any = NULL;
            if ((err = srs_amf0_read_any(&b, &any)) != srs_success) {
                srs_freep(msg);
                return srs_error_wrap(err, "read data");
            }

            std::string code;
            if (any->is_object()) {
                SrsAmf0Any* prop = any->to_object()->ensure_property_string(StatusCode);
                if (prop) {
                    code = prop->to_str();
                }
            }
            srs_freep(any);
            srs_freep(msg);

            if (code == expect_code) {
                return srs_success;
            }
            // 기대와 다른 onStatus(Play.Reset 등)는 건너뛴다.
        }
    }
    // audio/video 미디어 메시지 송신.
    srs_error_t send_media(int8_t type, uint32_t timestamp, int stream_id) {
        SrsMessageHeader header;
        header.message_type = type;
        header.timestamp = timestamp;
        header.stream_id = stream_id;
        header.prefer_cid = (type == RTMP_MSG_AudioMessage) ? RTMP_CID_Audio : RTMP_CID_Video;

        char payload[] = { (char)0xaf, (char)0x01, (char)0x00 };
        header.payload_length = sizeof(payload);

        char* p = new char[sizeof(payload)];
        memcpy(p, payload, sizeof(payload));

        SrsSharedPtrMessage* msg = new SrsSharedPtrMessage();
        srs_error_t err = msg->create(&header, p, sizeof(payload));
        if (err != srs_success) {
            srs_freep(msg);
            return err;
        }
        return protocol->send_and_free_message(msg, stream_id);
    }
};

// 임시 포트로 서버를 띄우고 실제 바인딩된 포트를 얻는다.
static srs_error_t mock_server_listen(SrsServer* server, int* pport)
{
    srs_error_t err = srs_success;

    _srs_config->listen_port = 0;
    if ((err = server->initialize()) != srs_success) {
        return srs_error_wrap(err, "initialize");
    }
    if ((err = server->listen()) != srs_success) {
        return srs_error_wrap(err, "listen");
    }

    sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    if (getsockname(server->rtmp_listener_->lfd, (sockaddr*)&addr, &alen) == -1) {
        return srs_error_new(ERROR_SOCKET_LISTEN, "getsockname");
    }
    *pport = ntohs(addr.sin_port);

    return err;
}

// 완료 조건: FMLE publish 시퀀스 전체가 실소켓으로 통하고, unpublish 후
// 재-publish 루프로 돌아가며, 클라이언트 종료 후 연결이 reap되고 서버는 산다.
VOID TEST(AppServerTest, FmlePublishLifecycle)
{
    srs_error_t err = srs_success;

    // 끊긴 소켓 write의 SIGPIPE로 utest가 죽지 않게 (srs_main_server.cpp와 동일).
    ::signal(SIGPIPE, SIG_IGN);

    SrsServer server;
    int port = 0;
    HELPER_ASSERT_SUCCESS(mock_server_listen(&server, &port));

    MockRtmpClient client;
    HELPER_ASSERT_SUCCESS(client.connect(port));
    HELPER_ASSERT_SUCCESS(client.handshake());
    HELPER_ASSERT_SUCCESS(client.connect_app(port));

    // FMLE publish 시퀀스를 파이프라인으로 송신 (OBS/ffmpeg과 동일 순서).
    HELPER_ASSERT_SUCCESS(client.protocol->send_and_free_packet(SrsFMLEStartPacket::create_release_stream("livestream"), 0));
    HELPER_ASSERT_SUCCESS(client.protocol->send_and_free_packet(SrsFMLEStartPacket::create_FC_publish("livestream"), 0));
    if (true) {
        SrsCreateStreamPacket* pkt = new SrsCreateStreamPacket();
        pkt->transaction_id = 4;
        HELPER_ASSERT_SUCCESS(client.protocol->send_and_free_packet(pkt, 0));
    }
    if (true) {
        SrsPublishPacket* pkt = new SrsPublishPacket();
        pkt->stream_name = "livestream";
        HELPER_ASSERT_SUCCESS(client.protocol->send_and_free_packet(pkt, 1));
    }

    // 응답: _result(releaseStream tid=2) → _result(FCPublish tid=3) → _result(createStream tid=4).
    if (true) {
        SrsCommonMessage* msg = NULL;
        SrsFMLEStartResPacket* res = NULL;
        HELPER_ASSERT_SUCCESS(client.protocol->expect_message<SrsFMLEStartResPacket>(&msg, &res));
        EXPECT_EQ(2, (int)res->transaction_id);
        srs_freep(msg);
        srs_freep(res);
    }
    if (true) {
        SrsCommonMessage* msg = NULL;
        SrsFMLEStartResPacket* res = NULL;
        HELPER_ASSERT_SUCCESS(client.protocol->expect_message<SrsFMLEStartResPacket>(&msg, &res));
        EXPECT_EQ(3, (int)res->transaction_id);
        srs_freep(msg);
        srs_freep(res);
    }
    if (true) {
        SrsCommonMessage* msg = NULL;
        SrsCreateStreamResPacket* res = NULL;
        HELPER_ASSERT_SUCCESS(client.protocol->expect_message<SrsCreateStreamResPacket>(&msg, &res));
        EXPECT_EQ(4, (int)res->transaction_id);
        EXPECT_EQ(1, (int)res->stream_id);
        srs_freep(msg);
        srs_freep(res);
    }
    // onFCPublish를 지나 onStatus(NetStream.Publish.Start) — publish 인가 완료.
    HELPER_ASSERT_SUCCESS(client.expect_on_status(StatusCodePublishStart));

    // 미디어 송신 — 서버 publishing 루프가 audio/video로 분류한다.
    for (int i = 0; i < 10; i++) {
        HELPER_ASSERT_SUCCESS(client.send_media(RTMP_MSG_AudioMessage, 10 * i, 1));
        HELPER_ASSERT_SUCCESS(client.send_media(RTMP_MSG_VideoMessage, 10 * i, 1));
    }

    // FCUnpublish → 서버가 3종 응답 후 재-publish 루프로 (ERROR_CONTROL_REPUBLISH 경로).
    if (true) {
        SrsFMLEStartPacket* pkt = new SrsFMLEStartPacket();
        pkt->command_name = RTMP_AMF0_COMMAND_UNPUBLISH;
        pkt->transaction_id = 6;
        pkt->stream_name = "livestream";
        HELPER_ASSERT_SUCCESS(client.protocol->send_and_free_packet(pkt, 1));
    }
    if (true) {
        SrsCommonMessage* msg = NULL;
        SrsFMLEStartResPacket* res = NULL;
        HELPER_ASSERT_SUCCESS(client.protocol->expect_message<SrsFMLEStartResPacket>(&msg, &res));
        EXPECT_EQ(6, (int)res->transaction_id);
        srs_freep(msg);
        srs_freep(res);
    }
    HELPER_ASSERT_SUCCESS(client.expect_on_status(StatusCodeUnpublishSuccess));

    // 클라이언트 종료 → 서버 연결이 종료되고 매니저가 reap.
    EXPECT_EQ(1, (int)server.conn_manager->size());
    client.close();
    SRVHELPER_WAIT_UNTIL(server.conn_manager->empty(), 3000);
    EXPECT_TRUE(server.conn_manager->empty());

    // 서버는 살아있다 — 두 번째 클라이언트가 정상 접속된다.
    MockRtmpClient client2;
    HELPER_ASSERT_SUCCESS(client2.connect(port));
    HELPER_ASSERT_SUCCESS(client2.handshake());
    HELPER_ASSERT_SUCCESS(client2.connect_app(port));
}

// 완료 조건: play 시퀀스 — StreamBegin + onStatus(Play.Start)까지 받고,
// 연결은 S8 전 스텁 상태로 유지된다.
VOID TEST(AppServerTest, PlayBootstrap)
{
    srs_error_t err = srs_success;

    ::signal(SIGPIPE, SIG_IGN);

    SrsServer server;
    int port = 0;
    HELPER_ASSERT_SUCCESS(mock_server_listen(&server, &port));

    MockRtmpClient client;
    HELPER_ASSERT_SUCCESS(client.connect(port));
    HELPER_ASSERT_SUCCESS(client.handshake());
    HELPER_ASSERT_SUCCESS(client.connect_app(port));

    // createStream(tid=2) → _result(streamId=1)
    if (true) {
        SrsCreateStreamPacket* pkt = new SrsCreateStreamPacket();
        pkt->transaction_id = 2;
        HELPER_ASSERT_SUCCESS(client.protocol->send_and_free_packet(pkt, 0));
    }
    if (true) {
        SrsCommonMessage* msg = NULL;
        SrsCreateStreamResPacket* res = NULL;
        HELPER_ASSERT_SUCCESS(client.protocol->expect_message<SrsCreateStreamResPacket>(&msg, &res));
        EXPECT_EQ(1, (int)res->stream_id);
        srs_freep(msg);
        srs_freep(res);
    }
    // play("livestream", sid=1)
    if (true) {
        SrsPlayPacket* pkt = new SrsPlayPacket();
        pkt->stream_name = "livestream";
        HELPER_ASSERT_SUCCESS(client.protocol->send_and_free_packet(pkt, 1));
    }

    // StreamBegin(sid=1) — stream_id=0으로 온다 (CLAUDE.md §2.5).
    if (true) {
        SrsCommonMessage* msg = NULL;
        SrsUserControlPacket* pkt = NULL;
        HELPER_ASSERT_SUCCESS(client.protocol->expect_message<SrsUserControlPacket>(&msg, &pkt));
        EXPECT_EQ(SrcPCUCStreamBegin, pkt->event_type);
        EXPECT_EQ(1, pkt->event_data);
        srs_freep(msg);
        srs_freep(pkt);
    }
    // onStatus(Play.Reset)를 지나 onStatus(Play.Start).
    HELPER_ASSERT_SUCCESS(client.expect_on_status(StatusCodeStreamStart));

    // 잠시 유지 — playing 스텁이 recv 타임아웃으로 살아있는지.
    usleep(50 * 1000);
    EXPECT_EQ(1, (int)server.conn_manager->size());

    // 클라이언트가 끊으면 연결이 reap된다.
    client.close();
    SRVHELPER_WAIT_UNTIL(server.conn_manager->empty(), 3000);
    EXPECT_TRUE(server.conn_manager->empty());
}

// 완료 조건: 비정상 종료(kill -9 흉내 — 핸드셰이크 중 절단)에도 서버가 죽지 않고
// 다중 접속을 계속 받는다.
VOID TEST(AppServerTest, SurvivesAbruptClose)
{
    srs_error_t err = srs_success;

    ::signal(SIGPIPE, SIG_IGN);

    SrsServer server;
    int port = 0;
    HELPER_ASSERT_SUCCESS(mock_server_listen(&server, &port));

    // 동시 다중 접속 + 제각각 비정상 절단.
    MockRtmpClient clients[3];
    for (int i = 0; i < 3; i++) {
        HELPER_ASSERT_SUCCESS(clients[i].connect(port));
    }
    SRVHELPER_WAIT_UNTIL(server.conn_manager->size() == 3, 2000);
    EXPECT_EQ(3, (int)server.conn_manager->size());

    // 0: 핸드셰이크 없이 절단, 1: C0C1만 보내고 절단, 2: 핸드셰이크 후 절단.
    clients[0].close();
    if (true) {
        char c0c1[1537];
        memset(c0c1, 0, sizeof(c0c1));
        c0c1[0] = 0x03;
        HELPER_ASSERT_SUCCESS(clients[1].io->write(c0c1, 1537, NULL));
        clients[1].close();
    }
    HELPER_ASSERT_SUCCESS(clients[2].handshake());
    clients[2].close();

    SRVHELPER_WAIT_UNTIL(server.conn_manager->empty(), 3000);
    EXPECT_TRUE(server.conn_manager->empty());

    // 서버 생존 확인.
    MockRtmpClient again;
    HELPER_ASSERT_SUCCESS(again.connect(port));
    HELPER_ASSERT_SUCCESS(again.handshake());
    HELPER_ASSERT_SUCCESS(again.connect_app(port));
}
