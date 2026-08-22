// srs_simple — 원본: trunk/src/app/srs_app_listener.cpp:279-340 (SrsTcpListener)
//            + protocol/srs_protocol_st.cpp (srs_tcp_listen/srs_fd_*)
#include <srs_app_listener.hpp>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <srs_kernel_log.hpp>

using namespace std;

// accept 스레드가 pull() 검사를 위해 주기적으로 깨어나는 간격.
// 원본 ST는 interrupt가 블록된 accept를 즉시 깨우지만, pthread에서는 poll 타임아웃으로
// 대신한다 (CLAUDE.md §5.1) — accept는 SO_RCVTIMEO를 존중하지 않는 플랫폼(macOS)이 있다.
// close() 지연의 상한이기도 하다.
#define SRS_TCP_ACCEPT_TIMEOUT_MS 100

srs_error_t srs_fd_closeexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    flags |= FD_CLOEXEC;
    if (fcntl(fd, F_SETFD, flags) == -1)
    {
        return srs_error_new(ERROR_SOCKET_CREATE, "closeexec fd=%d", fd);
    }

    return srs_success;
}

srs_error_t srs_fd_reuseaddr(int fd)
{
    int v = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(int)) == -1)
    {
        return srs_error_new(ERROR_SOCKET_SETREUSE, "reuseaddr fd=%d", fd);
    }

    return srs_success;
}

srs_error_t srs_tcp_listen(string ip, int port, srs_netfd_t *pfd)
{
    srs_error_t err = srs_success;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
    {
        return srs_error_new(ERROR_SOCKET_CREATE, "socket");
    }

    if ((err = srs_fd_closeexec(fd)) != srs_success)
    {
        ::close(fd);
        return srs_error_wrap(err, "set closeexec");
    }

    if ((err = srs_fd_reuseaddr(fd)) != srs_success)
    {
        ::close(fd);
        return srs_error_wrap(err, "set reuseaddr");
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
    {
        ::close(fd);
        return srs_error_new(ERROR_SOCKET_BIND, "invalid ip=%s", ip.c_str());
    }

    if (::bind(fd, (sockaddr *)&addr, sizeof(addr)) == -1)
    {
        ::close(fd);
        return srs_error_new(ERROR_SOCKET_BIND, "bind %s:%d", ip.c_str(), port);
    }

    if (::listen(fd, 512) == -1)
    {
        ::close(fd);
        return srs_error_new(ERROR_SOCKET_LISTEN, "listen %s:%d", ip.c_str(), port);
    }

    *pfd = fd;

    return err;
}

ISrsListener::ISrsListener()
{
}

ISrsListener::~ISrsListener()
{
}

ISrsTcpHandler::ISrsTcpHandler()
{
}

ISrsTcpHandler::~ISrsTcpHandler()
{
}

SrsTcpListener::SrsTcpListener(ISrsTcpHandler *h)
{
    handler = h;
    port_ = 0;
    lfd = SRS_NETFD_INVALID;
    label_ = "TCP";
    trd = new SrsDummyCoroutine();
}

SrsTcpListener::~SrsTcpListener()
{
    close();
    srs_freep(trd);
}

SrsTcpListener *SrsTcpListener::set_label(const std::string &label)
{
    label_ = label;
    return this;
}

SrsTcpListener *SrsTcpListener::set_endpoint(const std::string &i, int p)
{
    ip = i;
    port_ = p;
    return this;
}

int SrsTcpListener::port()
{
    return port_;
}

srs_error_t SrsTcpListener::listen()
{
    srs_error_t err = srs_success;

    // Ignore if not configured.
    if (ip.empty())
        return err;

    srs_close_stfd(lfd);
    if ((err = srs_tcp_listen(ip, port_, &lfd)) != srs_success)
    {
        return srs_error_wrap(err, "listen at %s:%d", ip.c_str(), port_);
    }

    srs_freep(trd);
    trd = new SrsSTCoroutine("tcp", this);
    if ((err = trd->start()) != srs_success)
    {
        return srs_error_wrap(err, "start coroutine");
    }

    srs_trace("%s listen at tcp://%s:%d, fd=%d", label_.c_str(), ip.c_str(), port_, srs_netfd_fileno(lfd));

    return err;
}

void SrsTcpListener::close()
{
    // stop()은 join이다 — poll 타임아웃으로 깨어난 스레드가 pull()에서 종료를
    // 발견할 때까지(최대 SRS_TCP_ACCEPT_TIMEOUT_MS) 기다린 뒤에 fd를 닫는다.
    trd->stop();
    srs_close_stfd(lfd);
}

srs_error_t SrsTcpListener::cycle()
{
    srs_error_t err = srs_success;

    while (true)
    {
        // Checks whether an error occurred.
        if ((err = trd->pull()) != srs_success)
        {
            return srs_error_wrap(err, "tcp listener");
        }

        if ((err = do_cycle()) != srs_success)
        {
            srs_warn("%s listener: Ignore error, %s", label_.c_str(), srs_error_desc(err).c_str());
            srs_freep(err);
        }
    }

    return err;
}

srs_error_t SrsTcpListener::do_cycle()
{
    srs_error_t err = srs_success;

    // 접속이 올 때까지 타임아웃 부로 대기 — 타임아웃이면 조용히 리턴해서
    // cycle이 pull()로 인터럽트만 확인하고 다시 들어온다.
    pollfd pfd;
    pfd.fd = lfd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int r0 = ::poll(&pfd, 1, SRS_TCP_ACCEPT_TIMEOUT_MS);
    if (r0 == 0)
    {
        return err;
    }
    if (r0 < 0)
    {
        if (errno == EINTR)
        {
            return err;
        }
        return srs_error_new(ERROR_SOCKET_ACCEPT, "poll at fd=%d", srs_netfd_fileno(lfd));
    }

    srs_netfd_t fd = ::accept(lfd, NULL, NULL);
    if (fd == SRS_NETFD_INVALID)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            return err;
        }
        return srs_error_new(ERROR_SOCKET_ACCEPT, "accept at fd=%d", srs_netfd_fileno(lfd));
    }

    if ((err = srs_fd_closeexec(fd)) != srs_success)
    {
        srs_close_stfd(fd);
        return srs_error_wrap(err, "set closeexec");
    }

    if ((err = handler->on_tcp_client(this, fd)) != srs_success)
    {
        return srs_error_wrap(err, "handle fd=%d", srs_netfd_fileno(fd));
    }

    return err;
}
