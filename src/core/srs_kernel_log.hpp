// srs_simple — 원본: trunk/src/kernel/srs_kernel_log.hpp + protocol/srs_protocol_log.hpp 병합
// 콘솔 로거(SrsConsoleLog)와 스레드별 context id(SrsThreadContext)만 유지한다.
#ifndef SRS_KERNEL_LOG_HPP
#define SRS_KERNEL_LOG_HPP

#include <srs_core.hpp>

#include <stdarg.h>
#include <stdio.h>

#include <string>

// The max size of a line of log.
#define SRS_BASIC_LOG_SIZE 8192

// The log level, only levels greater than or equal to the current level will be logged.
enum SrsLogLevel
{
    SrsLogLevelForbidden = 0x00,

    // Only used for very verbose debug, generally,
    // we compile without this level for high performance.
    SrsLogLevelVerbose = 0x01,
    SrsLogLevelInfo = 0x02,
    SrsLogLevelTrace = 0x04,
    SrsLogLevelWarn = 0x08,
    SrsLogLevelError = 0x10,

    SrsLogLevelDisabled = 0x20,
};

// Get the level in string. Indexed by the level value (bit flag).
extern const char* srs_log_level_strings[];

// The log interface provides methods to write logs.
class ISrsLog
{
public:
    ISrsLog();
    virtual ~ISrsLog();
public:
    // Initialize log utilities.
    virtual srs_error_t initialize() = 0;
    // Write an application level log.
    virtual void log(SrsLogLevel level, const char* tag, const SrsContextId& context_id, const char* fmt, va_list args) = 0;
};

// The logic context, for example, an RTMP connection. We can grep the context id
// in logs to identify all logs of one connection, for debugging.
class ISrsContext
{
public:
    ISrsContext();
    virtual ~ISrsContext();
public:
    // Generate a new context id.
    // @remark We do not set it on the current thread, the user should do this.
    virtual SrsContextId generate_id() = 0;
    // Get the context id of the current thread.
    virtual const SrsContextId& get_id() = 0;
    // Set the context id of the current thread.
    virtual const SrsContextId& set_id(const SrsContextId& v) = 0;
};

// The pthread context, get_id identifies the current thread (so, the connection).
// 원본은 st 스레드 → cid 맵을 쓰지만 (SrsThreadContext in srs_protocol_log.hpp),
// pthread 모델에서는 thread_local이 정확히 같은 역할이다.
class SrsThreadContext : public ISrsContext
{
public:
    SrsThreadContext();
    virtual ~SrsThreadContext();
public:
    virtual SrsContextId generate_id();
    virtual const SrsContextId& get_id();
    virtual const SrsContextId& set_id(const SrsContextId& v);
};

// The basic console log, which writes logs to the console.
class SrsConsoleLog : public ISrsLog
{
private:
    SrsLogLevel level_;
    bool utc;
public:
    SrsConsoleLog(SrsLogLevel l, bool u);
    virtual ~SrsConsoleLog();
// Interface ISrsLog
public:
    virtual srs_error_t initialize();
    virtual void log(SrsLogLevel level, const char* tag, const SrsContextId& context_id, const char* fmt, va_list args);
};

// @global The context and log object, initialized in srs_kernel_log.cpp.
// utest나 main에서 교체 가능하다.
extern ISrsContext* _srs_context;
extern ISrsLog* _srs_log;

// Global log function implementation. Please use helper macros, for example, srs_trace or srs_error.
extern void srs_logger_impl(SrsLogLevel level, const char* tag, const SrsContextId& context_id, const char* fmt, ...);

// Log style.
#define srs_verbose(msg, ...) srs_logger_impl(SrsLogLevelVerbose, NULL, _srs_context->get_id(), msg, ##__VA_ARGS__)
#define srs_info(msg, ...) srs_logger_impl(SrsLogLevelInfo, NULL, _srs_context->get_id(), msg, ##__VA_ARGS__)
#define srs_trace(msg, ...) srs_logger_impl(SrsLogLevelTrace, NULL, _srs_context->get_id(), msg, ##__VA_ARGS__)
#define srs_warn(msg, ...) srs_logger_impl(SrsLogLevelWarn, NULL, _srs_context->get_id(), msg, ##__VA_ARGS__)
#define srs_error(msg, ...) srs_logger_impl(SrsLogLevelError, NULL, _srs_context->get_id(), msg, ##__VA_ARGS__)

#endif
