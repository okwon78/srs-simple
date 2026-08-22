#!/usr/bin/env bash
#
# runner.sh — srs_simple 실행 스크립트
#
#   ./runner.sh            빌드(필요 시) → 기존 프로세스 종료 → 포그라운드 실행 (Ctrl+C로 종료)
#   ./runner.sh -d         같은 동작이되 백그라운드 실행 (로그: objs/srs_simple.log)
#   ./runner.sh -b         강제로 다시 빌드한 뒤 실행
#   ./runner.sh stop       실행 중인 srs_simple만 종료
#   ./runner.sh status     실행 상태 확인
#
# 기본 RTMP 포트는 src/app/srs_app_config.hpp (1935).
# TS-HLS/플레이어 페이지의 HTTP 서빙(:8080)은 외부 nginx가 담당한다 — conf/nginx.conf.
# nginx가 설치돼 있으면 서버와 함께 띄우고 내린다 (없으면 RTMP/LL-HLS만 동작).
# LL-HLS(:8081)는 서버 내장 HTTP가 직접 서빙한다 — nginx 불필요.
#   플레이어 페이지: TS-HLS는 www/hls.html (http://127.0.0.1:8080/),
#                    LL-HLS는 www/llhls.html (http://127.0.0.1:8080/llhls.html — 파일로 직접 열어도 동작)
#   publish는 GOP ≤ 2초 필요: ffmpeg … -c:v libx264 -g 60 -keyint_min 60 -bf 0 -c:a aac …

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
BIN="$BUILD_DIR/srs_simple"
LOG_FILE="$PROJECT_DIR/objs/srs_simple.log"
PID_FILE="$PROJECT_DIR/objs/srs_simple.pid"
NGINX_CONF="$PROJECT_DIR/conf/nginx.conf"
NGINX_PID="$PROJECT_DIR/objs/nginx.pid"

DAEMON=0
FORCE_BUILD=0
ACTION="run"

usage() {
    sed -n '3,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        -d|--daemon)  DAEMON=1 ;;
        -b|--build)   FORCE_BUILD=1 ;;
        stop)         ACTION="stop" ;;
        status)       ACTION="status" ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

# 실행 중인 srs_simple PID 목록 (자기 자신/이 스크립트는 제외).
running_pids() {
    pgrep -f "$BIN" 2>/dev/null | grep -v "^$$\$" || true
}

