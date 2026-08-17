// srs_simple — 원본: trunk/src/core/srs_core.hpp (+ srs_core_time.hpp 병합)
#ifndef SRS_CORE_HPP
#define SRS_CORE_HPP

#include <stdint.h>
#include <string>

// The project informations, sent to client in RTMP connect response.
#define RTMP_SIG_SRS_KEY "SRS_SIMPLE"
#define RTMP_SIG_SRS_VERSION "0.1.0"
#define RTMP_SIG_SRS_SERVER RTMP_SIG_SRS_KEY "/" RTMP_SIG_SRS_VERSION

// To free the p and set to NULL.
// @remark The p must be a pointer T*.
#define srs_freep(p) \
    delete p; \
    p = NULL; \
    (void)0
// Please use the freepa(T[]) to free an array, otherwise the behavior is undefined.
#define srs_freepa(pa) \
    delete[] pa; \
    pa = NULL; \
    (void)0

// Error predefined for all modules.
class SrsCplxError;
typedef SrsCplxError* srs_error_t;

// The context ID, to identify the logic unit (a connection) in logs.
// 원본은 _SrsContextId 래퍼 클래스지만, 원본 주석대로 string을 직접 사용한다.
typedef std::string SrsContextId;

// The time unit for timeout, interval or duration, in us.
typedef int64_t srs_utime_t;

// The time unit in ms, for example 100 * SRS_UTIME_MILLISECONDS means 100ms.
#define SRS_UTIME_MILLISECONDS 1000
// The time unit in seconds, for example 120 * SRS_UTIME_SECONDS means 120s.
#define SRS_UTIME_SECONDS 1000000LL
// The time unit in minutes, for example 3 * SRS_UTIME_MINUTES means 3m.
#define SRS_UTIME_MINUTES 60000000LL
// The time unit in hours, for example 2 * SRS_UTIME_HOURS means 2h.
#define SRS_UTIME_HOURS 3600000000LL
// Never timeout.
#define SRS_UTIME_NO_TIMEOUT ((srs_utime_t) -1LL)

// Convert srs_utime_t as ms.
#define srsu2ms(us) ((us) / SRS_UTIME_MILLISECONDS)
#define srsu2msi(us) int((us) / SRS_UTIME_MILLISECONDS)

#endif
