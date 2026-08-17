// srs_simple — 원본: trunk/src/app/srs_app_config.cpp
// 전역 설정 객체의 정의만 남는다 (CLAUDE.md §5.4).
#include <srs_app_config.hpp>

static SrsSimpleConfig _config;
SrsSimpleConfig* _srs_config = &_config;
