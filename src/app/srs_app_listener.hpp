// srs_simple — 원본: trunk/src/app/srs_app_listener.hpp (TCP만) + protocol/srs_protocol_st.cpp의 srs_tcp_listen
#ifndef SRS_APP_LISTENER_HPP
#define SRS_APP_LISTENER_HPP

#include <srs_core.hpp>

#include <string>

#include <srs_app_st.hpp>
#include <srs_kernel_error.hpp>

// Bind and listen the tcp port. IPv4 전용으로 단순화 (원본은 getaddrinfo).
extern srs_error_t srs_tcp_listen(std::string ip, int port, srs_netfd_t* pfd);
// Set the close-on-exec flag for fd.
extern srs_error_t srs_fd_closeexec(int fd);
// Set the SO_REUSEADDR for fd.
extern srs_error_t srs_fd_reuseaddr(int fd);

// All listener should support listen method.
class ISrsListener
{
public:
    ISrsListener();
    virtual ~ISrsListener();
public:
    virtual srs_error_t listen() = 0;
};

// The tcp connection handler.
class ISrsTcpHandler
{
public:
    ISrsTcpHandler();
    virtual ~ISrsTcpHandler();
public:
    // When got tcp client.
    virtual srs_error_t on_tcp_client(ISrsListener* listener, srs_netfd_t stfd) = 0;
};

// Bind and listen tcp port, use handler to process the client.
class SrsTcpListener : public ISrsCoroutineHandler, public ISrsListener
{
private:
    std::string label_;
    srs_netfd_t lfd;
    SrsCoroutine* trd;
private:
    ISrsTcpHandler* handler;
    std::string ip;
    int port_;
public:
    SrsTcpListener(ISrsTcpHandler* h);
    virtual ~SrsTcpListener();
public:
    SrsTcpListener* set_label(const std::string& label);
    SrsTcpListener* set_endpoint(const std::string& i, int p);
    int port();
public:
    virtual srs_error_t listen();
    void close();
// Interface ISrsCoroutineHandler
public:
    virtual srs_error_t cycle();
private:
    srs_error_t do_cycle();
};

#endif
