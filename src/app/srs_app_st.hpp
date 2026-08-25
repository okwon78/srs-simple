// srs_simple — 원본: trunk/src/app/srs_app_st.hpp + protocol/srs_protocol_st.hpp 병합
// ST 코루틴 라이브러리를 pthread(1 connection = 1 thread)로 대체한다 (CLAUDE.md §5.1).
// 인터페이스(ISrsCoroutineHandler::cycle, SrsCoroutine::start/stop/interrupt/pull)는
// 원본 그대로이므로, 원본을 읽을 때 ST 호출만 치환해서 읽으면 된다.
#ifndef SRS_APP_ST_HPP
#define SRS_APP_ST_HPP

#include <srs_core.hpp>

#include <pthread.h>

#include <mutex>
#include <string>

#include <srs_kernel_error.hpp>
#include <srs_protocol_io.hpp>

// The network fd. 원본은 ST의 netfd 래퍼 포인터(srs_netfd_t)지만,
// pthread 모델에서는 OS fd 그대로다. 이름은 원본을 유지한다.
typedef int srs_netfd_t;
#define SRS_NETFD_INVALID (-1)

// Get the underlying os fd.
extern int srs_netfd_fileno(srs_netfd_t fd);
// Close the netfd and set to invalid.
extern void srs_close_stfd(srs_netfd_t& fd);

// Each coroutine must implement this interface,
// to do the cycle job and handle some events.
//
// A thread does a job then terminates normally, it's a SrsOneCycleThread:
//      class SrsOneCycleThread : public ISrsCoroutineHandler {
//          public: SrsCoroutine trd;
//          public: virtual srs_error_t cycle() {
//              // Do something, then return from this cycle and the thread terminates normally.
//          }
//      };
//
// A thread has its own inner loop, such as the RTMP receive thread:
//      class SrsReceiveThread : public ISrsCoroutineHandler {
//          public: SrsCoroutine* trd;
//          public: virtual srs_error_t cycle() {
//              while (true) {
//                  // Check whether the thread is interrupted.
//                  if ((err = trd->pull()) != srs_success) {
//                      return err;
//                  }
//                  // Do something, such as read() packets.
//              }
//          }
//      };
class ISrsCoroutineHandler
{
public:
    ISrsCoroutineHandler();
    virtual ~ISrsCoroutineHandler();
public:
    // Do the work. The coroutine will terminate normally if it returns.
    // @remark If the cycle has its own loop, it must check the thread pull.
    virtual srs_error_t cycle() = 0;
};

// Start the object, generally a coroutine.
class ISrsStartable
{
public:
    ISrsStartable();
    virtual ~ISrsStartable();
public:
    virtual srs_error_t start() = 0;
};

// Allow user to interrupt the coroutine, for example, to stop it.
class ISrsInterruptable
{
public:
    ISrsInterruptable();
    virtual ~ISrsInterruptable();
public:
    virtual void interrupt() = 0;
    virtual srs_error_t pull() = 0;
};

// The coroutine object.
class SrsCoroutine : public ISrsStartable, public ISrsInterruptable
{
public:
    SrsCoroutine();
    virtual ~SrsCoroutine();
public:
    virtual void stop() = 0;
public:
    // Get and set the context id of coroutine.
    virtual const SrsContextId& cid() = 0;
    virtual void set_cid(const SrsContextId& cid) = 0;
};

// An empty coroutine, the user can default to this object before creating any real coroutine.
// @see https://github.com/ossrs/srs/pull/908
class SrsDummyCoroutine : public SrsCoroutine
{
private:
    SrsContextId cid_;
public:
    SrsDummyCoroutine();
    virtual ~SrsDummyCoroutine();
public:
    virtual srs_error_t start();
    virtual void stop();
    virtual void interrupt();
    virtual srs_error_t pull();
    virtual const SrsContextId& cid();
    virtual void set_cid(const SrsContextId& cid);
};

