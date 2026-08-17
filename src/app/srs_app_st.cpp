// srs_simple — 원본: trunk/src/app/srs_app_st.cpp + protocol/srs_protocol_st.cpp 병합
// ST 코루틴/ST 소켓을 pthread + 블로킹 BSD 소켓으로 대체 (CLAUDE.md §5.1).
#include <srs_app_st.hpp>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <vector>

#include <srs_kernel_log.hpp>

using namespace std;

int srs_netfd_fileno(srs_netfd_t fd)
{
    return fd;
}

void srs_close_stfd(srs_netfd_t& fd)
{
    if (fd != SRS_NETFD_INVALID) {
        ::close(fd);
        fd = SRS_NETFD_INVALID;
    }
}

ISrsCoroutineHandler::ISrsCoroutineHandler()
{
}

ISrsCoroutineHandler::~ISrsCoroutineHandler()
{
}

ISrsStartable::ISrsStartable()
{
}

ISrsStartable::~ISrsStartable()
{
}

ISrsInterruptable::ISrsInterruptable()
{
}

ISrsInterruptable::~ISrsInterruptable()
{
}

SrsCoroutine::SrsCoroutine()
{
}

SrsCoroutine::~SrsCoroutine()
{
}

SrsDummyCoroutine::SrsDummyCoroutine()
{
}

SrsDummyCoroutine::~SrsDummyCoroutine()
{
}

srs_error_t SrsDummyCoroutine::start()
{
    return srs_error_new(ERROR_THREAD_DUMMY, "dummy coroutine");
}

void SrsDummyCoroutine::stop()
{
}

void SrsDummyCoroutine::interrupt()
{
}

srs_error_t SrsDummyCoroutine::pull()
{
    return srs_error_new(ERROR_THREAD_DUMMY, "dummy pull");
}

const SrsContextId& SrsDummyCoroutine::cid()
{
    return cid_;
}

void SrsDummyCoroutine::set_cid(const SrsContextId& cid)
{
    cid_ = cid;
}

SrsSTCoroutine::SrsSTCoroutine(string n, ISrsCoroutineHandler* h)
{
    name = n;
    handler = h;
    trd_err = srs_success;
    started = interrupted = disposed = cycle_done = false;
}

SrsSTCoroutine::SrsSTCoroutine(string n, ISrsCoroutineHandler* h, SrsContextId cid)
{
    name = n;
    handler = h;
    cid_ = cid;
    trd_err = srs_success;
    started = interrupted = disposed = cycle_done = false;
}

SrsSTCoroutine::~SrsSTCoroutine()
{
    stop();

    srs_freep(trd_err);
}

srs_error_t SrsSTCoroutine::start()
{
    srs_error_t err = srs_success;

    std::lock_guard<std::mutex> guard(lock_);

    if (started || disposed) {
        if (disposed) {
            err = srs_error_new(ERROR_THREAD_DISPOSED, "disposed");
        } else {
            err = srs_error_new(ERROR_THREAD_STARTED, "started");
        }

        if (trd_err == srs_success) {
            trd_err = srs_error_copy(err);
        }

        return err;
    }

    int r0 = pthread_create(&trd, NULL, pfn, this);
    if (r0 != 0) {
        err = srs_error_new(ERROR_THREAD_CREATE, "create failed, r0=%d", r0);

        srs_freep(trd_err);
        trd_err = srs_error_copy(err);

        return err;
    }

    started = true;

    return err;
}

void SrsSTCoroutine::stop()
{
    bool need_join = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (disposed) {
            return;
        }
        disposed = true;
        need_join = started;
    }

    interrupt();

    // We always create joinable thread, so we must join it or memory leak.
    // 워커는 소켓 타임아웃으로 깨어나 pull()에서 인터럽트를 발견하고 곧 종료한다.
    if (need_join) {
        pthread_join(trd, NULL);
    }

    // If there's no error occur from worker, try to set to terminated error.
    std::lock_guard<std::mutex> guard(lock_);
    if (trd_err == srs_success && !cycle_done) {
        trd_err = srs_error_new(ERROR_THREAD_TERMINATED, "terminated");
    }
}

