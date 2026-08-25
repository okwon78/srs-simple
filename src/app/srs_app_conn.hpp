// srs_simple — 원본: trunk/src/app/srs_app_conn.hpp (SrsResourceManager)
//            + protocol/srs_protocol_conn.hpp (ISrsResource/ISrsResourceManager/ISrsConnection 병합)
// 연결의 비동기 해제(reap)를 담당한다. 핵심 규칙: 연결은 자기 코루틴에서 delete this를
// 하지 않고, 종료 시 manager->remove(this)로 등록하면 매니저 스레드가 해제한다 (CLAUDE.md §5.3).
// 원본의 id/fast-id/name 인덱스 맵, disposing 핸들러 구독은 라이브 경로에 불필요해 제거 — CLAUDE.md §5.6.
#ifndef SRS_APP_CONN_HPP
#define SRS_APP_CONN_HPP

#include <srs_core.hpp>

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <srs_app_st.hpp>

// The resource managed by ISrsResourceManager.
// (원본: protocol/srs_protocol_conn.hpp — 프로토콜 계층 연결 파일을 따로 만들지 않아 여기로 병합)
class ISrsResource
{
public:
    ISrsResource();
    virtual ~ISrsResource();
public:
    // Get the context id of the connection.
    virtual const SrsContextId& get_id() = 0;
public:
    // The resource description, optional.
    virtual std::string desc();
};

// The manager for resources.
class ISrsResourceManager
{
public:
    ISrsResourceManager();
    virtual ~ISrsResourceManager();
public:
    // Remove then free the specified connection. Note that the manager always frees the resource c,
    // in the same coroutine or another coroutine.
    virtual void remove(ISrsResource* c) = 0;
};

// The connection interface for all HTTP/RTMP/RTSP objects.
class ISrsConnection : public ISrsResource
{
public:
    ISrsConnection();
    virtual ~ISrsConnection();
public:
    // Get the remote ip address.
    virtual std::string remote_ip() = 0;
};

// The resource manager removes a resource and deletes it asynchronously.
// 원본은 ST cond로 좀비를 넘기지만, pthread에서는 mutex + condition_variable을 쓴다.
// interrupt는 블록된 cond wait를 모르는 SrsSTCoroutine::stop()에서 오므로,
// 리스너의 poll(100ms)과 같은 이디엄으로 타임아웃 부 대기를 한다 (CLAUDE.md §5.1).
class SrsResourceManager : public ISrsCoroutineHandler, public ISrsResourceManager
{
private:
    std::string label_;
    SrsCoroutine* trd;
private:
    std::mutex lock_;
    std::condition_variable cond;
    // Whether the manager itself is destroying, so remove() should not queue zombies.
    bool disposing_;
    // The connections owned by this manager.
    std::vector<ISrsResource*> conns_;
    // The zombie connections, we will delete them asynchronously.
    std::vector<ISrsResource*> zombies_;
public:
    SrsResourceManager(const std::string& label);
    virtual ~SrsResourceManager();
public:
    srs_error_t start();
    bool empty();
    size_t size();
// Interface ISrsCoroutineHandler
public:
    virtual srs_error_t cycle();
public:
    void add(ISrsResource* conn);
// Interface ISrsResourceManager
public:
    // A callback for a connection to remove itself, at the end of its cycle().
    // 자기 스레드에서 delete this 금지 — 여기 등록만 하고, 매니저 스레드가 해제한다.
    virtual void remove(ISrsResource* c);
private:
    void clear();
};

#endif
