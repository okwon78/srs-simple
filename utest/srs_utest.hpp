// srs_simple — 원본: trunk/src/utest/srs_utest.hpp
#ifndef SRS_UTEST_PUBLIC_SHARED_HPP
#define SRS_UTEST_PUBLIC_SHARED_HPP

// Before defining private/protected, we must include some system header files.
// Or it may fail with:
//      redeclared with different access struct __xfer_bufptrs
// @see https://stackoverflow.com/questions/47839718/sstream-redeclared-with-public-access-compiler-error
#include "gtest/gtest.h"

// S2: 아래 srs 헤더들이 <mutex>/<vector> 등을 끌어오므로, 접근 지정자 재정의 전에 포함해 둔다.
#include <mutex>
#include <vector>

// Make all private and protected members public.
#define private public
#define protected public

#include <srs_core.hpp>
#include <srs_kernel_error.hpp>

#include <string>
using namespace std;

// we add an empty macro for upp to show the smart tips.
#define VOID

// For errors.
// @remark we directly delete the err, because we allow the user to append a message on failure.
#define HELPER_EXPECT_SUCCESS(x) \
    if ((err = x) != srs_success) fprintf(stderr, "err %s", srs_error_desc(err).c_str()); \
    if (err != srs_success) delete err; \
    EXPECT_TRUE(srs_success == err)
#define HELPER_EXPECT_FAILED(x) \
    if ((err = x) != srs_success) delete err; \
    EXPECT_TRUE(srs_success != err)

// For errors, assert.
#define HELPER_ASSERT_SUCCESS(x) \
    if ((err = x) != srs_success) fprintf(stderr, "err %s", srs_error_desc(err).c_str()); \
    if (err != srs_success) delete err; \
    ASSERT_TRUE(srs_success == err)
#define HELPER_ASSERT_FAILED(x) \
    if ((err = x) != srs_success) delete err; \
    ASSERT_TRUE(srs_success != err)

// For initializing array data.
#define HELPER_ARRAY_INIT(buf, sz, val) \
    for (int _iii = 0; _iii < (int)sz; _iii++) (buf)[_iii] = val

#endif
