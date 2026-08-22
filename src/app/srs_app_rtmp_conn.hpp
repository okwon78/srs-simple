// srs_simple — 원본: trunk/src/app/srs_app_rtmp_conn.hpp
// 연결 수명주기: do_cycle(핸드셰이크→connect) → service_cycle(협상→재-publish 루프)
//             → stream_service_cycle(식별→publish/play 분기).
// 원본 대비 제거: edge/refer/security/bandwidth/http_hooks/statistic/APM, 수신 전용
// 코루틴(SrsQueueRecvThread/SrsPublishRecvThread — 연결당 1스레드로 직접 recv, CLAUDE.md §5.1),
// reload 핸들러, ISrsExpire — CLAUDE.md §5.6 S7.
#ifndef SRS_APP_RTMP_CONN_HPP
#define SRS_APP_RTMP_CONN_HPP

#include <srs_core.hpp>

#include <string>

#include <srs_app_conn.hpp>
#include <srs_app_st.hpp>
#include <srs_protocol_rtmp_stack.hpp>

class SrsServer;
class SrsRtmpServer;
class SrsRequest;
class SrsResponse;
class SrsCommonMessage;
class SrsStSocket;
class SrsLiveSource;
class SrsLiveConsumer;

// Some information of client.
class SrsClientInfo
{
public:
    // The type of client, play or publish.
    SrsRtmpConnType type;
    // Original request object from client.
    SrsRequest* req;
    // Response object to client.
    SrsResponse* res;
public:
    SrsClientInfo();
    virtual ~SrsClientInfo();
};

// The client provides the main logic control for RTMP clients.
class SrsRtmpConn : public ISrsConnection, public ISrsStartable, public ISrsCoroutineHandler
{
private:
    SrsServer* server;
    SrsRtmpServer* rtmp;
    // About the rtmp client.
    SrsClientInfo* info;
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
    SrsRtmpConn(SrsServer* svr, srs_netfd_t c, std::string cip, int cport);
    virtual ~SrsRtmpConn();
// Interface ISrsResource.
public:
    virtual std::string desc();
protected:
    virtual srs_error_t do_cycle();
private:
    // When valid and connected to vhost/app, service the client.
    virtual srs_error_t service_cycle();
    // The stream(play/publish) service cycle, identify client first.
    virtual srs_error_t stream_service_cycle();
    virtual srs_error_t playing(SrsLiveSource* source);
    virtual srs_error_t do_playing(SrsLiveSource* source, SrsLiveConsumer* consumer);
    virtual srs_error_t publishing(SrsLiveSource* source);
    virtual srs_error_t acquire_publish(SrsLiveSource* source);
    virtual void release_publish(SrsLiveSource* source);
    virtual srs_error_t do_publishing(SrsLiveSource* source);
    virtual srs_error_t handle_publish_message(SrsLiveSource* source, SrsCommonMessage* msg);
    virtual srs_error_t process_publish_message(SrsLiveSource* source, SrsCommonMessage* msg);
    virtual srs_error_t process_play_control_msg(SrsCommonMessage* msg);
// Interface ISrsStartable
public:
    // Start the client green thread.
    // when server get a client from listener,
    // 1. server will create an concrete connection(for instance, RTMP connection),
    // 2. then add connection to its connection manager,
    // 3. start the client thread by invoke this start()
    virtual srs_error_t start();
// Interface ISrsCoroutineHandler
public:
    // The thread cycle function,
    // when serve connection completed, terminate the loop which will terminate the thread.
    virtual srs_error_t cycle();
// Interface ISrsConnection.
public:
    virtual std::string remote_ip();
    virtual const SrsContextId& get_id();
};

#endif
