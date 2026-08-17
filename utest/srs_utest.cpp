// srs_simple — 원본: trunk/src/utest/srs_utest.cpp
#include <srs_utest.hpp>

#include <srs_kernel_log.hpp>

// 테스트 출력이 로그에 묻히지 않게 Warn 이상만 출력한다.
int main(int argc, char** argv)
{
    _srs_log = new SrsConsoleLog(SrsLogLevelWarn, false);

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
