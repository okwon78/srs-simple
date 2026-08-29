#!/usr/bin/env bash
#
# build.sh — srs_simple 빌드 스크립트 (기본: Release)
#
#   ./build.sh             Release 빌드 (-O3, NDEBUG)
#   ./build.sh -d          Debug 빌드 (CMakeLists의 기본 타입)
#   ./build.sh -c          build 디렉터리를 지우고 처음부터 빌드
#   ./build.sh -t          빌드 후 유닛테스트(./build/srs_utest) 실행
#   ./build.sh -j N        병렬 컴파일 잡 수 (기본: CPU 코어 수)
#
# 산출물은 runner.sh가 그대로 쓰는 build/ 한 곳에 만든다 (build/srs_simple, build/srs_utest).
# CMAKE_BUILD_TYPE만 바뀌므로 같은 디렉터리에서 Debug ↔ Release를 오갈 수 있다 —
# 전환 시 소스는 전부 다시 컴파일된다 (CMake가 플래그 변경을 감지).

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

BUILD_TYPE="Release"
CLEAN=0
RUN_TESTS=0
if command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.ncpu)"          # macOS
elif command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"                      # Linux
else
    JOBS=8
fi

usage() {
    sed -n '3,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        -d|--debug)   BUILD_TYPE="Debug" ;;
        -r|--release) BUILD_TYPE="Release" ;;
        -c|--clean)   CLEAN=1 ;;
        -t|--test)    RUN_TESTS=1 ;;
        -j|--jobs)    shift; JOBS="${1:-8}" ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

if [ "$CLEAN" -eq 1 ]; then
    echo "[build] clean — $BUILD_DIR 삭제"
    rm -rf "$BUILD_DIR"
fi

echo "[build] configure — $BUILD_TYPE"
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >/dev/null

echo "[build] compile — j$JOBS"
cmake --build "$BUILD_DIR" -j"$JOBS"

if [ "$RUN_TESTS" -eq 1 ]; then
    echo "[build] utest"
    "$BUILD_DIR/srs_utest"
fi

echo "[build] done — $BUILD_DIR/srs_simple ($BUILD_TYPE)"