void SrsSTCoroutine::interrupt()
{
    std::lock_guard<std::mutex> guard(lock_);

    if (!started || interrupted || cycle_done) {
        return;
    }
    interrupted = true;

    if (trd_err == srs_success) {
        trd_err = srs_error_new(ERROR_THREAD_INTERRUPED, "interrupted");
    }
}

srs_error_t SrsSTCoroutine::pull()
{
    std::lock_guard<std::mutex> guard(lock_);

    if (trd_err == srs_success) {
        return srs_success;
    }
    return srs_error_copy(trd_err);
}

const SrsContextId& SrsSTCoroutine::cid()
{
    return cid_;
}

void SrsSTCoroutine::set_cid(const SrsContextId& cid)
{
    cid_ = cid;
}

srs_error_t SrsSTCoroutine::cycle()
{
    if (_srs_context) {
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (cid_.empty()) {
                cid_ = _srs_context->generate_id();
            }
        }
        _srs_context->set_id(cid_);
    }

    srs_error_t err = handler->cycle();
    if (err != srs_success) {
        return srs_error_wrap(err, "coroutine cycle");
    }

    // Set cycle done, no need to interrupt it.
    std::lock_guard<std::mutex> guard(lock_);
    cycle_done = true;

    return err;
}

void* SrsSTCoroutine::pfn(void* arg)
{
    SrsSTCoroutine* p = (SrsSTCoroutine*)arg;

    srs_error_t err = p->cycle();

    // Set the err for function pull to fetch it.
    std::lock_guard<std::mutex> guard(p->lock_);
    if (p->trd_err == srs_success) {
        p->trd_err = err;
    } else {
        // interrupt가 이미 에러를 설정했으면 그쪽을 유지한다.
        srs_freep(err);
    }

    return NULL;
}

SrsStSocket::SrsStSocket()
{
    init(SRS_NETFD_INVALID);
}

SrsStSocket::SrsStSocket(srs_netfd_t fd)
{
    init(fd);
}

SrsStSocket::~SrsStSocket()
{
    // @remark The fd is not owned by socket, user should close it.
}

void SrsStSocket::init(srs_netfd_t fd)
{
    stfd_ = fd;
    rtm = stm = SRS_UTIME_NO_TIMEOUT;
    rbytes = sbytes = 0;
}

