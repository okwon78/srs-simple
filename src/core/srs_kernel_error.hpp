// srs_simple — 원본: trunk/src/kernel/srs_kernel_error.hpp
// 원본은 X-macro(SRS_ERRNO_MAP_*)로 300여 개 코드를 생성한다.
// 여기서는 RTMP 라이브 경로에 필요한 서브셋만 평범한 enum으로 유지한다 (값은 원본과 동일).
#ifndef SRS_KERNEL_ERROR_HPP
#define SRS_KERNEL_ERROR_HPP

#include <srs_core.hpp>

#include <string>

enum SrsErrorCode
{
    ERROR_SUCCESS = 0,

    // The system error.
    ERROR_SOCKET_CREATE = 1000,
    ERROR_SOCKET_SETREUSE = 1001,
    ERROR_SOCKET_BIND = 1002,
    ERROR_SOCKET_LISTEN = 1003,
    ERROR_SOCKET_CLOSED = 1004,
    ERROR_SOCKET_READ = 1007,
    ERROR_SOCKET_READ_FULLY = 1008,
    ERROR_SOCKET_WRITE = 1009,
    ERROR_SOCKET_TIMEOUT = 1011,
    ERROR_SYSTEM_PACKET_INVALID = 1019,
    ERROR_SYSTEM_CLIENT_INVALID = 1020,
    ERROR_SYSTEM_ASSERT_FAILED = 1021,
    ERROR_READER_BUFFER_OVERFLOW = 1022,
    ERROR_SYSTEM_STREAM_BUSY = 1028,
    ERROR_THREAD_DISPOSED = 1069,
    ERROR_THREAD_INTERRUPED = 1070,
    ERROR_THREAD_TERMINATED = 1071,
    ERROR_THREAD_DUMMY = 1072,
    ERROR_THREAD_STARTED = 1078,
    ERROR_SOCKET_ACCEPT = 1081,
    ERROR_THREAD_CREATE = 1082,
    ERROR_THREAD_FINISHED = 1083,

    // The RTMP protocol error.
    ERROR_RTMP_PLAIN_REQUIRED = 2000,
    ERROR_RTMP_CHUNK_START = 2001,
    ERROR_RTMP_MSG_INVALID_SIZE = 2002,
    ERROR_RTMP_AMF0_DECODE = 2003,
    ERROR_RTMP_AMF0_INVALID = 2004,
    ERROR_RTMP_REQ_CONNECT = 2005,
    ERROR_RTMP_REQ_TCURL = 2006,
    ERROR_RTMP_MESSAGE_DECODE = 2007,
    ERROR_RTMP_MESSAGE_ENCODE = 2008,
    ERROR_RTMP_AMF0_ENCODE = 2009,
    ERROR_RTMP_CHUNK_SIZE = 2010,
    ERROR_RTMP_PACKET_SIZE = 2013,
    ERROR_RTMP_HANDSHAKE = 2016,
    ERROR_RTMP_NO_REQUEST = 2017,
    ERROR_RTMP_STREAM_NOT_FOUND = 2048,
    ERROR_RTMP_STREAM_NAME_EMPTY = 2051,
    ERROR_RTMP_MESSAGE_CREATE = 2053,
    ERROR_RTMP_CREATE_STREAM_DEPTH = 2055,
    ERROR_CONTROL_RTMP_CLOSE = 2998,
    ERROR_CONTROL_REPUBLISH = 2999,

    // For user-define error.
    ERROR_USER_DISCONNECT = 9001,
    ERROR_SOURCE_NOT_FOUND = 9002,
};

// Whether the error code is an system control error. (RTMP close/republish)
extern bool srs_is_system_control_error(srs_error_t err);
// It's closed by client.
extern bool srs_is_client_gracefully_close(srs_error_t err);

// The complex error carries code, message and callstack,
// which is more strong and easy to locate problem by log.
// please @read https://github.com/ossrs/srs/issues/913
class SrsCplxError
{
private:
    int code;
    SrsCplxError* wrapped;
    std::string msg;

    std::string func;
    std::string file;
    int line;

    SrsContextId cid;
    int rerrno;

    std::string desc;
    std::string _summary;
private:
    SrsCplxError();
public:
    virtual ~SrsCplxError();
private:
    virtual std::string description();
    virtual std::string summary();
public:
    static SrsCplxError* create(const char* func, const char* file, int line, int code, const char* fmt, ...);
    static SrsCplxError* wrap(const char* func, const char* file, int line, SrsCplxError* err, const char* fmt, ...);
    static SrsCplxError* success();
    static SrsCplxError* copy(SrsCplxError* from);
    static std::string description(SrsCplxError* err);
    static std::string summary(SrsCplxError* err);
    static int error_code(SrsCplxError* err);
    static std::string error_code_str(SrsCplxError* err);
public:
    static void srs_assert(bool expression);
};

// Error helpers, should use these functions to new or wrap an error.
#define srs_success NULL // SrsCplxError::success()
#define srs_error_new(ret, fmt, ...) SrsCplxError::create(__FUNCTION__, __FILE__, __LINE__, ret, fmt, ##__VA_ARGS__)
#define srs_error_wrap(err, fmt, ...) SrsCplxError::wrap(__FUNCTION__, __FILE__, __LINE__, err, fmt, ##__VA_ARGS__)
#define srs_error_copy(err) SrsCplxError::copy(err)
#define srs_error_desc(err) SrsCplxError::description(err)
#define srs_error_summary(err) SrsCplxError::summary(err)
#define srs_error_code(err) SrsCplxError::error_code(err)
#define srs_error_code_str(err) SrsCplxError::error_code_str(err)
#define srs_error_reset(err) srs_freep(err); err = srs_success

#ifndef srs_assert
#define srs_assert(expression) SrsCplxError::srs_assert(expression)
#endif

#endif