// The pthread-backed coroutine. 원본 이름(SrsSTCoroutine)을 유지한다 —
// 원본은 SrsFastCoroutine에 위임하는 래퍼지만(성능 분리), 여기서는 직접 구현한다.
// @remark We always create a joinable thread, so we must join it (in stop) or leak memory.
class SrsSTCoroutine : public SrsCoroutine
{
private:
    std::string name;
    ISrsCoroutineHandler* handler;
private:
    pthread_t trd;
    SrsContextId cid_;
    // The error to return by pull(). Set by interrupt/stop, or by the worker when cycle done.
    srs_error_t trd_err;
private:
    bool started;
    bool interrupted;
    bool disposed;
    // Cycle done, no need to interrupt it.
    bool cycle_done;
private:
    // 원본 ST는 단일 스레드라 락이 없지만, pthread에서는 상태(trd_err 등)를
    // 워커/외부 스레드가 함께 만지므로 뮤텍스가 필요하다 (CLAUDE.md §5.6).
    std::mutex lock_;
public:
    // Create a thread with name n and handler h.
    // @remark User can specify a cid for thread to use, or we will allocate a new one.
    SrsSTCoroutine(std::string n, ISrsCoroutineHandler* h);
    SrsSTCoroutine(std::string n, ISrsCoroutineHandler* h, SrsContextId cid);
    virtual ~SrsSTCoroutine();
public:
    // Start the thread.
    // @remark Should never start it when stopped or terminated.
    virtual srs_error_t start();
    // Interrupt the thread then wait(join) until it terminates.
    virtual void stop();
    // Interrupt the thread and notify it to terminate, then pull() returns error.
    // ST는 블록된 IO를 즉시 깨우지만, pthread에서는 소켓 타임아웃(SO_RCVTIMEO)으로
    // 주기적으로 깨어난 워커가 pull()을 확인해 종료한다 (CLAUDE.md §5.1).
    virtual void interrupt();
    // Check whether the thread is terminated normally or with error(stopped or terminated with error),
    // and the thread should be running if it returns ERROR_SUCCESS.
    // @remark Return the specified error when the thread terminated normally with an error.
    // @remark Return ERROR_THREAD_TERMINATED when the thread terminated normally without error.
    // @remark Return ERROR_THREAD_INTERRUPED when the thread is interrupted.
    virtual srs_error_t pull();
    // Get and set the context id of thread.
    virtual const SrsContextId& cid();
    virtual void set_cid(const SrsContextId& cid);
private:
    srs_error_t cycle();
    static void* pfn(void* arg);
};

// The real-socket implementation of ISrsProtocolReadWriter.
// 원본은 protocol/srs_protocol_st.hpp의 ST 소켓이지만, 여기서는 블로킹 BSD 소켓 +
// SO_RCVTIMEO/SO_SNDTIMEO 타임아웃으로 구현한다. 타임아웃/EINTR은 ERROR_SOCKET_TIMEOUT.
class SrsStSocket : public ISrsProtocolReadWriter
{
private:
    // The recv/send timeout in srs_utime_t.
    // @remark Use SRS_UTIME_NO_TIMEOUT for never timeout.
    srs_utime_t rtm;
    srs_utime_t stm;
    // The recv/send data in bytes
    int64_t rbytes;
    int64_t sbytes;
    // The underlying fd.
    srs_netfd_t stfd_;
public:
    SrsStSocket();
    SrsStSocket(srs_netfd_t fd);
    virtual ~SrsStSocket();
private:
    void init(srs_netfd_t fd);
public:
    virtual void set_recv_timeout(srs_utime_t tm);
    virtual srs_utime_t get_recv_timeout();
    virtual void set_send_timeout(srs_utime_t tm);
    virtual srs_utime_t get_send_timeout();
    virtual int64_t get_recv_bytes();
    virtual int64_t get_send_bytes();
public:
    // @param nread, the actual read bytes, ignore if NULL.
    virtual srs_error_t read(void* buf, size_t size, ssize_t* nread);
    virtual srs_error_t read_fully(void* buf, size_t size, ssize_t* nread);
    // @param nwrite, the actual write bytes, ignore if NULL.
    virtual srs_error_t write(void* buf, size_t size, ssize_t* nwrite);
    virtual srs_error_t writev(const iovec* iov, int iov_size, ssize_t* nwrite);
};

#endif
