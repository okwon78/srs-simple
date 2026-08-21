// srs_simple — 원본: trunk/src/main/srs_main_server.cpp (do_main:97)
// 원본은 st_init 후 SrsServer를 하이브리드 서버로 감싸 이벤트 루프를 돌린다.
// pthread 모델에서는 리스너/연결이 각자 스레드에서 돌므로 메인 스레드는 대기만 한다.
#include <srs_core.hpp>

#include <signal.h>
#include <unistd.h>

#include <srs_app_config.hpp>
#include <srs_app_server.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

srs_error_t do_main(int argc, char** argv)
{
    srs_error_t err = srs_success;

    // 끊긴 소켓에 write하면 SIGPIPE로 프로세스가 죽는다 — 비정상 종료한 클라이언트가
    // 서버를 못 죽이게 무시한다 (원본은 ST 라이브러리가 처리).
    ::signal(SIGPIPE, SIG_IGN);

    srs_trace("%s started, pid=%d", RTMP_SIG_SRS_SERVER, (int)::getpid());

    SrsServer* server = new SrsServer();

    if ((err = server->initialize()) != srs_success) {
        return srs_error_wrap(err, "server initialize");
    }

    if ((err = server->listen()) != srs_success) {
        return srs_error_wrap(err, "server listen");
    }

    srs_trace("SRS_SIMPLE ready, rtmp://127.0.0.1:%d/live/livestream", _srs_config->listen_port);
    srs_trace("HLS ready, http://127.0.0.1:%d/live/livestream.m3u8", _srs_config->http_listen_port);

    // 워커 스레드들이 서비스하는 동안 메인 스레드는 대기 (Ctrl+C로 종료).
    while (true) {
        ::pause();
    }

    srs_freep(server);
    return err;
}

int main(int argc, char** argv)
{
    srs_error_t err = do_main(argc, argv);

    if (err != srs_success) {
        srs_error("Failed, %s", srs_error_desc(err).c_str());
        int ret = srs_error_code(err);
        srs_freep(err);
        return ret;
    }

    return 0;
}