# 최대 5초 동안 프로세스가 사라지길 기다린다.
wait_gone() {
    local i=0
    while [ $i -lt 50 ]; do
        [ -z "$(running_pids)" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# TERM → KILL 순으로 종료. 이미 없으면 조용히 통과.
# CONT를 함께 보내는 이유: 디버거에 붙어 멈춰 있는(T 상태) 프로세스는 깨우지 않으면
# 시그널을 처리하지 못한다.
stop_running() {
    local pids
    pids="$(running_pids)"
    [ -z "$pids" ] && { rm -f "$PID_FILE"; return 0; }

    echo "[runner] 기존 srs_simple 종료: $(echo "$pids" | tr '\n' ' ')"
    # shellcheck disable=SC2086
    { kill $pids; kill -CONT $pids; } 2>/dev/null || true
    wait_gone && { rm -f "$PID_FILE"; return 0; }

    pids="$(running_pids)"
    echo "[runner] TERM에 반응 없음 → SIGKILL"
    # shellcheck disable=SC2086
    { kill -9 $pids; kill -CONT $pids; } 2>/dev/null || true
    wait_gone && { rm -f "$PID_FILE"; return 0; }

    # SIGKILL로도 안 죽으면 디버거(lldb/debugserver)가 붙잡고 있는 경우다.
    # 그 디버거를 정리해야 포트가 풀린다 — VSCode 디버그 세션이 여기서 끊긴다.
    for pid in $(running_pids); do
        local ppid pcmd
        ppid="$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')"
        [ -z "$ppid" ] && continue
        pcmd="$(ps -o command= -p "$ppid" 2>/dev/null || true)"
        case "$pcmd" in
            *debugserver*|*lldb*|*gdb*)
                echo "[runner] 디버거에 붙잡힌 프로세스($pid) — 디버거 종료: $ppid"
                kill -9 "$ppid" 2>/dev/null || true
                kill -9 "$pid"  2>/dev/null || true
                ;;
        esac
    done
    wait_gone && { rm -f "$PID_FILE"; return 0; }

    echo "[runner] 종료 실패: $(running_pids | tr '\n' ' ')" >&2
    return 1
}

# HLS/플레이어 페이지 서빙용 nginx (conf/nginx.conf). 없으면 경고만 하고 RTMP만 돈다.
start_nginx() {
    if ! command -v nginx >/dev/null 2>&1; then
        echo "[runner] nginx 없음 — HLS/플레이어 페이지 서빙 생략 (brew install nginx)" >&2
        return 0
    fi
    nginx -p "$PROJECT_DIR" -c "$NGINX_CONF"
    echo "[runner] nginx started — http://127.0.0.1:8080/ (conf/nginx.conf)"
}

stop_nginx() {
    [ -f "$NGINX_PID" ] || return 0
    echo "[runner] nginx 종료"
    nginx -p "$PROJECT_DIR" -c "$NGINX_CONF" -s quit 2>/dev/null \
        || kill "$(cat "$NGINX_PID" 2>/dev/null)" 2>/dev/null || true
    rm -f "$NGINX_PID"
}

build_if_needed() {
    if [ "$FORCE_BUILD" -eq 1 ] || [ ! -x "$BIN" ]; then
        echo "[runner] 빌드 중..."
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" >/dev/null
        cmake --build "$BUILD_DIR" -j8
    fi
}

show_status() {
    local pids
    pids="$(running_pids)"
    if [ -n "$pids" ]; then
        echo "[runner] 실행 중: $(echo "$pids" | tr '\n' ' ')"
    else
        echo "[runner] 실행 중 아님"
    fi
}

case "$ACTION" in
    stop)
        stop_running
        stop_nginx
        echo "[runner] stopped"
        exit 0
        ;;
    status)
        show_status
        exit 0
        ;;
esac

build_if_needed
stop_running

# HLS 세그먼트(objs/hls)와 로그는 실행 디렉터리 기준으로 생성되므로 프로젝트 루트에서 실행한다.
cd "$PROJECT_DIR"
mkdir -p "$PROJECT_DIR/objs"

stop_nginx
start_nginx

if [ "$DAEMON" -eq 1 ]; then
    "$BIN" >>"$LOG_FILE" 2>&1 &
    pid=$!
    echo "$pid" >"$PID_FILE"
    sleep 0.5
    # 포트 바인딩 실패 등은 즉시 죽으므로 방금 띄운 PID를 직접 확인한다.
    if ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$PID_FILE"
        echo "[runner] 기동 실패 — 로그 확인: $LOG_FILE" >&2
        tail -n 20 "$LOG_FILE" >&2 || true
        exit 1
    fi
    echo "[runner] started (pid $pid), log: $LOG_FILE"
    echo "[runner] rtmp://127.0.0.1:1935/live/livestream , http://127.0.0.1:8080/ (nginx, TS-HLS)"
    echo "[runner] LL-HLS: http://127.0.0.1:8081/live/<stream>.m3u8 (내장 HTTP — 플레이어는 :8080/llhls.html)"
else
    echo "[runner] starting (Ctrl+C로 종료) — rtmp://127.0.0.1:1935 , http://127.0.0.1:8080 (nginx, TS-HLS) , :8081 (LL-HLS)"
    # 포그라운드 종료(Ctrl+C 포함) 시 nginx도 함께 내린다.
    trap 'stop_nginx' EXIT
    "$BIN"
fi
