// srs_simple — 원본: trunk/src/kernel/srs_kernel_log.cpp + protocol/srs_protocol_log.cpp 병합
#include <srs_kernel_log.hpp>

#include <srs_kernel_error.hpp>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
using namespace std;

// Indexed by the level value (bit flag), same as the original.
const char* srs_log_level_strings[] = {
    "Forb", "Verb", "Debug", NULL, "Trace", NULL, NULL, NULL,
    "Warn", NULL,   NULL,    NULL, NULL,    NULL, NULL, NULL,
    "Error",
};

ISrsLog::ISrsLog()
{
}

ISrsLog::~ISrsLog()
{
}

ISrsContext::ISrsContext()
{
}

ISrsContext::~ISrsContext()
{
}

SrsThreadContext::SrsThreadContext()
{
}

SrsThreadContext::~SrsThreadContext()
{
}

// The cid of current thread. 원본의 map<srs_thread_t, SrsContextId> cache에 대응.
static thread_local SrsContextId _srs_thread_cid = "main";

SrsContextId SrsThreadContext::generate_id()
{
    // 원본은 8자 랜덤 문자열(srs_random_str). 교육용으로는 추적이 쉬운 증가 번호를 쓴다.
    static int seq = 0;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&lock);
    int v = ++seq;
    pthread_mutex_unlock(&lock);

    char buf[32];
    snprintf(buf, sizeof(buf), "cid-%d", v);
    return SrsContextId(buf);
}

const SrsContextId& SrsThreadContext::get_id()
{
    return _srs_thread_cid;
}

const SrsContextId& SrsThreadContext::set_id(const SrsContextId& v)
{
    _srs_thread_cid = v;
    return _srs_thread_cid;
}

SrsConsoleLog::SrsConsoleLog(SrsLogLevel l, bool u)
{
    level_ = l;
    utc = u;
}

SrsConsoleLog::~SrsConsoleLog()
{
}

srs_error_t SrsConsoleLog::initialize()
{
    return srs_success;
}

// Generate the log header.
// @param dangerous Whether log is warning or error, log the errno if true.
bool srs_log_header(char* buffer, int size, bool utc, bool dangerous, const char* tag, SrsContextId cid, const char* level, int* psize)
{
    // clock time
    timeval tv;
    if (gettimeofday(&tv, NULL) == -1) {
        return false;
    }

    // to calendar time
    struct tm now;
    if (utc) {
        if (gmtime_r(&tv.tv_sec, &now) == NULL) {
            return false;
        }
    } else {
        if (localtime_r(&tv.tv_sec, &now) == NULL) {
            return false;
        }
    }

    int written = -1;
    if (dangerous) {
        written = snprintf(buffer, size,
            "[%d-%02d-%02d %02d:%02d:%02d.%03d][%s][%d][%s][%d] ",
            1900 + now.tm_year, 1 + now.tm_mon, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec, (int)(tv.tv_usec / 1000),
            level, getpid(), cid.c_str(), errno);
    } else {
        written = snprintf(buffer, size,
            "[%d-%02d-%02d %02d:%02d:%02d.%03d][%s][%d][%s] ",
            1900 + now.tm_year, 1 + now.tm_mon, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec, (int)(tv.tv_usec / 1000),
            level, getpid(), cid.c_str());
    }

    // Exceed the size, ignore this log.
    if (written <= 0 || written >= size) {
        return false;
    }

    *psize = written;
    return true;
}

void SrsConsoleLog::log(SrsLogLevel level, const char* tag, const SrsContextId& context_id, const char* fmt, va_list args)
{
    if (level < level_ || level >= SrsLogLevelDisabled) {
        return;
    }

    // 원본은 멤버 버퍼를 재사용 — ST 단일 스레드라 안전. pthread 모델에서는
    // 스택 버퍼로 스레드마다 독립시킨다 (CLAUDE.md §5.6).
    char buffer[SRS_BASIC_LOG_SIZE];

    int size = 0;
    if (!srs_log_header(buffer, SRS_BASIC_LOG_SIZE, utc, level >= SrsLogLevelWarn, tag, context_id, srs_log_level_strings[level], &size)) {
        return;
    }

    // Something not expected, drop the log.
    int r0 = vsnprintf(buffer + size, SRS_BASIC_LOG_SIZE - size, fmt, args);
    if (r0 <= 0 || r0 >= SRS_BASIC_LOG_SIZE - size) {
        return;
    }
    size += r0;

    // Add errno and strerror() if error.
    if (level == SrsLogLevelError && errno != 0) {
        r0 = snprintf(buffer + size, SRS_BASIC_LOG_SIZE - size, "(%s)", strerror(errno));
        if (r0 <= 0 || r0 >= SRS_BASIC_LOG_SIZE - size) {
            return;
        }
        size += r0;
    }

    if (level >= SrsLogLevelWarn) {
        fprintf(stderr, "%s\n", buffer);
    } else {
        fprintf(stdout, "%s\n", buffer);
        // 파일로 리다이렉트하면 stdout이 전량 버퍼링된다 — 라인마다 밀어낸다
        // (원본은 tty 전제 혹은 자체 파일 로거).
        fflush(stdout);
    }
}

// @global 원본은 srs_main_server.cpp에서 정의. 여기 두면 utest도 공유한다.
ISrsContext* _srs_context = new SrsThreadContext();
ISrsLog* _srs_log = new SrsConsoleLog(SrsLogLevelTrace, false);

void srs_logger_impl(SrsLogLevel level, const char* tag, const SrsContextId& context_id, const char* fmt, ...)
{
    if (!_srs_log) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    _srs_log->log(level, tag, context_id, fmt, args);
    va_end(args);
}