void SrsStSocket::set_recv_timeout(srs_utime_t tm)
{
    rtm = tm;

    // 원본 ST는 read 호출마다 타임아웃 인자를 넘기지만, 여기서는 소켓 옵션으로 건다.
    timeval tv = {0, 0};
    if (tm != SRS_UTIME_NO_TIMEOUT) {
        tv.tv_sec = tm / SRS_UTIME_SECONDS;
        tv.tv_usec = tm % SRS_UTIME_SECONDS;
    }
    if (stfd_ != SRS_NETFD_INVALID) {
        ::setsockopt(stfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
}

srs_utime_t SrsStSocket::get_recv_timeout()
{
    return rtm;
}

void SrsStSocket::set_send_timeout(srs_utime_t tm)
{
    stm = tm;

    timeval tv = {0, 0};
    if (tm != SRS_UTIME_NO_TIMEOUT) {
        tv.tv_sec = tm / SRS_UTIME_SECONDS;
        tv.tv_usec = tm % SRS_UTIME_SECONDS;
    }
    if (stfd_ != SRS_NETFD_INVALID) {
        ::setsockopt(stfd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
}

srs_utime_t SrsStSocket::get_send_timeout()
{
    return stm;
}

int64_t SrsStSocket::get_recv_bytes()
{
    return rbytes;
}

int64_t SrsStSocket::get_send_bytes()
{
    return sbytes;
}

srs_error_t SrsStSocket::read(void* buf, size_t size, ssize_t* nread)
{
    srs_error_t err = srs_success;

    srs_assert(stfd_ != SRS_NETFD_INVALID);

    ssize_t nb_read = ::recv(stfd_, buf, size, 0);

    if (nread) {
        *nread = nb_read;
    }

    // On success a non-negative integer indicating the number of bytes actually read is returned
    // (a value of 0 means the network connection is closed or end of file is reached).
    if (nb_read <= 0) {
        // 타임아웃(EAGAIN)과 시그널(EINTR)은 ERROR_SOCKET_TIMEOUT — 호출자가 pull()을
        // 확인하고 재시도할 수 있는, 연결이 살아있는 에러다.
        if (nb_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return srs_error_new(ERROR_SOCKET_TIMEOUT, "timeout %d ms", srsu2msi(rtm));
        }

        if (nb_read == 0) {
            errno = ECONNRESET;
        }

        return srs_error_new(ERROR_SOCKET_READ, "read");
    }

    rbytes += nb_read;

    return err;
}

srs_error_t SrsStSocket::read_fully(void* buf, size_t size, ssize_t* nread)
{
    srs_error_t err = srs_success;

    srs_assert(stfd_ != SRS_NETFD_INVALID);

    // 원본 st_read_fully처럼 size를 다 채울 때까지 루프.
    size_t nb = 0;
    while (nb < size) {
        ssize_t n = ::recv(stfd_, (char*)buf + nb, size - nb, 0);

        if (n <= 0) {
            if (nread) {
                *nread = nb;
            }

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                return srs_error_new(ERROR_SOCKET_TIMEOUT, "timeout %d ms", srsu2msi(rtm));
            }

            if (n == 0) {
                errno = ECONNRESET;
            }

            return srs_error_new(ERROR_SOCKET_READ_FULLY, "read fully, size=%d, nn=%d", (int)size, (int)nb);
        }

        nb += n;
    }

    if (nread) {
        *nread = nb;
    }
    rbytes += nb;

    return err;
}

srs_error_t SrsStSocket::write(void* buf, size_t size, ssize_t* nwrite)
{
    srs_error_t err = srs_success;

    srs_assert(stfd_ != SRS_NETFD_INVALID);

    // 원본 st_write처럼 size를 다 쓸 때까지 루프 (블로킹 소켓도 부분 쓰기가 가능하다).
    size_t nb = 0;
    while (nb < size) {
        ssize_t n = ::send(stfd_, (char*)buf + nb, size - nb, 0);

        if (n <= 0) {
            if (nwrite) {
                *nwrite = nb;
            }

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                return srs_error_new(ERROR_SOCKET_TIMEOUT, "write timeout %d ms", srsu2msi(stm));
            }

            return srs_error_new(ERROR_SOCKET_WRITE, "write");
        }

        nb += n;
    }

    if (nwrite) {
        *nwrite = nb;
    }
    sbytes += nb;

    return err;
}

srs_error_t SrsStSocket::writev(const iovec* iov, int iov_size, ssize_t* nwrite)
{
    srs_error_t err = srs_success;

    srs_assert(stfd_ != SRS_NETFD_INVALID);

    // 부분 쓰기를 처리하기 위해 iovec 사본을 전진시키며 전부 쓴다 (원본 st_writev와 동일 의미).
    vector<iovec> iovs(iov, iov + iov_size);
    size_t cur = 0;
    ssize_t total = 0;

    while (cur < iovs.size()) {
        ssize_t n = ::writev(stfd_, &iovs[cur], (int)(iovs.size() - cur));

        if (n <= 0) {
            if (nwrite) {
                *nwrite = total;
            }

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                return srs_error_new(ERROR_SOCKET_TIMEOUT, "writev timeout %d ms", srsu2msi(stm));
            }

            return srs_error_new(ERROR_SOCKET_WRITE, "writev");
        }

        total += n;

        // 쓰인 바이트만큼 iovec을 소비한다.
        while (cur < iovs.size() && n >= (ssize_t)iovs[cur].iov_len) {
            n -= iovs[cur].iov_len;
            cur++;
        }
        if (cur < iovs.size() && n > 0) {
            iovs[cur].iov_base = (char*)iovs[cur].iov_base + n;
            iovs[cur].iov_len -= n;
        }
    }

    if (nwrite) {
        *nwrite = total;
    }
    sbytes += total;

    return err;
}
