# CLAUDE.md — srs_simple

> **목적**: 원본 [SRS](file:///Users/obiwan/Dev/srs) 프로젝트의 RTMP 라이브 스트리밍 경로를 이해하기 위한 **교육용 단순화 서버**.
> 이 프로젝트의 코드를 이해하면 원본 SRS의 동일한 코드가 그대로 읽히도록, **클래스/파일/메서드 이름과 구조를 SRS와 1:1로 미러링**한다.
> 복잡도만 제거하고, 개념은 제거하지 않는다.

이 파일은 설계 문서이자 작업 규칙이다. 코드 주석이 `CLAUDE.md §5.6` 형태로 이 파일의 절 번호를 참조하므로 **절 번호는 재사용/변경하지 않는다**.
빌드·실행·데모 시나리오는 [README.md](README.md) 참조. 구현은 완료 상태이며 이력은 §8에 있다.

원본 분석 기준: `/Users/obiwan/Dev/srs/trunk/src/` (SRS 6.0)

---

## 0. 참조 프로젝트와 작업 규칙

이 프로젝트는 원본 SRS 소스를 참조한다: `../srs` (절대 경로: `/Users/obiwan/Dev/srs`)

- 원본 개발 가이드: [../srs/AGENTS.md](file:///Users/obiwan/Dev/srs/AGENTS.md)
- 소스 루트: `../srs/trunk/src/` — `core/`, `kernel/`, `protocol/`, `app/`, `main/`, `utest/`
- 빌드 산출물/생성 헤더: `../srs/trunk/objs/` (`srs_auto_headers.hpp` 포함)

### 작업 규칙

1. `../srs`는 **읽기 전용 참조**다. 별도 지시가 없는 한 해당 디렉터리의 파일은 수정하지 않는다.
2. 작업 시작 시 이 파일과, 손댈 영역에 해당하는 원본 파일(§7의 매핑 참조)을 먼저 읽는다.
3. **클래스/메서드/파일 이름은 원본과 동일하게 유지한다** — 단순화하더라도 이름을 바꾸지 않는다. 원본 대조가 이 프로젝트의 존재 이유다.
4. 원본과 의도적으로 다르게 구현한 지점은 **§5(특히 §5.6)에 기록한다**. 코드 주석은 이 문서를 가리키는 포인터일 뿐, 차이의 단일 출처는 이 문서다.
5. 작업 종료 시 컴파일과 전체 테스트가 통과해야 한다 (`cmake --build build -j8 && ./build/srs_utest`).

### 설정 위치

- [.claude/settings.json](.claude/settings.json) — `permissions.additionalDirectories`에 `../srs` 등록 (Claude가 원본 소스를 읽고 검색 가능)
- [.vscode/c_cpp_properties.json](.vscode/c_cpp_properties.json) — SRS 인클루드 경로 (IntelliSense / 헤더 점프)
- [srs_simple.code-workspace](srs_simple.code-workspace) — `srs_simple` + `srs`를 함께 여는 멀티 루트 워크스페이스

---

## 1. 목표와 비목표

### 목표

- OBS / ffmpeg로 **publish** 하고 ffplay / VLC로 **play** 되는 실동작 RTMP 서버
- **HLS play** (S10 추가): RTMP publish → TS 세그먼트 + m3u8 생성 + HTTP 서빙 → Safari/hls.js/ffplay가 `http://…/live/livestream.m3u8`로 재생
- SRS와 동일한 레이어링: `core → kernel → protocol → app → main`
- SRS와 동일한 클래스 이름 (`SrsProtocol`, `SrsRtmpServer`, `SrsRtmpConn`, `SrsLiveSource`, `SrsLiveConsumer`, `SrsGopCache`, `SrsHls`, `SrsTsContext` …)
- SRS와 동일한 이디엄: `srs_error_t` 에러 체인, `SrsBuffer` 바이트 커서, `MockBufferIO` 기반 프로토콜 단위 테스트
- 원본의 동일 경로(약 20,000줄+)를 개념 손실 없이 축소 — 실제 결과는 `src/` 16,003줄 + `utest/` 4,396줄 (S10 HLS 포함. S9 시점에는 12,436/3,897줄). 원본 로직을 그대로 옮긴 데다 교육용 주석 비중이 커서 줄 수 자체는 초기 목표(4,000~5,000줄)보다 크다

### 비목표 (원본에서 의도적으로 제거)

> HLS와 그 서빙용 HTTP 정적 서버는 원래 비목표였으나 **S10에서 목표로 승격해 구현했다** (§5.6 S10, §8).

| 제거 대상 | 원본 위치 | 제거 이유 |
| --- | --- | --- |
| 복잡 핸드셰이크 (HMAC-SHA256/DH) | `srs_protocol_rtmp_handshake.cpp` 약 1,300줄 중 1,000줄 | OBS/ffmpeg는 심플 핸드셰이크로 충분. Flash 전용 |
| ST 코루틴 라이브러리 | `3rdparty/st-srs`, `srs_protocol_st.cpp` | pthread 1-connection-1-thread로 대체 (§5.1) |
| 설정 시스템 (`SrsConfig`) | `srs_app_config.cpp` 10,000줄+ | 상수 구조체로 대체 (§5.4) |
| HTTP API, DVR/DASH/HDS, RTC/SRT, Edge/Origin 클러스터, Forward/Transcode | `app/` 대부분 | RTMP/HLS 학습과 무관 |
| 훅/보안/통계 (`SrsHttpHooks`, `SrsRefer`, `SrsSecurity`, `SrsStatistic`) | `srs_app_*.cpp` | 부가 기능 |
| AMF3, Aggregate 메시지, mix_correct, atc, merged-read, iovec 배칭 | 산재 | 희귀 경로/성능 최적화 |

---

## 2. RTMP 프로토콜 핵심 (구현 대상 요약)

이 절은 구현한 프로토콜의 압축 명세다. 상세 근거는 원본 file:line을 §7에 매핑했다.

### 2.1 핸드셰이크 (심플만 구현)

```text
client                              server
  ── C0(1B, 0x03) + C1(1536B) ──▶   read_fully(1537)
  ◀─ S0(1B) + S1(1536B) + S2(1536B) ─  write(3073)
  ── C2(1536B) ──────────────▶      read_fully(1536), 검증 없이 폐기
```

- C1/S1 = `time(4) | version(4) | random(1528)`
- S2 = 받은 C1의 1536바이트 복사. C2는 읽고 버림 (SRS도 검증하지 않음 — ffmpeg 호환)
- 원본은 복잡 핸드셰이크 시도 → `ERROR_RTMP_TRY_SIMPLE_HS` → 심플 폴백. 우리는 심플만.

### 2.2 청크 스트림

**Basic header** (1~3바이트): `fmt(2bit) | csid(6bit)`. csid 0 → 2바이트(64+b1), csid 1 → 3바이트(64+b1+b2*256).

**Message header** (fmt별 크기 `{11, 7, 3, 0}`):

| fmt | 내용 |
| --- | --- |
| 0 | timestamp(3, 절대값) + payload_length(3) + type(1) + stream_id(4, **리틀엔디언**) |
| 1 | timestamp delta(3) + payload_length(3) + type(1) |
| 2 | timestamp delta(3) |
| 3 | 전부 이전 청크 상태 상속 |

- timestamp 필드가 `0xFFFFFF`이면 4바이트 **extended timestamp**가 뒤따름
- 수신: csid별 `SrsChunkStream`에 상태 저장, `payload_length`가 찰 때까지 `in_chunk_size` 단위로 누적 — 이것이 청크 재조립의 전부
- 송신: **첫 청크는 항상 fmt=0, 이어지는 청크는 항상 fmt=3** (SRS도 fmt=1/2를 송신하지 않음). 파싱은 4가지 fmt 모두 필요
- 청크 크기: 기본 128. 인바운드 `SetChunkSize`를 반드시 반영, 아웃바운드는 60000 사용

### 2.3 프로토콜 컨트롤 메시지 (csid=2, stream_id=0)

상호운용에 필요한 최소 집합:

| 메시지 | type | 방향 | 비고 |
| --- | --- | --- | --- |
| SetChunkSize | 1 | 송+수 | connect 응답 전에 송신 (OBS 이슈 #454) |
| Acknowledgement | 3 | 자동 송신 | 수신 바이트가 윈도우 절반 넘으면 |
| UserControl | 4 | 송+수 | StreamBegin(0) — play 시작 전 필수. PingRequest(6)→PingResponse(7) 에코 |
| WindowAckSize | 5 | 송+수 | |
| SetPeerBandwidth | 6 | 송신만 | 관례상 1회 송신 |

### 2.4 AMF0 (서브셋)

필요 타입만: **Number(0x00, 8B IEEE754 BE), Boolean(0x01), String(0x02, u16 len), Object(0x03, `00 00 09`로 종료), Null(0x05), Undefined(0x06), EcmaArray(0x08, 읽기 전용 — ffmpeg의 onMetaData)**.

- Object의 **프로퍼티 삽입 순서는 보존**해야 함 (`vector<pair<string, SrsAmf0Any*>>`) — 원본 주석: 정렬하면 FMLE가 죽음
- StrictArray/Date/LongString/XmlDocument/TypedObject/Reference는 불필요

### 2.5 커맨드 흐름

**Publisher (OBS/ffmpeg = FMLE 스타일):**

```text
C→S connect(app)              tid=1  [csid3, type20]
S→C WindowAckSize, SetPeerBW, SetChunkSize
S→C _result(connect)          tid=1  (NetConnection.Connect.Success)
C→S releaseStream(name)       tid=2  → S→C _result
C→S FCPublish(name)           tid=3  → S→C _result
C→S createStream()            tid=4  → S→C _result(streamId=1)
C→S publish(name, "live")     tid=5, sid=1 [csid5]
S→C onFCPublish + onStatus(NetStream.Publish.Start)
C→S @setDataFrame/onMetaData(type18) → audio(type8)/video(type9) 반복
```

**Player (ffplay/VLC):**

```text
C→S connect(app)              tid=1
S→C (위와 동일한 부트스트랩)
C→S createStream()            tid=2  → S→C _result(streamId=1)
C→S play(name)                tid=0, sid=1
S→C UserControl StreamBegin(sid=1)          [stream_id=0으로 전송]
S→C onStatus(NetStream.Play.Reset)          [stream_id=1로 전송]
S→C onStatus(NetStream.Play.Start)
S→C |RtmpSampleAccess(true,true) [type18]
S→C onMetaData → AVC/AAC 시퀀스 헤더 → GOP 캐시 → 라이브 메시지
```

필요 패킷 클래스는 12개: `SrsConnectAppPacket(+Res)`, `SrsCreateStreamPacket(+Res)`, `SrsFMLEStartPacket(+Res)`, `SrsPublishPacket`, `SrsPlayPacket`, `SrsOnStatusCallPacket`, `SrsOnMetaDataPacket`, `SrsSetChunkSizePacket`, `SrsSetWindowAckSizePacket`, `SrsAcknowledgementPacket`, `SrsSetPeerBandwidthPacket`, `SrsUserControlPacket`. (원본은 약 25개) 여기에 `SrsSampleAccessPacket`을 더해 구현했다.

---

## 3. 레이어 아키텍처

원본 SRS와 동일한 5레이어. 화살표는 의존 방향(위→아래만 의존).

```text
┌───────────────────────────────────────────────────────────┐
│ main/     srs_main_server.cpp — main(): Server 생성, listen │
├───────────────────────────────────────────────────────────┤
│ app/      "정책" — 연결 수명주기와 스트림 허브                  │
│   SrsServer / SrsTcpListener     accept → Rtmp/Http Conn 생성 │
│   SrsRtmpConn                    핸드셰이크→connect→식별→루프  │
│   SrsLiveSource / SrsLiveConsumer  1 publisher → N player    │
│   SrsGopCache / SrsMetaCache / SrsRtmpJitter                 │
│   SrsOriginHub / SrsHls / SrsHlsMuxer  RTMP→HLS 트랜스먹스(S10)│
│   SrsFragmentWindow / SrsHttpConn  세그먼트 윈도우·HTTP 서빙(S10)│
│   SrsCoroutine (pthread 구현)                                │
├───────────────────────────────────────────────────────────┤
│ protocol/ "메커니즘" — RTMP 바이트 ↔ 메시지                    │
│   SrsSimpleHandshake             핸드셰이크                   │
│   SrsProtocol / SrsChunkStream   청크 재조립·직렬화             │
│   SrsPacket 서브클래스 12종        AMF0 커맨드 인코딩/디코딩      │
│   SrsRtmpServer                  커맨드 흐름 파사드             │
│   SrsAmf0Any/Object/EcmaArray    AMF0 코덱                   │
│   ISrsProtocolReadWriter         소켓 추상화 (테스트 가능성 핵심) │
│   SrsFastStream                  수신 버퍼                    │
├───────────────────────────────────────────────────────────┤
│ kernel/   프로토콜 무관 유틸                                   │
│   SrsBuffer                      빅엔디언 바이트 커서            │
│   SrsMessageHeader / SrsCommonMessage / SrsSharedPtrMessage  │
│   SrsFlvVideo / SrsFlvAudio      시퀀스 헤더/키프레임 판별        │
│   SrsFormat                      avcC/ASC/NALU 코덱 파싱 (S10) │
│   SrsTsContext / SrsTsMessageCache  MPEG-TS 먹서 (S10)        │
│   SrsFileWriter / SrsFileReader  세그먼트 파일 IO (S10)         │
├───────────────────────────────────────────────────────────┤
│ core/     전역 기반                                          │
│   srs_error_t (에러 체인), srs_freep, 로깅 매크로               │
└───────────────────────────────────────────────────────────┘
```

### 파일 레이아웃 ↔ 원본 매핑

`실제`는 hpp+cpp 합계 줄 수(주석 포함). 각 파일 상단에는 대응하는 원본 파일을 적은 헤더 주석이 있다.

| srs_simple | 원본 SRS (`trunk/src/`) | 범위 | 실제 |
| --- | --- | --- | --- |
| `src/core/srs_core.hpp` | `core/srs_core.hpp` (+ `srs_core_time.hpp`) | freep/기본 타입 | 포함 |
| `src/core/srs_kernel_error.{hpp,cpp}` | `kernel/srs_kernel_error.*` | 에러코드 축소, X-macro 제거 | 402 |
| `src/core/srs_kernel_log.{hpp,cpp}` | `kernel/srs_kernel_log.*` + `protocol/srs_protocol_log.*` | 콘솔 로거만 | 298 |
| `src/kernel/srs_kernel_buffer.{hpp,cpp}` | `kernel/srs_kernel_buffer.*` | BE 1/2/3/4/8 + LE4 + string/bytes | 346 |
| `src/kernel/srs_kernel_stream.{hpp,cpp}` | `kernel/srs_kernel_stream.*` | `SrsSimpleStream` (MockBufferIO용) | 포함 |
| `src/kernel/srs_kernel_flv.{hpp,cpp}` | `kernel/srs_kernel_flv.*` | 메시지 3종 + 상수 + c0/c3 헤더 직렬화 | 652 |
| `src/kernel/srs_kernel_codec.{hpp,cpp}` | `kernel/srs_kernel_codec.*` (4,700줄) | S1~S9: FLV 판별자만. S10: `SrsFormat` 코덱 파싱(avcC/ASC/NALU) 추가 | 826 |
| `src/kernel/srs_kernel_io.hpp` | `kernel/srs_kernel_io.hpp` | S10에서 원본 위치로 복원 (그전엔 protocol_io에 병합) | 38 |
| `src/kernel/srs_kernel_file.{hpp,cpp}` | `kernel/srs_kernel_file.*` + `srs_kernel_utility.cpp`의 path 헬퍼 | Writer/Reader 최소만 (S10) | 265 |
| `src/kernel/srs_kernel_ts.{hpp,cpp}` | `kernel/srs_kernel_ts.*` (5,900줄) | 인코더만. 패킷 클래스 트리 제거 (S10, §5.6) | 766 |
| `src/protocol/srs_protocol_io.hpp` | `protocol/srs_protocol_io.hpp` | 프로토콜 IO 인터페이스 (kernel_io 포함) | 76 |
| `src/protocol/srs_protocol_stream.{hpp,cpp}` | `protocol/srs_protocol_stream.*` | `SrsFastStream::grow/read_slice`. merged-read 제거 | 184 |
| `src/protocol/srs_protocol_amf0.{hpp,cpp}` | `protocol/srs_protocol_amf0.*` (1,779줄) | 7타입 서브셋 | 1,716 |
| `src/protocol/srs_protocol_rtmp_handshake.{hpp,cpp}` | `protocol/srs_protocol_rtmp_handshake.*` (1,310줄) | 심플만 + `SrsHandshakeBytes` | 211 |
| `src/protocol/srs_protocol_rtmp_stack.{hpp,cpp}` | `protocol/srs_protocol_rtmp_stack.*` (4,620줄) | SrsProtocol + 패킷 13종 + SrsRtmpServer | 4,217 |
| `src/protocol/srs_protocol_rtmp_msg_array.{hpp,cpp}` | `protocol/srs_protocol_rtmp_msg_array.*` | `SrsMessageArray` | 포함 |
| `src/protocol/srs_protocol_utility.{hpp,cpp}` | `protocol/srs_protocol_utility.*` + `kernel/srs_kernel_utility.*` 문자열 헬퍼 | `srs_discovery_tc_url` 손 파싱 (SrsHttpUri 제거) | 316 |
| `src/app/srs_app_st.{hpp,cpp}` | `app/srs_app_st.*` + `protocol/srs_protocol_st.*` | pthread 기반 SrsCoroutine | 687 |
| `src/app/srs_app_listener.{hpp,cpp}` | `app/srs_app_listener.*` (812줄) | TCP만 | 295 |
| `src/app/srs_app_conn.{hpp,cpp}` | `app/srs_app_conn.*` + `protocol/srs_protocol_conn.hpp` | `SrsResourceManager` reaper | 268 |
| `src/app/srs_app_server.{hpp,cpp}` | `app/srs_app_server.*` (1,628줄) | accept→conn 생성만 (S10: HTTP 리스너 추가) | 157 |
| `src/app/srs_app_rtmp_conn.{hpp,cpp}` | `app/srs_app_rtmp_conn.*` (1,679줄) | 수명주기 + publish/play 루프 | 714 |
| `src/app/srs_app_source.{hpp,cpp}` | `app/srs_app_source.*` (2,812줄) | Source/Consumer/GopCache/MetaCache/Jitter/Queue + `SrsOriginHub`(S10, HLS만) | 1,586 |
| `src/app/srs_app_fragment.{hpp,cpp}` | `app/srs_app_fragment.*` | 세그먼트 수명주기 + 롤링 윈도우 (S10) | 314 |
| `src/app/srs_app_hls.{hpp,cpp}` | `app/srs_app_hls.*` (1,900줄) | 암호화/ts_floor/훅 제거 (S10, §5.6) | 963 |
| `src/app/srs_app_http_conn.{hpp,cpp}` | `app/srs_app_http_conn.*` + `srs_app_http_static.*` + `protocol/srs_protocol_http_stack.*` | GET 전용 손 파싱 HTTP (S10, §5.6) | 324 |
| `src/app/srs_app_config.{hpp,cpp}` | `app/srs_app_config.*` (10,138줄) | 상수 구조체 (S10: hls_*/http 추가) | 66 |
| `src/main/srs_main_server.cpp` | `main/srs_main_server.cpp` | main() | 57 |
| `utest/` | `utest/` | gtest + MockBufferIO. 동일 설계 | 4,396 |

---

## 4. 데이터 흐름 (핵심 두 경로)

### 4.1 Publish 경로 (OBS → 서버)

```text
TCP accept (SrsTcpListener)
 → new SrsRtmpConn + 전용 스레드 시작
 → do_cycle: handshake → connect_app (tcUrl 파싱)
 → service_cycle: WindowAckSize/SetPeerBW/SetChunkSize/_result 송신, 이후 재-publish 루프
 → stream_service_cycle: identify_client → FMLEPublish 판별
 → start_fmle_publish: FCPublish/createStream/publish에 각각 _result 응답
 → SrsLiveSourceManager::fetch_or_create("vhost/app/stream") → SrsLiveSource
 → publishing 루프: recv_message
     ├ audio  → source->on_audio(msg)
     ├ video  → source->on_video(msg)
     └ AMF0 data(onMetaData) → source->on_meta_data(msg)
```

`SrsLiveSource::on_video()` 내부 (audio도 대칭):

```text
시퀀스 헤더(SPS/PPS)면 → SrsMetaCache에 저장 (GOP 캐시에는 넣지 않음)
모든 consumer에 enqueue  ← 팬아웃 지점 (SrsSharedPtrMessage::copy = refcount만 증가, 페이로드 무복사)
SrsGopCache::cache(msg)  ← 키프레임이 오면 clear 후 새로 시작 → 항상 [마지막 키프레임..현재] 유지
```

### 4.2 Play 경로 (서버 → ffplay)

```text
identify_client → Play 판별 → start_play: StreamBegin + onStatus 2종 + |RtmpSampleAccess
 → source->create_consumer(consumer)
 → source->consumer_dumps(consumer)   ★ 송신 루프 시작 전에 반드시 실행
     ① MetaCache: onMetaData → AAC 시퀀스 헤더 → AVC 시퀀스 헤더(SPS/PPS) 순서로 주입
     ② GopCache: 마지막 키프레임부터의 GOP 전체를 재생 큐에 주입
 → playing 루프: consumer->wait() → dump_packets → send_and_free_messages
```

**세 캐시가 필수인 이유** (중간 입장 플레이어의 정합성):

- **onMetaData** 없으면 플레이어가 해상도/코덱을 모름
- **SPS/PPS(AVC 시퀀스 헤더)** 없으면 디코더 초기화 불가 → 이후 모든 NALU 디코딩 불가. 라이브 스트림에서 publish 직후 딱 한 번만 오므로 캐시 없이는 늦게 온 플레이어는 영원히 못 받음
- **GOP 캐시** 없으면 다음 키프레임까지 검은 화면 (GOP 2초면 최대 2초 블랙)

**지터 보정 (`SrsRtmpJitter`)**: consumer마다 개별 소유. 입력 타임스탬프의 절대값을 믿지 않고 **위생 처리한 델타를 누적**해 0부터 시작하는 출력 타임라인을 재구성. 델타가 비정상(±250ms 초과 등)이면 10ms로 클램프. GOP 캐시 재생분(과거 타임스탬프)이 새 플레이어에게 0 기반의 깨끗한 타임라인으로 나가는 이유.

**느린 소비자 정책 (`SrsMessageQueue::shrink`)**: 큐가 넘치면 오래된 N개를 버리는 게 아니라 **전체를 비우고 최신 시퀀스 헤더만 재주입** — 뒤처진 플레이어를 라이브 시점으로 스냅.

### 4.3 HLS 경로 (S10: publish → Safari/hls.js)

RTMP 팬아웃(§4.1)의 `on_audio_imp`/`on_video_imp`에서 consumer 팬아웃 **앞에** `SrsOriginHub`로 분기한다:

```text
SrsLiveSource::on_video_imp
 → SrsOriginHub::on_video
     ├ SrsFormat::on_video        코덱 파싱 — 시퀀스 헤더면 avcC→SPS/PPS 저장,
     │                            아니면 AVCC 길이 프리픽스 → NALU 샘플 목록
     └ SrsHls::on_video           시퀀스 헤더면 마킹만(세그먼트에 안 씀), 아니면 ↓
        → SrsHlsController::write_video(frame, dts*90)
            ├ SrsTsMessageCache::cache_video   NALU들 → annex-b 변환
            │    (AUD 삽입 + IDR 앞에 캐시된 SPS/PPS 재삽입 — §4.2의 세 캐시와 같은 원리:
            │     TS 세그먼트는 중간부터 틀어도 디코딩 가능해야 한다)
            ├ 세그먼트 컷 판단: duration ≥ hls_fragment && 키프레임 도착
            │    → segment_close(임시파일 rename + m3u8 재작성 + 윈도우 shrink)
            │    → segment_open(새 ts, PAT/PMT부터 다시)
            └ SrsHlsMuxer::flush_video → SrsTsContextWriter → SrsTsContext::encode
                 PES 헤더(PTS/DTS 90kHz) + 188바이트 TS 패킷 분할 + 키프레임 PCR
```

플레이어는 `SrsHttpConn`(GET 전용 정적 서버, :8080)이 서빙하는 `hls_path`의 m3u8/ts를 폴링한다.

**RTMP play와의 지연 차이가 곧 교육 포인트다**: RTMP는 프레임 단위 push(1초 미만), HLS는
세그먼트(10초) 단위 pull — 같은 스트림 허브에서 갈라진 두 소비 모델을 나란히 관찰할 수 있다.

---

## 5. 주요 설계 결정

### 5.1 동시성: ST 코루틴 → pthread (1 connection = 1 thread)

원본 SRS는 state-threads(ST) 코루틴으로 단일 OS 스레드에서 수만 연결을 처리한다 (setjmp/longjmp, 락 불필요).
교육용에서는 **`SrsCoroutine` / `ISrsCoroutineHandler::cycle()` / `pull()` 인터페이스는 원본 그대로 유지**하되 구현만 pthread로 바꾼다:

| 원본 ST | srs_simple |
| --- | --- |
| `st_thread_create` | `pthread_create` |
| `st_cond_*` | `pthread_cond_*` |
| 타임아웃 인자 per-call | `SO_RCVTIMEO`/`SO_SNDTIMEO` |
| `st_thread_interrupt` (블록된 IO 즉시 깨움) | interrupted 플래그 설정 → 소켓/accept 타임아웃으로 깨어난 워커가 `pull()`에서 발견 |
| 락 불필요 (단일 스레드) | `SrsLiveSource` 내부에 `std::mutex` 필요 |

이렇게 하면 "각 연결은 `cycle()`을 실행하는 코루틴이며 `pull()`로 협조적 취소를 확인한다"는 SRS의 핵심 구조가 코드에 그대로 남고, 원본을 읽을 때 ST 호출만 치환해서 읽으면 된다.

원본은 연결당 코루틴 2개(메인 + 수신 전용 `SrsRecvThread`)를 쓰지만(성능 이슈 #217/#237), 우리는 **연결당 1스레드 + recv 타임아웃**으로 단순화한다. publish 쪽 워치독 루프도 생략.

### 5.2 두 개의 메시지 클래스는 유지

- `SrsCommonMessage` — 수신 측. 청크 재조립기가 만들고 단일 소유
- `SrsSharedPtrMessage` — 송신 측. refcount 페이로드 공유. 1MB 키프레임을 1,000명에게 팬아웃해도 페이로드 복사 0회

이 구분은 SRS 이해에 핵심적이므로 단순화하지 않는다.

### 5.3 원본이 지키는 미묘한 규칙들 — 그대로 보존

1. **자기 코루틴에서 `delete this` 금지**: 연결 종료 시 `SrsResourceManager`(우리는 단순화된 reaper)가 다른 스레드에서 해제. 이 아키텍처의 고전적 use-after-free 지점
2. **`consumer_dumps`는 송신 루프 시작 전에**: 순서가 바뀌면 새 플레이어의 첫 바이트가 SPS/PPS 없는 GOP 중간이 됨
3. **SetChunkSize는 connect `_result`보다 먼저** (128바이트 넘는 응답 전에)
4. **AMF0 Object 프로퍼티 순서 보존**
5. **onStatus(NetStream.Publish.Start)는 인가 통과 후에** 송신 (OBS 재연결 루프 방지)
6. **시퀀스 헤더는 GOP 캐시에 넣지 않고 MetaCache에만** (중복/유실 방지)

### 5.4 설정: `SrsConfig` → 상수 구조체

```cpp
struct SrsSimpleConfig {
    int  listen_port            = 1935;
    int  chunk_size             = 60000;    // connect 응답 전에 송신
    int  window_ack_size        = 2500000;
    int  peer_bandwidth         = 2500000;
    bool gop_cache              = true;
    int  gop_cache_max_frames   = 2500;
    srs_utime_t queue_length    = 30 * SRS_UTIME_SECONDS;  // 소비자 큐 최대 길이
    int  mw_msgs                = 128;      // 1회 writev 최대 메시지 수

    // S10 HLS (원본 vhost.hls.*의 기본값. 단 hls_enabled는 데모 편의로 on — §5.6 S10)
    bool hls_enabled            = true;
    srs_utime_t hls_fragment    = 10 * SRS_UTIME_SECONDS;  // 세그먼트 길이
    srs_utime_t hls_window      = 60 * SRS_UTIME_SECONDS;  // m3u8 롤링 윈도우
    double hls_aof_ratio        = 2.1;      // pure-audio 강제 컷 배율
    bool hls_wait_keyframe      = true;     // 키프레임에서만 컷
    bool hls_cleanup            = true;     // 만료 ts 삭제
    std::string hls_path        = "./objs/hls";
    std::string hls_m3u8_file   = "[app]/[stream].m3u8";
    std::string hls_ts_file     = "[app]/[stream]-[seq].ts";
    int  http_listen_port       = 8080;     // HLS 파일 서빙용 HTTP (원본 http_server.listen)
    std::string http_dir        = "./www";  // 정적 페이지 루트 ("/" → HLS 플레이어 index.html)
};
```

### 5.5 테스트: 원본의 최고 아이디어를 그대로 복사

프로토콜 전체가 `ISrsProtocolReadWriter` 인터페이스에만 의존하므로 **소켓 없이 테스트 가능**하다.
원본 `utest/`의 설계를 그대로 가져온다:

- `MockBufferIO` — 두 개의 메모리 버퍼로 소켓 흉내 (`in_buffer`에 손으로 쓴 청크 hex를 넣고 `recv_message` 결과를 검증). 위치: `utest/srs_utest_protocol.hpp`
- `MockRtmpClient` — 실소켓 + `SrsProtocol`로 핸드셰이크/connect/publish/play를 수행하는 통합 테스트용 클라이언트. 위치: `utest/srs_utest_server.cpp`
- `HELPER_EXPECT_SUCCESS(x)` — `srs_error_t`를 검사하고 실패 시 `srs_error_desc` 출력
- gtest 사용, `#define private public` 트릭으로 내부 상태 검증. `<mutex>` 등 표준 헤더는 이 매크로보다 **먼저** 포함해야 한다 (`utest/srs_utest.hpp` 상단)

### 5.6 원본과의 차이 — 전체 목록

**이 절이 원본 대비 모든 의도적 차이의 단일 출처다.** 코드 주석은 여기를 가리키기만 한다.

#### pthread 전환에 따른 스레드 안전성 조정

원본은 ST 단일 OS 스레드 가정이라 안전한 공유 버퍼들이 pthread 모델에서는 데이터 레이스가 된다. 이름과 구조는 유지하되 다음만 바꿨다:

| 지점 | 원본 (ST 단일 스레드) | srs_simple (pthread) |
| --- | --- | --- |
| `SrsCplxError::create/wrap`의 포맷 버퍼 | static 4MB 재사용 | 스택 버퍼 4KB |
| `SrsConsoleLog::log`의 라인 버퍼 | 멤버 변수 재사용 | 스택 버퍼 |
| `SrsCplxError::error_code_str`의 이름 맵 | 호출 시 lazy 채움 | C++11 magic static 초기화 |
| `SrsThreadContext`의 cid 저장 | `map<srs_thread_t, SrsContextId>` | `thread_local` 변수 |
| `SrsSTCoroutine`의 상태(`trd_err` 등) | 락 없음 (단일 스레드) | 내부 `std::mutex` (워커/외부 스레드가 공유) |
| `SrsTcpListener`의 accept 대기 | 블로킹 accept, interrupt가 깨움 | `poll(100ms)` 타임아웃 후 accept — macOS는 accept가 `SO_RCVTIMEO`를 존중하지 않으므로 poll 사용. `close()` 지연 상한 100ms |

#### core/kernel (S1)

`SrsContextId`는 `_SrsContextId` 래퍼 대신 `std::string` typedef(원본 주석이 허용하는 형태), context id는 8자 랜덤 대신 증가 번호(`cid-N`), 에러코드는 X-macro 대신 평범한 enum(값은 원본과 동일), `srs_error_desc`에서 원본의 `error_code_longstr` 프레임은 생략. `_srs_log`/`_srs_context` 전역은 `srs_kernel_log.cpp`에서 정의한다 (원본은 main에서 정의). `SrsConsoleLog`는 라인마다 `fflush` — 로그를 파일로 리다이렉트할 때 필요.

#### I/O·스레드 (S2)

`SrsSTCoroutine`은 원본의 `SrsFastCoroutine` 위임 구조(성능 분리) 없이 pthread를 직접 구현, `srs_netfd_t`는 ST netfd 포인터 대신 `int` typedef, `srs_tcp_listen`은 IPv4 전용(getaddrinfo 제거)이며 위치는 `srs_app_listener.cpp`(원본은 `protocol/srs_protocol_st.cpp`), `SrsTcpListener::listen()`은 utest용으로 port 0(임시 포트) 허용, `SrsFastStream`은 merged-read와 `set_buffer` 리사이즈 제거. `srs_protocol_io.hpp`는 원본 `kernel/srs_kernel_io.hpp`를 병합 — `ISrsWriter`가 원본의 `ISrsStreamWriter`(write)+`ISrsVectorWriter`(writev) 역할.

#### 메시지 모델·핸드셰이크 (S3)

- `srs_chunk_header_c0/c3`는 원본 `kernel/srs_kernel_utility.cpp:1184/1259`에 있으나, utility 파일을 만들지 않으므로 `srs_kernel_flv.{hpp,cpp}`에 병합. `SRS_CONSTS_RTMP_MAX_FMT0/3_HEADER_SIZE`(원본 `srs_kernel_consts.hpp`)도 같은 이유로 `srs_kernel_flv.hpp`에 정의
- `SrsHandshakeBytes`는 원본 `rtmp_stack.{hpp,cpp}` 소속이나 rtmp_stack보다 먼저 필요하므로 `srs_protocol_rtmp_handshake.{hpp,cpp}`로 이동. 서버 역할만 유지 — 클라이언트 측(`create_c0c1/create_c2/read_s0s1s2`, `handshake_with_server`)과 RTMP 프록시(`proxy_real_ip`) 제거
- `srs_random_generate`(원본 utility)는 handshake cpp의 파일-로컬 static으로 축소
- `SrsSharedPtrMessage`: 원본의 `copy2()`(헤더 없는 복사, RTC 경로용)를 `copy()`에 병합, `_srs_pps_objs_msgs` 통계 카운터 제거
- `srs_kernel_codec`: 레거시 FLV 헤더만 판별 (enhanced-RTMP ext header/HEVC 분기 제거), enum은 필요한 값만 유지(이름/값은 원본과 동일)
- 핸드셰이크는 `SrsHandshakeBytes`를 호출자(`SrsRtmpServer::handshake`)가 소유하는 구조 — 원본은 성공 후 `dispose()`로 3KB 반납

#### AMF0 (S4)

- `SrsAmf0StrictArray`/`SrsAmf0Date` 클래스와 관련 판별자·변환자(`is_strict_array/is_date/to_date/to_date_time_zone` 등) 제거. 제거된 마커를 만나면 `discovery`가 `ERROR_RTMP_AMF0_INVALID` 반환 (원본은 StrictArray/Date도 파싱)
- `is_complex_object`는 object/object-eof/ecma-array만 검사 (원본은 strict-array 포함)
- `human_print`(디버그 덤프)와 `to_json`(SrsJsonAny 변환) 제거 — JSON 계층 자체가 없음
- 마커 `#define`은 원본 전체 목록(0x00~0x11)을 주석 겸 유지, 클래스만 서브셋
- 나머지(클래스/메서드 이름, `SrsUnSortedHashtable`의 삽입 순서 보존 vector, discovery→read 2단 프로토콜, utf8 len≤0 처리, EOF 없이 끝나는 Object를 허용하는 관용)는 원본과 동일

#### 청크 스택 (S5) — 성능 최적화 제거, 파싱 로직은 원본과 동일

- `cs_cache`(`SRS_PERF_CHUNK_STREAM_CACHE`, cid<16 배열 캐시) 제거 — `chunk_streams` map만 사용
- iovec 배칭(`SRS_PERF_COMPLEX_SEND`/`do_iovs_send`/`srs_write_large_iovs`) 제거 — `do_send_messages`는 청크마다 (c0/c3 헤더, 페이로드) 2-iov `writev` 1회. `out_iovs`는 `iovec[2]`, `out_c0c3_caches`는 헤더 1개 크기(16B) 고정 멤버
- `set_auto_response`/`manual_response_queue` 제거 — 항상 자동 응답 (원본은 수신 전용 코루틴이 송신 경합을 피하려고 응답을 큐잉하지만, 연결당 1스레드에서는 불필요)
- 청크 상수(`SRS_CONSTS_RTMP_PROTOCOL_CHUNK_SIZE` 등, 원본 `srs_kernel_consts.hpp`)는 rtmp_stack.hpp에, `RTMP_FMT_TYPE0~3`과 `srs_min`은 원본과 동일하게 cpp 파일-로컬로 정의
- `SrsUniquePtr` 없이 명시적 `srs_freep` (원본의 최신 스타일 대신 수동 해제 — 프로젝트 전체 이디엄과 일치)
- extended timestamp 미반복(ffmpeg 스타일) 감지는 4바이트를 미리 읽고 값이 다르면 `skip(-4)`

#### 패킷·RtmpServer (S6) — 서버 역할 서브셋

- `SrsRtmpClient`(클라이언트 역할 전체) 제거. 단 `SrsProtocol`의 `requests` 맵(tid→커맨드 이름)과 `_result`/`_error` 디코드 경로는 유지 — utest가 클라이언트 역할로 서버 응답을 검증하는 데 사용
- 패킷 서브셋: 커맨드 12종(§2.5) + `SrsSampleAccessPacket`만. `SrsCallPacket(+Res)`/`SrsCloseStreamPacket`/`SrsPausePacket`/`SrsPlayResPacket`/`SrsOnBWDonePacket`/`SrsOnStatusDataPacket` 제거 — 미지의 커맨드는 기본 `SrsPacket`으로 드롭 (원본은 `SrsCallPacket`으로 받아 null 응답, 예: Haivision의 `_checkbw`). ffmpeg의 `getStreamLength`도 이 경로로 무시되며 진행에 문제 없음
- `SrsRtmpServer` 메서드 서브셋: `proxy_real_ip`/`set_auto_response`/`redirect`/`response_connect_reject`/`on_bw_done`/`on_play_client_pause`/`start_haivision_publish`/`start_flash_publish` 제거. `handshake()`는 심플만 (원본은 복잡 시도 → `ERROR_RTMP_TRY_SIMPLE_HS` → 심플 폴백)
- `srs_discovery_tc_url`: 원본은 `SrsHttpUri`(HTTP 스택)로 URL 파싱 — HTTP 계층이 없으므로 같은 시그니처로 손 파싱 (`srs_protocol_utility.{hpp,cpp}`). `srs_guess_stream_by_app`/`srs_generate_tc_url`/`srs_parse_query_string` 제거, 문자열 헬퍼(`srs_string_replace/remove/trim_*`, 원본 kernel_utility 소속)는 protocol_utility로 병합
- `SrsRequest`: `update_auth`/`as_http`/`protocol`/`ice_ufrag_`/`ice_pwd_` 제거 (RTC/HTTP 경로 전용)
- `SrsRtmpConnType`: HLS/FLV/RTC/SRT/Haivision 값 제거 (남긴 이름/값은 원본과 동일)
- 상수 배치: `SRS_CONSTS_RTMP_DEFAULT_PORT/VHOST/APP`, `SRS_CONSTS_RTMP_SET_DATAFRAME/ON_METADATA`(원본 kernel_consts)를 rtmp_stack.hpp에 정의 (S3/S5와 같은 이유 — consts 파일 없음)
- `response_connect_app`의 data EcmaArray 필드는 서브셋 (version/srs_sig/srs_server/srs_version/srs_server_ip/srs_pid/srs_id만 — license/url/authors 제거)

#### 연결 수명주기 (S7)

- `SrsRtmpConn`: edge/refer/security/bandwidth/http_hooks/statistic/APM/reload 관련 멤버·메서드 전부 제거. `on_disconnect`(훅 전용)와 `ISrsExpire::expire`(HTTP-API kick-off 전용)도 제거
- **수신 전용 코루틴 제거**: 원본은 play에 `SrsQueueRecvThread`, publish에 `SrsPublishRecvThread`를 붙여 연결당 코루틴 2개를 쓴다(#217/#237). 여기서는 연결당 1스레드가 직접 `recv_message` (§5.1의 계획대로). publish 타임아웃 세분화(1stpkt/normal/kickoff_for_idle)도 함께 제거 — `SO_RCVTIMEO` 30초 고정
- `~SrsRtmpConn`은 `shutdown(fd, SHUT_RDWR)`로 블록된 recv를 깨운 뒤 join한다 — ST는 interrupt가 블록된 IO를 즉시 깨우지만 pthread는 못 하므로 (§5.1). 소멸자 외에는 소켓 타임아웃이 `pull()` 확인 지점
- pithy print는 5초 간격 recv/send KB 로그로 축소
- `SrsRtmpConnFlashPublish` 분기는 `start_flash_publish`(S6에서 제거) 없이 `publishing` 안의 `start_publishing`이 같은 onStatus(Publish.Start)를 송신
- `SrsResourceManager`: 원본의 id/fast-id/name 인덱스 맵, level-0 캐시, disposing 핸들러 구독(subscribe/unsubscribe)을 제거하고 conns_/zombies_ 두 벡터만 유지. ST cond → `std::mutex` + `std::condition_variable`(100ms 타임아웃 부 대기 — 리스너 poll과 같은 이디엄, §5.1). 소멸 중 `remove()`는 `disposing_` 플래그로 무시 (원본의 `p_disposing_` 검사에 대응). "자기 스레드에서 delete this 금지" 규칙(§5.3)은 그대로 보존
- `ISrsResource`/`ISrsResourceManager`/`ISrsConnection`은 원본 `protocol/srs_protocol_conn.hpp` 소속이나, protocol 연결 파일을 만들지 않으므로 `srs_app_conn.hpp`로 병합
- `SrsServer`: HTTP API/서버, signal/pid/hourglass/ingester/reload/통계 제거 — RTMP 리스너 1개 + conn_manager + `on_tcp_client`만. `SrsRtmpConn`의 `manager = svr`(서버가 ISrsResourceManager 구현) 구조는 원본 그대로
- `srs_app_config`: §5.4의 상수 구조체. 전역 이름 `_srs_config`는 원본과 동일, `get_xxx(vhost)` 호출은 멤버 접근으로 대체
- `srs_get_peer_ip/port`/`srs_get_local_ip`는 원본과 동일하게 `srs_protocol_utility`에 두되 IPv4 전용 (getnameinfo → getpeername/getsockname+inet_ntop)
- `srs_main_server.cpp`: st_init/데몬화/시그널 관리자 없음. **`SIGPIPE` 무시 필수**(ST가 대신 해주던 것 — 끊긴 소켓 write에 프로세스가 죽지 않게), 메인 스레드는 `pause()` 대기. utest도 서버 테스트 첫머리에서 같은 처리를 한다

#### 스트림 허브 (S8)

- `SrsLiveSource`에서 edge(`SrsPlayEdge`/`SrsPublishEdge`)/`SrsOriginHub`(HLS·DVR·Forward·Transcode)/bridge/`SrsRtmpFormat`(코덱 파싱) 제거. 시퀀스 헤더 판별은 `format_->is_aac_sequence_header()` 대신 kernel codec의 `SrsFlvAudio::sh`/`SrsFlvVideo::sh` 직접 호출
- mix_correct/atc 제거(§1)에 따라 `on_frame`/`SrsMixQueue` 없이 `on_audio→on_audio_imp` 직행, `enqueue`/`dumps`류 시그니처에서 `atc` 인자 제거. `SrsFlvVideo::acceptable`(비정상 헤더 드롭) 검사도 생략 — GopCache의 h264 검사가 비 h264를 걸러냄
- `SrsLiveSourceManager`: 원본의 `SrsSharedPtr<SrsLiveSource>` + hourglass 타이머(죽은 소스 3초 후 청소) 대신 raw 포인터 + 프로세스 종료까지 유지 (`stream_is_dead`/`cycle`/`dispose` 제거). 교육용에서 소스 수는 유한하므로 수용. 전역 `_srs_sources`는 `_srs_config`와 같은 정적 초기화 이디엄
- `ISrsLiveSourceHandler`(HTTP-FLV 마운트 콜백) 제거 — `fetch_or_create(req, &source)` 시그니처에서 handler 인자 삭제
- `SrsMetaCache`: `previous_video/previous_audio`(reduce_sequence_header 전용)와 `SrsRtmpFormat` 멤버 제거. `drop_for_reduce` 경로 전체 제거 (기본 설정 off)
- pthread 안전성(§5.1): `SrsLiveSource`에 `std::mutex`(consumers/캐시/can_publish_ 보호), `SrsLiveConsumer`에 `std::mutex` + `std::condition_variable`(원본 `SRS_PERF_QUEUE_COND_WAIT`의 st_cond 대체). **락 순서는 항상 source → consumer**
- publish 점유 원자화: 원본은 conn의 `acquire_publish`가 `can_publish()` 검사 후 `on_publish()`를 호출 (ST 단일 스레드라 원자적). pthread에서는 `on_publish()` 내부에서 검사+점유를 원자적으로 수행 — 이미 publish 중이면 `ERROR_SYSTEM_STREAM_BUSY`
- `SrsLiveConsumer::wait`: 원본은 enqueue가 깨울 때까지 무한 st_cond 대기(수신 코루틴이 별도 존재) — 여기서는 100ms 타임아웃 부 cond wait로 깨어나 호출자가 `pull()`/컨트롤 메시지를 재확인 (리스너 poll과 같은 이디엄). `ISrsWakable`/`wakeup()`/pause(`on_play_client_pause`) 제거
- `SrsRtmpConn::do_playing`: 원본의 수신 전용 코루틴(`SrsQueueRecvThread`) 대신 루프 선두에서 0ms `poll()`로 소켓이 읽을 수 있을 때만 `recv_message` — 플레이어의 컨트롤 메시지 소비와 종료(FIN) 감지를 겸한다. `SrsMessageArray`는 원본 파일명대로 `srs_protocol_rtmp_msg_array.{hpp,cpp}` 추가. duration 제한 재생/send_min_interval/mw 소켓 버퍼 튜닝 제거
- `SrsGopCache`는 H.265 분기만 제거하고 원본 전체 유지 (pure audio 가드 `SRS_PURE_AUDIO_GUESS_COUNT=115` 포함). `SrsRtmpJitter`/`SrsMessageQueue`(shrink 정책 포함)는 원본 알고리즘 그대로
- play 경로의 컨트롤 메시지(closeStream/pause)는 패킷 클래스를 제거했으므로 드롭만 한다

#### HLS·HTTP (S10) — 2026-08-21 추가

§1 비목표였던 HLS를 목표로 승격해 구현했다. RTMP publish → TS 세그먼트 + m3u8 + HTTP 서빙.
먼저 레이어 재배치 두 건:

- `kernel/srs_kernel_io.hpp` **복원**: S1~S9에서는 protocol_io에 병합했으나, kernel 소비자(`SrsFileWriter`, `SrsTsContext`)가 생겨 원본 위치로 되돌렸다. `srs_protocol_io.hpp`는 이제 kernel_io를 include
- `srs_crc32_mpegts`(원본 kernel_utility)는 `srs_kernel_ts.hpp`로, `srs_path_exists`/`srs_create_dir_recursively`는 `srs_kernel_file.hpp`로, `srs_path_build_stream`은 `srs_protocol_utility.hpp`로 병합 (utility 파일 없음 — S3/S5/S6과 같은 이유)

#### 코덱 파싱 (S10)

- `SrsFormat`을 kernel_codec에 추가하되 HLS가 소비하는 것만: avcC→SPS/PPS(`avc_demux_sps_pps`), AVCC→NALU 샘플(`avc_demux_ibmf_format` — 원본의 `do_` 접두사 없이), ASC→object/rate/channels(`audio_aac_sequence_header_demux`). SPS 비트스트림 파싱(해상도/fps), HEVC/AV1, MP3/Opus, `SrsRtmpFormat` 파생 제거
- 원본은 `SrsFrame::initialize(SrsCodecConfig*)`로 코덱을 주입 — 여기서는 `SrsFormat`이 `audio->acodec_`/`video->vcodec_` 필드에 직접 대입 (접근자 `acodec()`/`vcodec()`는 원본과 동일)
- 비AAC/비H.264는 에러가 아니라 id만 기록하고 성공 리턴 — 호출자(HLS)가 id를 보고 드롭
- ffmpeg가 스트림 끝에 보내는 end-of-sequence(`SrsVideoAvcFrameTraitSequenceHeaderEOF`)는 샘플 0개로 파싱되고, `SrsHls`가 NALU 아닌 패킷과 빈 프레임을 걸러 AUD만 있는 PES가 TS에 나가지 않게 한다 (안 거르면 디코더가 "missing picture" 오류)

#### TS 먹서 (S10)

- **디코더(demux) 경로 전체 제거** — HLS 쓰기 전용. 원본 5,900줄 → 766줄
- 원본의 `SrsTsPacket`/`SrsTsHeader`/`SrsTsAdaptationField`/`SrsTsPayloadPAT/PMT` 클래스 트리 제거 — `SrsTsContext::encode_pat_pmt`/`encode_pes`가 188바이트 버퍼를 직접 조립한다. **바이트 레이아웃은 원본과 동일** (PAT/PMT 섹션 + CRC32-MPEG2, PES 33비트 PTS/DTS, 키프레임 첫 패킷의 PCR+random_access, 0xFF 스터핑 AF)
- 고정 PID(PAT 0/PMT 0x1001/video 0x100/audio 0x101)와 stream_type(0x1b/0x0f)은 원본 상수 그대로. continuity counter는 pid별로 세그먼트를 넘어 이어진다
- `SrsTsMessageCache::do_cache_aac`(ADTS 7바이트 생성)/`do_cache_avc`(annex-b 변환 + AUD 삽입 + IDR 앞 SPS/PPS 재삽입)는 원본 알고리즘 그대로. 원본의 pure-audio 프레임 집계(`SRS_CONSTS_HLS_PURE_AUDIO_AGGREGATE`) 제거 — 프레임마다 flush
- pure-audio에서 PCR을 audio가 싣는 규칙은 context의 vcodec 판단으로 단순화

#### HLS 앱 계층 (S10)

- `SrsHls`/`SrsHlsController`/`SrsHlsMuxer`/`SrsHlsSegment`/`SrsFragment`/`SrsFragmentWindow` — 세그먼트 컷 판단(`is_segment_overflow` + `wait_keyframe`, pure-audio는 `is_segment_absolutely_overflow`), 임시파일→rename, m3u8 원자적 교체(temp+rename), 100ms 미만 세그먼트 드롭, reap 시 video 먼저 flush(iPhone 호환)는 전부 원본 알고리즘 그대로
- 제거: AES-128 암호화(`hls_keys`), `hls_ts_floor`(타임스탬프 양자화), `hls_entry_prefix`, on_hls/on_hls_notify 훅(`SrsDvrAsyncCallOnHls*`), async reload/dispose 타이머, `hls_td_ratio`(=1.0 고정)
- **오디오 dts는 `timestamp * 90` 직접 사용** — 원본 기본은 AAC 샘플 수 누적으로 재구성(이슈 #547)하지만, 원본에도 있는 `hls_dts_directly`(이슈 #1506) 방식으로 고정
- HLS 오류 전략은 원본 `hls_on_error`의 **'ignore'로 고정** — `SrsOriginHub`가 경고 로그 + `hls->on_unpublish()` 후 삼킨다. HLS 오류가 RTMP publish를 죽이지 않는다 (원본 기본값은 'continue')
- `SrsOriginHub`는 HLS만 유지 (DVR/Forward/Transcode/HDS 제거). hub는 source의 `lock_` 안에서 실행되므로 세그먼트 파일 IO도 락 안에서 일어난다 — 교육용 수용 (원본은 ST 단일 스레드라 같은 코루틴에서 실행)
- 알려진 한계: 오디오 전용 스트림은 PMT가 기본값(H.264)을 광고하고 PCR이 없다 — 원본도 `hls_vcodec vn` 설정 없이는 동일. 세그먼트는 `hls_aof_ratio`(21초)에서 잘린다

#### HTTP 정적 서버 (S10)

- 원본의 HTTP 스택(`SrsHttpServeMux`/`SrsHttpMessage`/`SrsHttpParser` + http-parser 라이브러리, `SrsHttpStaticServer`/`SrsVodStream`) 전체를 `SrsHttpConn` 하나의 손 파싱으로 대체 — GET 전용, 요청 1개 처리 후 `Connection: close`, `..` 경로 탈출 차단, CORS(`Access-Control-Allow-Origin: *`) 허용
- MIME 타입(.m3u8 → `application/vnd.apple.mpegurl`, .ts → `video/MP2T`)은 원본 `_mime` 맵과 동일
- `SrsServer`가 HTTP 리스너를 하나 더 열고 `do_on_tcp_client`에서 리스너로 분기 — 원본 구조 그대로. RTMP/HTTP 연결이 같은 `SrsResourceManager`를 쓴다
- 정적 페이지: `"/"`는 `http_dir`(기본 `./www`)의 `index.html` — hls.js/Safari 네이티브 HLS 플레이어 페이지. 경로 조회는 hls_path 우선, 없으면 http_dir (원본은 `research/players/`를 `http_server.dir`에 설치하는 방식)
- 설정: `hls_enabled` 기본 **on**(원본은 off — 설정 파일이 없으므로 데모 편의), `hls_path`는 `./objs/hls`(원본 `./objs/nginx/html`)

---

## 6. 빌드

- **CMake + C++11** (원본은 bash configure 스크립트 — 학습 가치가 없어 교체)
- 의존성: pthread만. OpenSSL 불필요(복잡 핸드셰이크 제거 덕분)
- 타깃: `srs_simple`(서버), `srs_utest`(gtest, FetchContent로 수급)
- **gtest는 release-1.12.1 고정 — C++11을 지원하는 마지막 릴리스이므로 업그레이드 금지**
- `src/{core,kernel,protocol,app}/*.cpp`는 GLOB으로 `srs_simple_lib`에 자동 포함 — 파일만 추가하면 빌드에 들어간다
- 명령: `cmake -S . -B build && cmake --build build -j8`, 테스트 `./build/srs_utest`, 실행 `./build/srs_simple`
- 검증 도구: `ffmpeg -re -i x.mp4 -c copy -f flv rtmp://localhost/live/test` → `ffplay rtmp://localhost/live/test`

---

## 7. 이 코드를 읽은 뒤 원본 SRS를 여는 법 (탐색 가이드)

srs_simple의 각 지점을 이해했다면, 원본에서 아래를 열면 같은 코드가 확장판으로 보인다.
아래 file:line은 원본과 전수 대조해 확인한 값이다 (2026-08-09 재검증 53건 일치, S10 HLS 행은 2026-08-21 대조).

| 개념 | srs_simple | 원본 SRS 진입점 (`trunk/src/`) |
| --- | --- | --- |
| 서버 기동 | `srs_main_server.cpp` | `main/srs_main_server.cpp:97` `do_main` (ST 초기화는 `app/srs_app_threads.cpp:572` `srs_st_init`, 나머지는 부가물) |
| accept 루프 | `SrsTcpListener::cycle` | `app/srs_app_listener.cpp:309-340` (listen은 :279) |
| 연결 생성 | `SrsServer::on_tcp_client` | `app/srs_app_server.cpp:1332` `do_on_tcp_client` |
| 연결 수명주기 | `SrsRtmpConn::do_cycle` | `app/srs_app_rtmp_conn.cpp:173` → `service_cycle:396` → `stream_service_cycle:491` → `playing:702` / `publishing:925` |
| 핸드셰이크 | `SrsSimpleHandshake` | `protocol/srs_protocol_rtmp_handshake.cpp:1071` (복잡판은 :1152, 폴백은 rtmp_stack.cpp:2219), `SrsHandshakeBytes`는 rtmp_stack.cpp:1680 |
| 청크 수신 | `SrsProtocol::recv_message` | `protocol/srs_protocol_rtmp_stack.cpp:328` → `recv_interlaced_message:786` → `read_basic_header:883` / `read_message_header:934`(mh_sizes :993, fmt3 delta :1088) / `read_message_payload:1188` |
| 청크 송신 | `SrsProtocol::do_send_messages` | `protocol/srs_protocol_rtmp_stack.cpp:391` (iovec 배칭 있는 확장판), c0/c3 직렬화는 `kernel/srs_kernel_utility.cpp:1184/1259` |
| 패킷 디코드 | `SrsProtocol::do_decode_message` | `protocol/srs_protocol_rtmp_stack.cpp:586` |
| 컨트롤 메시지 반영 | `SrsProtocol::on_recv_message` | `protocol/srs_protocol_rtmp_stack.cpp:1230` (자동 ACK은 :1370) |
| 클라이언트 식별 | `SrsRtmpServer::identify_client` | `protocol/srs_protocol_rtmp_stack.cpp:2440` |
| connect 처리 | `SrsRtmpServer::connect_app` | `protocol/srs_protocol_rtmp_stack.cpp:2244`, 응답은 `response_connect_app:2319` |
| publish 시퀀스 | `SrsRtmpServer::start_fmle_publish` | `protocol/srs_protocol_rtmp_stack.cpp:2647`, onStatus 분리는 `start_publishing:2800` (이슈 #4037) |
| play 시퀀스 | `SrsRtmpServer::start_play` | `protocol/srs_protocol_rtmp_stack.cpp:2519` |
| tcUrl 파싱 | `srs_discovery_tc_url` | `protocol/srs_protocol_utility.cpp:47` (원본은 SrsHttpUri 사용) |
| AMF0 | `SrsAmf0Any::discovery` | `protocol/srs_protocol_amf0.cpp` (`SrsAmf0Object::read:672`/`write:716`, 순서 보존 해시테이블은 hpp:778 주석) |
| 스트림 허브 | `SrsLiveSource::on_video` | `app/srs_app_source.cpp:2408` `on_video_imp` (팬아웃은 :2457) |
| 소비자 큐 | `SrsLiveConsumer::enqueue/wait` | `app/srs_app_source.cpp:450/527`, shrink 정책은 :339 |
| GOP 캐시 | `SrsGopCache::cache/dump` | `app/srs_app_source.cpp:611/686` (키프레임에서 clear는 :653) |
| 메타 캐시 | `SrsMetaCache::dumps` | `app/srs_app_source.cpp:1626` (audio를 video보다 먼저 — :1636 주석) |
| 새 플레이어 프리필 | `SrsLiveSource::consumer_dumps` | `app/srs_app_source.cpp:2703` |
| 소스 조회/생성 | `SrsLiveSourceManager::fetch_or_create` | `app/srs_app_source.cpp:1769` |
| 지터 보정 | `SrsRtmpJitter::correct` | `app/srs_app_source.cpp:74-132` |
| 원본 허브 | `SrsOriginHub::on_video` | `app/srs_app_source.cpp:1028` (생성은 :823, on_publish는 :1121 — DVR/Forward 분기가 함께 보인다) |
| HLS 트랜스먹서 | `SrsHls::on_video` | `app/srs_app_hls.cpp:1433` (on_audio는 :1352 — AAC 샘플 수 기반 dts 재구성 포함, on_publish는 :1295) |
| 세그먼트 컷 | `SrsHlsController::write_video` | `app/srs_app_hls.cpp:1065` (write_audio는 :1014, reap_segment는 :1106) |
| 세그먼트 열기/닫기 | `SrsHlsMuxer::segment_open` | `app/srs_app_hls.cpp:386` (ts_floor 분기 포함), `do_segment_close:650` |
| m3u8 생성 | `SrsHlsMuxer::_refresh_m3u8` | `app/srs_app_hls.cpp:786` (원자적 교체는 refresh_m3u8:759) |
| 세그먼트 윈도우 | `SrsFragmentWindow::shrink` | `app/srs_app_fragment.cpp:208` (duration은 append:29, 만료 삭제는 clear_expired:231) |
| TS 먹싱 | `SrsTsContext::encode` | `kernel/srs_kernel_ts.cpp:281` (PAT/PMT는 encode_pat_pmt:369, PES는 encode_pes:430 — 패킷 클래스 트리를 쓰는 확장판) |
| FLV→PES 변환 | `SrsTsMessageCache::do_cache_aac/avc` | `kernel/srs_kernel_ts.cpp:2916/3041` (ADTS 생성과 annex-b 변환) |
| 코덱 파싱 | `SrsFormat::on_video` | `kernel/srs_kernel_codec.cpp:832` (avcC는 avc_demux_sps_pps:2150, NALU는 do_avc_demux_ibmf_format:2621, ASC는 audio_aac_sequence_header_demux:2810) |
| HLS 파일 서빙 | `SrsHttpConn`(축소판) | `app/srs_app_http_static.cpp:391` `SrsVodStream`, 실제 파일 응답은 `protocol/srs_protocol_http_stack.cpp:420` `serve_file` |
| 코루틴 | `SrsCoroutine` (pthread판) | `app/srs_app_st.cpp` `SrsFastCoroutine` (인터럽트는 :274), ST 래퍼는 `protocol/srs_protocol_st.cpp` |
| 에러 체인 | `srs_error_new/wrap` | `kernel/srs_kernel_error.cpp` (`SrsCplxError::description:206`) |
| 프로토콜 테스트 | `MockBufferIO` | `utest/srs_utest_protocol.hpp:52` |

원본에서 srs_simple에 **없는** 것을 만나면 대부분 §1의 제거 목록(설정/훅/통계/Edge/HLS/RTC/성능 최적화)에 해당한다 — 라이브 경로 이해에는 건너뛰어도 된다.

---

## 8. 구현 이력과 검증 상태

구현은 S1~S9 세션(RTMP, 2026-08-08 ~ 2026-08-09)과 S10 세션(HLS, 2026-08-21)으로 진행해 **전부 완료**되었다. 세션 순서가 곧 **코드 읽는 순서**이며 의존 순서다:

```text
S1 core/kernel 기반 → S2 I/O·스레드 → S3 핸드셰이크·메시지 모델 → S4 AMF0
  → S5 청크 스택 → S6 패킷·RtmpServer → S7 연결 수명주기 → S8 Source 허브 → S9 통합 검증·문서
  → S10 HLS (코덱 파싱 → TS 먹서 → 세그먼터/m3u8 → OriginHub 연결 → HTTP 서빙)
```

| 세션 | 내용 | 산출 파일 |
| --- | --- | --- |
| S1 | 에러 체인·로깅·바이트 커서 | `src/core/*`, `src/kernel/srs_kernel_buffer.*` |
| S2 | I/O 추상화·수신 버퍼·스레드 모델·리스너 | `srs_protocol_io.hpp`, `srs_protocol_stream.*`, `srs_app_st.*`, `srs_app_listener.*` |
| S3 | 메시지 모델·핸드셰이크 | `srs_kernel_flv.*`, `srs_kernel_codec.*`, `srs_protocol_rtmp_handshake.*` |
| S4 | AMF0 코덱 | `srs_protocol_amf0.*` |
| S5 | 청크 스트림 스택 (심장 1) | `srs_protocol_rtmp_stack.*`의 `SrsProtocol` |
| S6 | 커맨드 패킷 12종·서버 파사드 | 같은 파일의 `SrsPacket` 서브클래스·`SrsRtmpServer`, `srs_protocol_utility.*` |
| S7 | 연결 수명주기 | `srs_app_rtmp_conn.*`, `srs_app_server.*`, `srs_app_conn.*`, `srs_app_config.*`, `srs_main_server.cpp` |
| S8 | 스트림 허브 (심장 2) | `srs_app_source.*`, `srs_protocol_rtmp_msg_array.*` |
| S9 | 실클라이언트 검증 + 문서 | `README.md` |
| S10 | HLS: 코덱 파싱·TS 먹서·세그먼터·HTTP 서빙 | `srs_kernel_io.hpp`, `srs_kernel_file.*`, `srs_kernel_ts.*`, `srs_kernel_codec.*`(SrsFormat), `srs_app_fragment.*`, `srs_app_hls.*`, `srs_app_http_conn.*`, `srs_app_source.*`(SrsOriginHub) |

### 검증 상태

**유닛테스트 104개 통과** (`./build/srs_utest`). 밀도가 가장 높은 곳은 청크 스택(ProtocolStackTest 16개: 청크 파싱/재조립/extended timestamp 3종/인터리빙/2·3바이트 basic header/프로토콜 위반/컨트롤 반영/송신 라운드트립). S10에서 HLS 경로 11개 추가(`srs_utest_hls.cpp`): CRC32-MPEG2 표준 벡터, avcC/ASC/NALU 파싱, ADTS 헤더 비트필드, annex-b 변환(AUD/SPS/PPS 삽입 위치), PAT/PMT CRC 재계산 일치, PES PTS/PCR 인코딩 라운드트립, 188바이트 분할/스터핑/continuity counter, fragment duration/윈도우 shrink, 컨트롤러 풀 파이프라인(실파일 세그먼트 3개 + m3u8 내용).

**실클라이언트 매트릭스** (macOS Darwin 25, ffmpeg/ffplay 8.1, VLC 3, OBS 31 — 2026-08-09):

- ffmpeg publish × ffplay(A-V 싱크 -0.02s, 드롭 0) / VLC play
- OBS publish — `type=fmle-publish` 식별, OBS의 SetChunkSize 4096 정상 반영, 플레이어 30fps 정확 수신
- 중간 입장 즉시 재생(GOP 캐시 프리필), 동시 다중 플레이어 각 30fps, 플레이어 종료(FIN) 감지 후 publish 지속
- REPUBLISH: publisher `kill -INT`(FCUnpublish) → `retry for republish` → 붙어 있던 플레이어가 재publish 후 이어서 수신
- publisher `kill -9` 후 서버 생존·리소스 정리
- 오디오 전용(`-vn`): 정상 릴레이, GopCache는 pure-audio 가드대로 캐시 안 함
- 장시간: 31분 연속 publish(56,452프레임, speed=1x), RSS 0.9MB 유지, Error 0건, 큐 shrink/GOP overflow 0건

상호운용 버그는 발견되지 않았다 (예상 지점이었던 extended timestamp / SetChunkSize 타이밍 / 오디오 전용 모두 문제 없음).

**S10 HLS 검증** (macOS, ffmpeg/ffprobe 8.1 — 2026-08-21):

- ffmpeg 35초 publish(h264+aac, GOP 2초) → 10초 세그먼트 4개(10.02/10.00/10.00/4.99초) + m3u8 생성, EXT-X-TARGETDURATION=11(올림 규칙)
- **전 세그먼트 ffmpeg 디코딩 오류 0건**, 세그먼트당 비디오 정확히 300프레임(30fps×10s)
- HTTP 서빙: curl/ffprobe가 `http://…/live/livestream.m3u8`에서 h264+aac 스트림 인식, Content-Type/CORS 헤더 정상
- publish 도중 컷 발견 버그 1건 수정: ffmpeg의 AVC end-of-sequence 패킷이 AUD만 있는 PES로 새어나가 마지막 세그먼트에 "missing picture" 디코딩 오류 → NALU 아닌 패킷/빈 프레임 가드 추가 (§5.6 S10)
- RTMP play와 동시 동작 (같은 publish를 RTMP·HLS로 동시 소비)

### 의도적으로 남긴 미구현

- 플레이어 pause 처리 (`SrsPausePacket`을 S6에서 제거)
- closeStream 처리 — play 루프에서 드롭만 한다
- HLS: unpublish 시 `#EXT-X-ENDLIST`를 쓰지 않는다 (원본과 동일 — 라이브 전용 시맨틱). HTTP는 GET만 지원 (HEAD는 405)
