# srs_simple

[SRS](https://github.com/ossrs/srs)의 RTMP 라이브 스트리밍 경로를 이해하기 위한 **교육용 단순화 서버**.

원본 SRS(약 20,000줄+)에서 RTMP 코어 경로만 남기고 단순화하되, **클래스/파일/메서드 이름과 구조를 원본과 1:1로 미러링**했다. 이 코드를 이해하면 원본 SRS의 동일한 코드가 확장판으로 읽힌다. 설계 근거·원본 매핑·구현 이력은 [CLAUDE.md](CLAUDE.md) 참조.

- 지원: RTMP publish(OBS/ffmpeg) → play(ffplay/VLC), 1 publisher → N player 팬아웃, GOP 캐시(중간 입장 즉시 재생), 지터 보정, 오디오 전용 스트림, **HLS play**(TS 세그먼트 + m3u8 생성, 서빙은 외부 nginx — Safari/hls.js/ffplay), **LL-HLS play**(fMP4 파트 0.5초 + 인메모리 + 내장 HTTP(:8081)의 블로킹 서빙 — hls.js `lowLatencyMode`/Safari, 실측 지연 ~2초)
- 규모: `src/` 약 19,300줄 + `utest/` 약 6,300줄 (gtest 130개)
- 의존성: CMake + C++11 + pthread. OpenSSL 불필요 (심플 핸드셰이크만 구현). TS-HLS 재생 데모에만 nginx 필요 (`brew install nginx`) — LL-HLS는 내장 HTTP라 nginx 불필요

## 빌드와 테스트

```sh
cmake -S . -B build
cmake --build build -j8
./build/srs_utest     # 유닛테스트 130개
```

## 실행

```sh
./runner.sh           # 빌드(필요 시) → srs_simple(:1935 RTMP, :8081 LL-HLS) + nginx(:8080 TS-HLS) 함께 실행
```

또는 수동으로:

```sh
./build/srs_simple                        # rtmp://localhost:1935 (포트는 src/app/srs_app_config.hpp)
nginx -p "$(pwd)" -c conf/nginx.conf      # http://localhost:8080 — HLS 파일/플레이어 페이지 서빙
nginx -p "$(pwd)" -s quit                 # nginx 종료
```

TS-HLS 세그먼트/플레이리스트는 실행 디렉터리의 `./objs/hls/`에 생성되고, **HTTP 서빙(:8080)은
외부 nginx가 담당한다**([conf/nginx.conf](conf/nginx.conf) — 원본 SRS의 `hls_path` 기본값
`./objs/nginx/html`이 전제하는 것과 같은 배포 모델, CLAUDE.md §5.6 S11).
브라우저에서 `http://localhost:8080/`을 열면 HLS 플레이어 페이지([www/hls.html](www/hls.html))가 뜬다.
두 플레이어 페이지 모두 라이브 전용(DVR 비활성)이다 — 뒤로 seek하거나 일시정지 후 재개하면
라이브 엣지로 스냅해 항상 최신 영상만 재생한다 (YouTube 라이브의 "뒤로 돌려보기 금지"와 같은 정책).

LL-HLS는 파일을 만들지 않는다 — 파트/세그먼트는 인메모리에 게시되고 **서버 내장 HTTP(:8081)가
직접 서빙한다** (블로킹 리로드·프리로드 힌트는 정적 파일 서버가 못 하는 일이라서 — CLAUDE.md §4.4).
플레이어 페이지는 [www/llhls.html](www/llhls.html) (`http://localhost:8080/llhls.html`, 파일로 직접 열어도 동작).

## 데모 시나리오

테스트 미디어 생성 (h264+aac, 키프레임 2초 간격):

```sh
ffmpeg -f lavfi -i "testsrc2=size=640x360:rate=30" -f lavfi -i "sine=frequency=440" \
       -c:v libx264 -g 60 -pix_fmt yuv420p -c:a aac -t 30 test.mp4
```

**1. publish → play** (터미널 2개):

```sh
ffmpeg -re -stream_loop -1 -i test.mp4 -c copy -f flv rtmp://localhost/live/test
ffplay rtmp://localhost/live/test
```

같은 일을 하는 단축 스크립트가 [publish.sh](publish.sh)/[play.sh](play.sh)다. 단 `publish.sh`는
**재인코딩**해서 보낸다(`-c:v libx264 -g 60 -keyint_min 60 -bf 0 -c:a aac`) — LL-HLS(§9)까지
한 번에 보려면 GOP가 2초 이하여야 하기 때문이다. RTMP/TS-HLS만 볼 때는 위의 `-c copy`로 충분하다.

**2. 중간 입장 즉시 재생 (GOP 캐시)** — publish를 켜둔 채 아무 때나 ffplay를 시작해도 다음 키프레임을 기다리지 않고 즉시 화면이 나온다. `consumer_dumps`가 onMetaData → 시퀀스 헤더 → 마지막 GOP를 먼저 밀어주기 때문 (ARCH §4.2).

**3. 동시 다중 플레이어** — ffplay를 여러 개 띄우면 각자 독립 큐/지터로 팬아웃된다. 페이로드 복사는 0회 (`SrsSharedPtrMessage`, ARCH §5.2).

**4. publish 중단 → 재시작 (REPUBLISH)** — publisher를 `Ctrl+C`(FCUnpublish)로 끊으면 서버 로그에 `rtmp: retry for republish`가 찍히고, 다시 publish하면 붙어 있던 플레이어가 이어서 재생된다.

**5. OBS publish** — 설정 → 방송: 서비스 *사용자 지정*, 서버 `rtmp://localhost/live`, 스트림 키 `test`. OBS는 SetChunkSize(4096)를 먼저 보내는 FMLE 스타일 publisher로 식별된다.

**6. VLC play** — `VLC rtmp://localhost/live/test` (또는 GUI에서 네트워크 스트림 열기).

**7. 오디오 전용** — `-vn`으로 publish하면 GOP 캐시는 pure-audio 가드(115 패킷 규칙)에 따라 캐시하지 않고 실시간 릴레이만 한다:

```sh
ffmpeg -re -stream_loop -1 -i test.mp4 -vn -c:a copy -f flv rtmp://localhost/live/audio
```

**8. HLS play** — publish를 켜둔 채 (첫 세그먼트가 만들어지는 ~10초 후):

```sh
open http://localhost:8080/                      # 브라우저 플레이어 페이지 (www/hls.html, nginx 서빙)
ffplay http://localhost:8080/live/test.m3u8      # 또는 Safari에서 URL 열기, VLC 네트워크 스트림
curl http://localhost:8080/live/test.m3u8        # 플레이리스트 직접 확인
ls objs/hls/live/                                # ts 세그먼트 파일들
```

플레이어 페이지는 `Hls.isSupported()`가 참이면 hls.js(CDN)로, 아니면 네이티브 HLS로 재생한다 —
**Safari에서도 `isSupported()`가 참이라 실제로는 hls.js 경로를 탄다** (네이티브는 CDN 로드 실패 시 폴백).
Safari 네이티브 HLS로 보려면 페이지 대신 m3u8 URL을 Safari에 직접 열면 된다.
입력창의 경로를 `/live/test.m3u8`처럼 스트림에 맞게 바꾸고 재생을 누르면 된다.

RTMP play(1초 미만 지연)와 HLS play(세그먼트 단위, 수십 초 지연)를 나란히 띄우면 같은 스트림
허브에서 갈라진 push/pull 두 소비 모델의 지연 차이가 그대로 보인다 (CLAUDE.md §4.3).

**9. LL-HLS play (지연 ~2초)** — LL-HLS는 세그먼트(2초)를 키프레임에서만 자르므로 **인코더 GOP가
2초 이하**여야 한다. `-c copy`는 원본 GOP를 그대로 쓰므로 재인코딩으로 publish한다:

```sh
ffmpeg -re -f lavfi -i "testsrc=size=640x360:rate=30" -f lavfi -i "sine=frequency=440:sample_rate=44100" \
       -c:v libx264 -preset veryfast -tune zerolatency -g 60 -keyint_min 60 -bf 0 -pix_fmt yuv420p \
       -c:a aac -f flv rtmp://localhost/live/test
open http://localhost:8080/llhls.html                # hls.js lowLatencyMode 플레이어 (www/llhls.html)
ffplay http://localhost:8081/live/test.m3u8          # 플레이어 페이지 없이 재생
curl "http://localhost:8081/live/test.m3u8"          # PART/PRELOAD-HINT가 실린 플레이리스트 확인
curl "http://localhost:8081/live/test.m3u8?_HLS_msn=999"   # 먼 미래 지목 → 400 (블로킹 리로드 경계)
```

두 플레이어 페이지 모두 라이브 엣지 지연(hls.latency)과 버퍼 잔량을 실시간 표시한다. 브라우저 개발자
도구 네트워크 탭에서 `_HLS_msn=N&_HLS_part=P` 블로킹 리로드와 파트 단위 로딩을 관찰할 수 있다.
LL-HLS 페이지도 Safari에서 hls.js 경로로 재생된다 — Safari 네이티브 LL-HLS(m3u8 URL 직접 열기)는
현재 서버에서 거부된다 (`MEDIA_ERR_SRC_NOT_SUPPORTED`, 원인 미확인 — CLAUDE.md §8 S17).
기존 HLS 페이지(`/`)와 나란히 열면 ~30초 vs ~2초의 지연 차이가 그대로 보인다 —
같은 publish를 RTMP·TS-HLS·LL-HLS 세 경로가 동시에 소비한다 (CLAUDE.md §4.4,
해설은 [docs/part11-llhls.md](docs/part11-llhls.md)).

## 코드 읽는 순서

구현 세션 순서(S1→S15)가 곧 읽는 순서다. 각 단계는 아래 계층을 하나씩 쌓는다:

| 순서 | 주제 | 파일 |
| --- | --- | --- |
| S1 | 에러 체인·로깅·바이트 커서 | `src/core/srs_kernel_error.*`, `srs_kernel_log.*`, `src/kernel/srs_kernel_buffer.*` |
| S2 | I/O 추상화·수신 버퍼·스레드 모델·리스너 | `src/protocol/srs_protocol_io.hpp`, `srs_protocol_stream.*`, `src/app/srs_app_st.*`, `srs_app_listener.*` |
| S3 | 메시지 모델·핸드셰이크 | `src/kernel/srs_kernel_flv.*`, `srs_kernel_codec.*`, `src/protocol/srs_protocol_rtmp_handshake.*` |
| S4 | AMF0 코덱 | `src/protocol/srs_protocol_amf0.*` |
| S5 | 청크 스트림 스택 (심장 1) | `src/protocol/srs_protocol_rtmp_stack.*`의 `SrsProtocol` |
| S6 | 커맨드 패킷 12종·서버 파사드 | 같은 파일의 `SrsPacket` 서브클래스와 `SrsRtmpServer`, `srs_protocol_utility.*` |
| S7 | 연결 수명주기 | `src/app/srs_app_rtmp_conn.*`, `srs_app_server.*`, `srs_app_conn.*`, `src/main/srs_main_server.cpp` |
| S8 | 스트림 허브 (심장 2) | `src/app/srs_app_source.*` — Source/Consumer/GopCache/MetaCache/Jitter/Queue |
| S10 | HLS 트랜스먹스 | `src/kernel/srs_kernel_codec.*`(SrsFormat) → `srs_kernel_ts.*` → `src/app/srs_app_hls.*`, `srs_app_fragment.*` (서빙은 [conf/nginx.conf](conf/nginx.conf)) |
| S12~S15 | LL-HLS | `src/kernel/srs_kernel_mp4.*`(fMP4) → `src/app/srs_app_llhls.*`(파트 컷·인메모리·플레이리스트) → `srs_app_http_conn.*`(블로킹 서빙) |

각 지점에서 원본 SRS의 어느 file:line을 열면 되는지는 [CLAUDE.md §7 탐색 가이드](CLAUDE.md#7-이-코드를-읽은-뒤-원본-srs를-여는-법-탐색-가이드)에 표로 정리되어 있다. 원본과 의도적으로 다르게 구현한 지점(pthread 전환 등)은 CLAUDE.md §5.6에 전부 기록되어 있다 — 코드 주석이 아니라 그 문서가 차이의 단일 출처다.

## 검증된 환경

macOS(Darwin 25) + ffmpeg/ffplay 8.1, VLC 3, OBS 31, Chrome/hls.js, Safari 26 기준으로 다음을 확인했다 (2026-08-09 RTMP / 2026-08-21 HLS / 2026-08-22 LL-HLS / 2026-08-23 Safari, CLAUDE.md §8):

- ffmpeg publish × ffplay/VLC play, OBS publish × ffmpeg play(ffplay와 동일한 libavformat RTMP 클라이언트)
- 중간 입장 즉시 재생, 동시 다중 플레이어, 오디오 전용 스트림
- publish 중단→재시작(REPUBLISH), 비정상 종료(kill -9)에도 서버 생존
- 30분+ 연속 publish 안정성
- HLS: 10초 세그먼트 생성·롤링 윈도우·m3u8, 전 세그먼트 ffmpeg 디코딩 오류 0건, RTMP play와 동시 소비
- LL-HLS: hls.js(lowLatencyMode) 실재생 + 블로킹 리로드(`_HLS_msn`) 동작, ffplay/ffmpeg HLS demuxer 디코딩 오류 0건, RTMP·TS-HLS·LL-HLS 3경로 동시 소비, **종단 지연 실측 2.2초**(화면 타임스탬프 비교 — 목표 ≤3초), republish 후 msn 연속, 장시간 publish에서 RSS 상한 유지(인메모리 윈도우 일정)
- Safari 26: 플레이어 페이지는 hls.js 경로로 재생됨(네이티브 아님). 네이티브 LL-HLS(m3u8 직접 열기)는 `MEDIA_ERR_SRC_NOT_SUPPORTED`로 거부 — 원인 미확인 (CLAUDE.md §8 S17)
