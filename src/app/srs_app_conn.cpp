// srs_simple — 원본: trunk/src/app/srs_app_conn.cpp (SrsResourceManager::cycle/remove/clear)
//            + protocol/srs_protocol_conn.cpp (인터페이스 기본 구현)
#include <srs_app_conn.hpp>

#include <algorithm>
#include <chrono>

#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

using namespace std;

ISrsResource::ISrsResource()
{
}

ISrsResource::~ISrsResource()
{
}

std::string ISrsResource::desc()
{
    return "Resource";
}

ISrsResourceManager::ISrsResourceManager()
{
}

ISrsResourceManager::~ISrsResourceManager()
{
}

ISrsConnection::ISrsConnection()
{
}

ISrsConnection::~ISrsConnection()
{
}

SrsResourceManager::SrsResourceManager(const std::string& label)
{
    label_ = label;
    trd = new SrsDummyCoroutine();
    disposing_ = false;
}

SrsResourceManager::~SrsResourceManager()
{
    // 매니저 스레드부터 세운다 — stop()은 interrupt 후 join.
    // 워커는 타임아웃 부 cond wait로 100ms 안에 깨어나 pull()에서 종료를 발견한다.
    trd->stop();
    srs_freep(trd);

    // 이미 등록된 좀비를 해제한다.
    clear();

    // 아직 살아있는 연결을 해제한다. delete는 연결의 스레드를 interrupt+join하고
    // (SrsRtmpConn 소멸자), 그 과정에서 워커가 부르는 remove()는 disposing_로 무시된다.
    vector<ISrsResource*> copy;
    {
        lock_guard<mutex> guard(lock_);
        disposing_ = true;
        copy = conns_;
    }
    for (int i = 0; i < (int)copy.size(); i++) {
        ISrsResource* conn = copy.at(i);
        srs_freep(conn);
    }

    // disposing_ 설정 직전에 remove()로 들어온 좀비가 있으면 마저 해제한다.
    clear();
}

srs_error_t SrsResourceManager::start()
{
    srs_error_t err = srs_success;

    srs_freep(trd);
    trd = new SrsSTCoroutine("manager", this);

    if ((err = trd->start()) != srs_success) {
        return srs_error_wrap(err, "conn manager");
    }

    return err;
}

bool SrsResourceManager::empty()
{
    lock_guard<mutex> guard(lock_);
    return conns_.empty();
}

size_t SrsResourceManager::size()
{
    lock_guard<mutex> guard(lock_);
    return conns_.size();
}

srs_error_t SrsResourceManager::cycle()
{
    srs_error_t err = srs_success;

    srs_trace("%s: connection manager run", label_.c_str());

    while (true) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "conn manager");
        }

        {
            unique_lock<mutex> lk(lock_);
            if (zombies_.empty()) {
                cond.wait_for(lk, chrono::milliseconds(100));
            }
        }

        clear();
    }

    return err;
}

void SrsResourceManager::add(ISrsResource* conn)
{
    lock_guard<mutex> guard(lock_);
    if (std::find(conns_.begin(), conns_.end(), conn) == conns_.end()) {
        conns_.push_back(conn);
    }
}

void SrsResourceManager::remove(ISrsResource* c)
{
    lock_guard<mutex> guard(lock_);

    vector<ISrsResource*>::iterator it = std::find(conns_.begin(), conns_.end(), c);
    if (it != conns_.end()) {
        conns_.erase(it);
    }

    // 소멸 중이면 소멸자가 직접 해제하므로 좀비로 넘기지 않는다.
    if (disposing_) {
        return;
    }

    // 이중 등록(원본의 in_zombie 검사)을 막는다.
    if (std::find(zombies_.begin(), zombies_.end(), c) != zombies_.end()) {
        return;
    }

    // Push to zombies, we will free it in another coroutine.
    zombies_.push_back(c);
    cond.notify_all();
}

void SrsResourceManager::clear()
{
    // 락 밖에서 해제한다 — delete가 연결 스레드 join으로 이어지고,
    // 그 스레드가 remove()에서 같은 락을 잡으려 할 수 있다.
    vector<ISrsResource*> copy;
    {
        lock_guard<mutex> guard(lock_);
        copy.swap(zombies_);
    }

    for (int i = 0; i < (int)copy.size(); i++) {
        ISrsResource* conn = copy.at(i);
        srs_trace("%s: dispose resource(%s), conns=%d", label_.c_str(), conn->desc().c_str(), (int)size());
        srs_freep(conn);
    }
}
