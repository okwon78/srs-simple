// srs_simple — 원본: trunk/src/app/srs_app_server.cpp (do_on_tcp_client:1332)
#include <srs_app_server.hpp>

#include <srs_app_config.hpp>
#include <srs_app_rtmp_conn.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>
#include <srs_protocol_utility.hpp>

using namespace std;

SrsServer::SrsServer()
{
    conn_manager = new SrsResourceManager("RTMP");
    rtmp_listener_ = new SrsTcpListener(this);
}

SrsServer::~SrsServer()
{
    // 새 연결부터 끊고(accept 스레드 join), 그 다음 매니저가 남은 연결을 해제한다.
    srs_freep(rtmp_listener_);
    srs_freep(conn_manager);
}

srs_error_t SrsServer::initialize()
{
    srs_error_t err = srs_success;

    if ((err = conn_manager->start()) != srs_success) {
        return srs_error_wrap(err, "conn manager");
    }

    return err;
}

srs_error_t SrsServer::listen()
{
    srs_error_t err = srs_success;

    rtmp_listener_->set_label("RTMP")->set_endpoint("0.0.0.0", _srs_config->listen_port);
    if ((err = rtmp_listener_->listen()) != srs_success) {
        return srs_error_wrap(err, "rtmp listen");
    }

    return err;
}

srs_error_t SrsServer::on_tcp_client(ISrsListener* listener, srs_netfd_t stfd)
{
    srs_error_t err = do_on_tcp_client(listener, stfd);

    // We always try to close the stfd, because it should be NULL if it has been handled or closed.
    srs_close_stfd(stfd);

    return err;
}

srs_error_t SrsServer::do_on_tcp_client(ISrsListener* listener, srs_netfd_t& stfd)
{
    srs_error_t err = srs_success;

    int fd = srs_netfd_fileno(stfd);
    string ip = srs_get_peer_ip(fd);
    int port = srs_get_peer_port(fd);

    // Ignore if ip is empty, for example, load balancer keepalive.
    if (ip.empty()) {
        return err;
    }

    // From now on, we always handle the stfd, so we set the original one to invalid.
    srs_netfd_t stfd2 = stfd;
    stfd = SRS_NETFD_INVALID;

    SrsRtmpConn* conn = new SrsRtmpConn(this, stfd2, ip, port);

    // Directly add the resource to managers.
    conn_manager->add(conn);

    // 연결 전용 스레드 시작. 실패하면 매니저가 다른 스레드에서 해제한다.
    if ((err = conn->start()) != srs_success) {
        conn_manager->remove(conn);
        return srs_error_wrap(err, "start conn coroutine");
    }

    return err;
}

void SrsServer::remove(ISrsResource* c)
{
    // 실제 해제는 conn_manager의 reaper 스레드가 한다.
    conn_manager->remove(c);
}
