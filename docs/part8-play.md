# RTMP 깊이 읽기 (8) — 커맨드 흐름 (3): play와 중간 입장 문제

> **시리즈 안내**: 이 시리즈는 교육용 RTMP 서버 [srs_simple](../README.md)의 코드를 읽기 위해 필요한
> RTMP 프로토콜 이론을 핸드셰이크부터 모든 패킷까지 정리한다. srs_simple은 원본
> [SRS](https://github.com/ossrs/srs)의 클래스/파일/메서드 이름을 1:1로 미러링하므로,
> 여기서 익힌 내용은 원본 SRS를 읽을 때 그대로 통한다.
>
> **시리즈 목차**
>
> 1. [RTMP 조감도 — 메시지, 청크, 스트림](part1-overview.md)
> 2. [핸드셰이크 — 연결의 첫 3,073+1,536 바이트](part2-handshake.md)
> 3. [청크 스트림 — RTMP의 심장](part3-chunk-stream.md)
> 4. [프로토콜 컨트롤 메시지 5종](part4-control-messages.md)
> 5. [AMF0 — 커맨드의 언어](part5-amf0.md)
> 6. [커맨드 흐름 (1) — connect와 스트림 생성](part6-netconnection.md)
> 7. [커맨드 흐름 (2) — publish와 미디어 메시지](part7-publish.md)
> 8. **커맨드 흐름 (3) — play와 중간 입장 문제** (이 글)
> 9. [(보너스) 서버 내부 — 팬아웃, 캐시, 지터](part9-server-internals.md)

이 글은 Part 7까지 읽었다고 가정한다.

이런 상황에서 시작하자. `./publish.sh`로 방송을 켠 지 30초가 지났다. onMetaData는
방송 시작 직후 한 번 지나갔고, SPS/PPS(AVC 시퀀스 헤더)도 그때 한 번 지나갔다.
지금 이 순간 `./play.sh`로 ffplay를 켠다 — 그런데 화면이 **즉시** 나온다. 이미 30초
전에 지나가 버린 "한 번만 오는 것들"을 이 플레이어는 어떻게 받았을까?

이것이 라이브 스트리밍 특유의 **중간 입장(mid-join) 문제**이고, 이번 파트의 심장이다.
전반부에서는 play의 커맨드 시퀀스(서버 응답 5연타)를 와이어 수준에서 해부하고,
후반부에서 세 캐시 — onMetaData, 시퀀스 헤더, GOP — 가 이 문제를 푸는 방식을 따라간다.
CLAUDE.md §2.5의 player 다이어그램에서 이번 파트가 확대하는 구간은 여기다:

```text
client (ffplay/VLC)                   server
  ── connect(app)         tid=1 ──▶      ┐
  ◀─ WinAckSize/SetPeerBW/SetChunkSize   │ Part 6과 동일한 부트스트랩
  ◀─ _result(connect) ────────────       ┘
  ── createStream()       tid=2 ──▶      ┐ identify (§1)
  ◀─ _result(streamId=1) ─────────       │
  ── play(name)           tid=0 ──▶      ┘  sid=1
  ◀─ UserControl StreamBegin(1) ──    ┐
  ◀─ onStatus(NetStream.Play.Reset)   │
  ◀─ onStatus(NetStream.Play.Start)   │ start_play — 응답 5연타 (§2)
  ◀─ |RtmpSampleAccess(true,true) ─   │
  ◀─ onMetaData → 시퀀스 헤더 → GOP ─  ┘  ← 프리필 (§4~6)
  ◀─ 라이브 audio/video ───────────      ← 송신 루프 (§7)
```

---

## 1. play까지 — 식별의 다른 갈래

Part 6의 `identify_client`는 connect 다음에 오는 커맨드를 보고 클라이언트의 정체를
판별하는 상태 기계였다. publisher는 releaseStream을 먼저 보내므로 그 자리에서 FMLE로
판정됐지만, player가 connect 다음에 보내는 것은 **createStream**이다.
createStream만으로는 정체를 알 수 없다 — publish도 play도 그 뒤에 오기 때문이다. 그래서
[`identify_client`](../src/protocol/srs_protocol_rtmp_stack.cpp#L1762)는
createStream을 만나면
[`identify_create_stream_client`](../src/protocol/srs_protocol_rtmp_stack.cpp#L2031)로
들어가 **`_result(streamId=1)`를 먼저 보내 놓고, 다음 커맨드를 계속 기다린다**:

```text
identify_client:
  createStream? ──▶ identify_create_stream_client(depth=3):
                      _result(streamId=1) 송신
                      다음 커맨드 대기...
                        ├─ play?         ──▶ type=Play, stream_name 확정   ← 이 경로
                        ├─ publish?      ──▶ type=FlashPublish
                        └─ createStream? ──▶ 재귀 (depth-1)
```

재귀 깊이 3의 가드는 createStream을 여러 번 보내는 클라이언트(일부 Flash 앱)를
수용하되 무한 재귀를 막는 안전판이다. play가 도착하면
[`identify_play_client`](../src/protocol/srs_protocol_rtmp_stack.cpp#L2121)는
판별식이랄 것도 없다 — type과 stream_name, duration을 채우고 끝난다.

### 1.1 SrsPlayPacket — 응답 없는 커맨드

play 커맨드의 페이로드를 바이트로 보면 (sid=1, csid 5로 온다 — Part 7 §2에서 본
"스트림에 대한 대화" 채널):

```text
02 00 04 70 6C 61 79                            string(4)  "play"
00 00 00 00 00 00 00 00 00                      number 0.0            ← tid = 0 !
05                                              null                  ← command object
02 00 0A 6C 69 76 65 73 74 72 65 61 6D          string(10) "livestream" ← 스트림 이름
(이하 선택) 00 C0 00 ...                         number -2.0           ← start
           00 BF F0 ...                         number -1.0           ← duration
           01 01                                boolean true          ← reset
```

**tid가 0이다.** connect(tid=1)/createStream(tid=2)과 달리 play는 `_result`를 받지
않는다 — 성공/실패 통보는 tid 매칭이 아니라 §2의 onStatus 이벤트로 온다. Part 6에서
본 "커맨드는 대부분 fire-and-forget"의 극단이다.

선택 필드 셋은 Flash 시대의 VOD 유산이다. 스펙상 **start**는 -2(라이브 있으면
라이브, 없으면 녹화본), -1(라이브만), 0 이상(녹화본의 초 단위 오프셋)을 구분하고
**duration**은 재생 길이 제한, **reset**은 이전 재생목록 플러시 여부다.
[`SrsPlayPacket::decode`](../src/protocol/srs_protocol_rtmp_stack.cpp#L2804)는
스트림 이름까지 읽은 뒤 [`stream->empty()`가 아닐 때만](../src/protocol/srs_protocol_rtmp_stack.cpp#L2827)
나머지를 읽는다 — 클라이언트마다 어디까지 보내는지 제각각이기 때문이다. reset은
[Boolean으로도 Number로도 온다](../src/protocol/srs_protocol_rtmp_stack.cpp#L2843)
(스펙이 "Boolean 또는 Number"라고 적어 둔 흔치 않은 정직함). 라이브 전용 서버인
srs_simple은 셋 다 읽기만 하고 쓰지 않는다 — 원본의 duration 제한 재생도 S8에서
제거했다 (CLAUDE.md §5.6).

---

## 2. 서버 응답 5연타 — start_play

identify가 끝나면 [`stream_service_cycle`](../src/app/srs_app_rtmp_conn.cpp#L244)이
[`rtmp->start_play(stream_id)`](../src/protocol/srs_protocol_rtmp_stack.cpp#L1830)
(원본 `protocol/srs_protocol_rtmp_stack.cpp:2519`)를 호출한다. Part 7의
`start_fmle_publish`가 문답 시퀀스였다면 start_play는 일방 통보 4연발 + 프리필이다.
하나씩 보자.

**① UserControl StreamBegin(1)** — Part 4에서 본 컨트롤 메시지다. event_data에
"시작되는 스트림"의 sid(1)를 싣지만, 패킷 자체는
[`send_and_free_packet(pkt, 0)`](../src/protocol/srs_protocol_rtmp_stack.cpp#L1839)
— **stream_id=0**으로 보낸다. 헷갈리기 쉬운 지점이라 표로 박아 두자:

| 응답 | message stream_id | 이유 |
| --- | --- | --- |
| StreamBegin | **0** | 프로토콜 수준 통보 — 특정 스트림에 속하지 않고, 대상 sid는 페이로드(event_data)에 |
| onStatus 2종, \|RtmpSampleAccess | **1** | 스트림 수준 사건 — createStream이 발급한 그 스트림에서 일어난 일 |

플레이어 다수(특히 Flash 계열)는 StreamBegin을 받아야 "스트림이 열렸다"고 보고
이후 메시지를 처리한다 — play 시작 전 필수인 이유다 (CLAUDE.md §2.3).

**② onStatus(NetStream.Play.Reset), ③ onStatus(NetStream.Play.Start)** — Part 7
§3에서 본 만능 통보 패킷 `SrsOnStatusCallPacket` 그대로다. 클라이언트가 기계적으로
검사하는 것은 code뿐이라는 것도 같다. Reset은 "이전 재생 상태를 버려라"(§1.1의
reset 필드에 대응하는 통보), Start가 진짜 개시 신호다. publisher의 Publish.Start가
인가 뒤로 미뤄졌던 것과 달리(Part 7 §3.2) play 쪽은 이 시점에 바로 보낸다 —
srs_simple에는 play 인가 단계가 없기 때문이다(원본의 refer/security는 CLAUDE.md
§1의 제거 목록).

**④ |RtmpSampleAccess(true, true)** — 낯선 이름의 type 18(AMF0 data) 메시지다:

```text
02 00 11 7C 52 74 6D 70 53 61 6D 70 6C 65 41 63 63 65 73 73   string(17) "|RtmpSampleAccess"
01 01                                                          boolean true  ← video
01 01                                                          boolean true  ← audio
```

Flash Player 보안 모델의 유산으로, ActionScript의 `BitmapData.draw()` 같은 API가
비디오 프레임의 픽셀을 읽어도 되는지(sample access)를 서버가 허가하는 신호였다.
ffplay/VLC는 무시한다. 원본 주석 스타일대로라면 (false,false)가 기본이지만 SRS는
[이슈 #49](https://github.com/ossrs/srs/issues/49) 이래 둘 다 true로 보내고,
srs_simple의 [`SrsSampleAccessPacket`](../src/protocol/srs_protocol_rtmp_stack.cpp#L2991)도
[같은 값을 따른다](../src/protocol/srs_protocol_rtmp_stack.cpp#L1880).

**⑤가 없다?** — 원본과 srs_simple 모두 `start_play`는 여기서 끝난다.
[함수 끝의 주석](../src/protocol/srs_protocol_rtmp_stack.cpp#L1888)이 "스펙 vs
현실"의 좋은 예다: 스펙 계열 문서들이 언급하는 onStatus(NetStream.Data.Start)를
보내면 **ffmpeg가 빈 데이터 스트림("Stream #0:0: Data: none")을 만들어 버려서**
일부러 보내지 않는다. 다이어그램의 다섯 번째 줄 — onMetaData부터 시작하는 프리필 —
은 커맨드 시퀀스가 아니라 다음 절부터의 주인공, 캐시 재생분이다.

utest의 [PlayBootstrap](../utest/srs_utest_server.cpp#L385)이 이 시퀀스를
실소켓으로 검증한다: createStream → play를 보내고, StreamBegin(event_data=1,
stream_id=0)과 onStatus(Play.Reset을 지나 Play.Start)를 순서대로 받는다.

---

## 3. 중간 입장 문제 — 30초 늦은 플레이어에게 없는 세 가지

서두의 상황으로 돌아가자. 방송 30초 뒤에 접속한 ffplay는 커맨드 시퀀스를 무사히
마쳤다. 이제 서버가 라이브 미디어를 중계해 주기만 하면 될까? **안 된다.** 지금
이 순간부터의 메시지만 보내면 플레이어에게는 세 가지가 없다:

1. **onMetaData가 없다** — 해상도, 프레임레이트, 코덱 id를 모른다. 플레이어는
   스트림을 프로브하느라 시간을 쓰거나, 일부는 아예 재생을 못 연다. Part 7 §4에서
   ffmpeg가 publish 직후 **한 번만** 보내는 것을 확인했다 — 30초 전에 지나갔다
2. **SPS/PPS(AVC 시퀀스 헤더)가 없다** — 디코더를 초기화할 수 없다. 이게 셋 중
   치명상이다: 이후 도착하는 **모든 NALU가 디코드 불가**다. 역시 publish 직후 한 번만
   온다(Part 7 §5.3). 캐시 없이는 늦게 온 플레이어는 영원히 화면을 못 연다
3. **직전 키프레임이 없다** — 시퀀스 헤더를 어찌 받았대도, H.264의 인터 프레임(P/B)은
   이전 프레임에 대한 차분이다. 키프레임(I) 없이는 그릴 수 없다. 라이브의 키프레임
   간격(GOP)이 2초라면, 다음 키프레임까지 **최대 2초의 검은 화면**을 보게 된다

세 결핍의 공통 구조가 보일 것이다: **필요한 데이터가 전부 "과거"에 있다.** 그래서
해법도 하나다 — 서버가 과거를 보관했다가, 새 플레이어에게 재생해 준다. Part 7에서
publisher 경로가 심어 둔 두 캐시(MetaCache, GopCache)가 정확히 이 보관소였고, 이제
회수 시점이다. 1은 MetaCache의 onMetaData가, 2는 MetaCache의 시퀀스 헤더 2종이,
3은 GopCache가 푼다.

---

## 4. consumer_dumps — 프리필, 그리고 순서가 규칙인 이유

play 경로의 conn 쪽 코드는 놀랄 만큼 짧다.
[`SrsRtmpConn::playing`](../src/app/srs_app_rtmp_conn.cpp#L272)
(원본 `app/srs_app_rtmp_conn.cpp:702`):

```cpp
SrsLiveConsumer* consumer = NULL;
source->create_consumer(consumer);      // ① 팬아웃 대상으로 등록
source->consumer_dumps(consumer);       // ② 캐시 재생분을 큐에 프리필  ★
err = do_playing(source, consumer);     // ③ 송신 루프 (§7)
srs_freep(consumer);                    //    소멸자가 팬아웃 대상에서 제거
```

[`create_consumer`](../src/app/srs_app_source.cpp#L997)는 이 연결 전용
`SrsLiveConsumer`(개인 재생 큐 + 개인 지터)를 만들어 소스의 consumers 목록에 넣는다.
이 순간부터 publisher가 미는 모든 메시지가 이 큐에도 복사(refcount만 증가 —
Part 9)되기 시작한다. 그리고
[`consumer_dumps`](../src/app/srs_app_source.cpp#L1009)
(원본 `app/srs_app_source.cpp:2703`)가 §3의 세 결핍을 한 번에 채운다:

```cpp
bool active = !can_publish_;                      // publish 중일 때만
if (active) {
    meta->dumps(consumer, jitter_algorithm, ...); // onMetaData → AAC sh → AVC sh
    gop_cache->dump(consumer, jitter_algorithm);  // [마지막 키프레임 .. 현재]
}
```

주입 순서는 고정이고, 각 순서에 이유가 있다:

- **onMetaData가 맨 앞**: 스트림의 명함이 미디어보다 먼저 —
  [`SrsMetaCache::dumps`](../src/app/srs_app_source.cpp#L582)
  (원본 :1626)의 첫 enqueue
- **AAC 시퀀스 헤더를 AVC보다 먼저**: [원본 이슈 #301](../src/app/srs_app_source.cpp#L592)
  — HLS 리먹서가 스트림 앞부분만 보고 오디오 코덱을 판별할 수 있도록 굳어진 순서다.
  srs_simple에는 HLS가 없지만 순서를 그대로 보존했다. utest
  [MetaCacheDumpsOrder](../utest/srs_utest_source.cpp#L173)가 검증하는 것이 이것이다
- **GOP는 시퀀스 헤더 뒤에**: 디코더 초기화(sh) 없이 프레임(GOP)부터 오면 §3-2의
  결핍이 재현된다

그리고 호출 위치 자체가 규칙이다 (CLAUDE.md §5.3 규칙 2): **consumer_dumps는 송신
루프 시작 전에 실행돼야 한다.** ①에서 팬아웃 등록이 먼저 일어나므로, 만약 ②를
건너뛰고 ③부터 돌면 이 플레이어의 첫 바이트는 SPS/PPS 없는 GOP 중간 — 정확히 §3의
세 결핍 상태 — 이 된다. 참고로 `active` 검사도 놓치기 쉬운 디테일이다: 아직 아무도
publish하지 않은 스트림에 플레이어가 먼저 접속하면(대기 시청) 캐시가 비어 있으므로
프리필 없이 루프로 들어가고, publisher가 붙는 순간부터 팬아웃으로 받는다.

이 전체 흐름의 단위 테스트가
[LiveSourceFanoutAndMidJoin](../utest/srs_utest_source.cpp#L231)이다: publisher가
시퀀스 헤더 2종 + 키프레임 + 인터 프레임을 밀어 넣은 **뒤에** consumer를 만들어
`consumer_dumps`를 부르면, 큐에서 정확히 `[AAC sh, AVC sh, 키프레임, 인터]` 4개가
이 순서로 나온다.

---

## 5. GopCache 알고리즘 — 항상 [마지막 키프레임 .. 현재]

[`SrsGopCache::cache`](../src/app/srs_app_source.cpp#L439)
(원본 `app/srs_app_source.cpp:611`)는 publisher의 모든 미디어 메시지마다 불리지만,
알고리즘의 뼈대는 세 줄로 요약된다:

```cpp
// 키프레임이 오면: 이전 GOP 전체를 버리고 새로 시작
if (msg->is_video() && SrsFlvVideo::keyframe(...)) clear();
// 모든 프레임(audio 포함)을 뒤에 붙인다
gop_cache.push_back(msg->copy());
```

키프레임에서 clear하고 다시 쌓는다 — 이 단순한 규칙의 결과로 캐시는 **언제나
"마지막 키프레임부터 현재까지"** 구간을 들고 있다. 새 플레이어가 어느 시점에
들어오든, 캐시 재생분의 첫 프레임은 키프레임이고 마지막 프레임은 방금 전이다. §3-3
(디코드 가능)과 "즉시 재생"(다음 키프레임을 기다릴 필요 없음)이 동시에 풀린다.
대가는 지연이다 — 새 플레이어는 최대 GOP 길이만큼 과거에서 재생을 시작한다.
라이브 엣지보다 시작 속도를 우선한 트레이드오프이고, 그래서 설정
(`_srs_config->gop_cache`)으로 끌 수 있다.
[GopCacheClearOnKeyframe](../utest/srs_utest_source.cpp#L128)이 clear-후-재시작
동작의 최소 재현이다.

뼈대 주변의 가드들도 원본 그대로 가져왔다:

- **시퀀스 헤더는 들어오지 않는다** — Part 7 §5.3에서 봤듯 `on_video_imp`가 sh를
  MetaCache로 보내고 GOP 캐시 앞에서 `return`한다. sh가 GOP에 섞이면 키프레임
  clear에 쓸려 나가 유실되거나 프리필에 중복 주입된다. "설정은 MetaCache, 프레임은
  GopCache" (CLAUDE.md §5.3 규칙 6)
- **pure audio 가드**: 비디오가 한 번도 안 왔으면([`pure_audio()`](../src/app/srs_app_source.cpp#L545))
  캐시하지 않고, 캐시하다가도 마지막 비디오 이후 오디오만
  [`SRS_PURE_AUDIO_GUESS_COUNT`(115개 ≈ 3초)](../src/app/srs_app_source.cpp#L472)를
  넘기면 "비디오가 끊겼다"고 추정하고 캐시를 비운다. 키프레임 clear가 다시 오지 않는
  스트림에서 캐시가 무한히 자라는 것을 막는 휴리스틱이다
  ([GopCachePureAudio](../utest/srs_utest_source.cpp#L159))
- **프레임 수 상한**: 그래도 캐시가 [`gop_cache_max_frames_`](../src/app/srs_app_source.cpp#L490)
  (기본 2500)를 넘으면 — 키프레임을 아예 안 보내는 비정상 인코더 — 경고를 찍고 비운다

---

## 6. 타임스탬프 위생 — 과거를 현재처럼 재생하기

프리필에는 마지막 퍼즐이 하나 남아 있다. GOP 캐시 속 프레임들의 타임스탬프는
publisher 타임라인의 **30초 근방**이다. 이걸 그대로 보내면 새 플레이어의 재생
타임라인은 30000ms에서 시작한다 — 그 자체로도 이상하지만, 더 큰 문제는 재-publish다
(Part 7 §6). 인코더가 재시작하면 타임스탬프가 0부터 다시 시작하는데, 붙어 있던
플레이어 입장에서는 시간이 30초 **뒤로 점프**한다. 대부분의 플레이어는 역행
타임스탬프에서 멈추거나 버벅인다.

그래서 consumer의 재생 큐로 들어가는 모든 메시지는
[`SrsLiveConsumer::enqueue`](../src/app/srs_app_source.cpp#L319) 안에서
[`SrsRtmpJitter::correct`](../src/app/srs_app_source.cpp#L48)
(원본 `app/srs_app_source.cpp:74-132`)를 통과한다. 핵심 아이디어는 하나다: **입력
타임스탬프의 절대값을 믿지 않는다.** 연속 메시지 사이의 델타만 취하고, 그 델타가
비정상(±250ms 초과 — 재-publish 점프, 시계 역행)이면 10ms로 클램프한 뒤,
위생 처리된 델타를 0에서부터 누적해 **이 consumer 전용의 0 기반 타임라인**을
재구성한다. GOP 재생분(30초 과거)이든 재-publish 직후(0으로 점프)든, 플레이어가
보는 출력은 항상 0부터 매끄럽게 흐른다. 지터가 consumer마다 개인 소유인 이유도
이제 보인다 — 입장 시점이 다르면 "0으로 삼을 기준점"도 다르다. 알고리즘 상세와
경계 사례는 Part 9에서 다룬다.

---

## 7. do_playing — 송신 펌프

프리필이 끝나면 [`do_playing`](../src/app/srs_app_rtmp_conn.cpp#L300)의 루프가
연결이 끊길 때까지 돈다. 한 바퀴는 네 동작이다:

```cpp
while (true) {
    trd->pull();                                      // 협조적 취소 확인 (Part 9)
    while (poll(&pfd, 1, 0) > 0) {                    // ① 0ms poll — 읽을 게 있으면
        rtmp->recv_message(&msg);                     //    받아서
        process_play_control_msg(msg);                //    처리(사실상 드롭)
    }
    consumer->wait(mw_msgs, SRS_PERF_MW_SLEEP);       // ② 큐가 찰 때까지 대기
    consumer->dump_packets(&msgs, count);             // ③ 큐에서 최대 128개 인출
    rtmp->send_and_free_messages(msgs.msgs, count, sid); // ④ 일괄 송신
}
```

**②~④가 SRS의 merged-write(MW) 전략이다.** 메시지가 하나 생길 때마다 write하지
않고, [`consumer->wait`](../src/app/srs_app_source.cpp#L381)로 "128개 초과 **그리고**
350ms치 초과"([SRS_PERF_MW_SLEEP](../src/app/srs_app_rtmp_conn.cpp#L38))가 둘 다
쌓일 때까지 기다렸다가 몰아서 보낸다. 시스템 콜 횟수를 줄여 수천 플레이어를 감당하는
원본의 처리량 최적화인데, 대가로 최대 350ms의 서버 측 지연이 붙는다 — GOP 캐시와
같은 "지연 vs 효율" 트레이드오프의 송신판이다. wait의 깨어남은 publisher 스레드의
`enqueue`가 시그널한다 (원본은 st_cond, 우리는 `std::condition_variable` + 100ms
타임아웃 — CLAUDE.md §5.1).

**①은 이 루프의 숨은 필수품이다.** play 연결은 송신 위주라 recv를 안 하기 쉽지만,
그러면 두 가지를 놓친다. 첫째, 플레이어도 이따금 보낸다 — Part 4의 PingResponse,
그리고 종료 시의 closeStream/pause 같은 커맨드. 둘째가 더 중요하다: **연결 종료(FIN)
감지**다. recv를 안 하면 상대가 끊어도 송신 버퍼가 찰 때까지 모른다. 0ms poll은
"읽을 게 있을 때만 recv"로 두 문제를 한 번에 해결한다 (원본은 수신 전용 코루틴
`SrsQueueRecvThread`가 같은 역할 — CLAUDE.md §5.6 S8).

받은 커맨드의 처리는
[`process_play_control_msg`](../src/app/srs_app_rtmp_conn.cpp#L374)인데,
srs_simple에서는 **디코드 후 전부 드롭**이다. 원본은 closeStream을
`ERROR_CONTROL_RTMP_CLOSE`(Part 7 §6에서 본 제어 신호 이디엄의 play판)로, pause를
consumer 일시정지로 처리하지만, 해당 패킷 클래스(`SrsCloseStreamPacket`/`SrsPausePacket`)는
S6에서 제거했다 — 미지의 커맨드는 기본 `SrsPacket`으로 디코드되어 무시된다
(CLAUDE.md §8 "의도적으로 남긴 미구현"). 실전에서 문제가 없는 이유는 ffplay/VLC가
closeStream 직후 TCP를 닫기 때문이다 — 종료 감지는 어차피 ①의 FIN 경로가 한다.

---

## 8. 정리

player 경로에서 들고 갈 것:

1. **play는 fire-and-forget이다.** tid=0, `_result` 없음 — 응답은
   onStatus(Play.Reset/Play.Start) 이벤트로 온다. start/duration/reset 선택 필드는
   VOD 유산이고 라이브 서버는 읽고 버린다
2. **응답 5연타의 stream_id 구분**: StreamBegin은 프로토콜 수준이라 stream_id=0
   (대상 sid는 페이로드에), onStatus/|RtmpSampleAccess는 스트림 수준이라 sid=1.
   NetStream.Data.Start는 ffmpeg 호환을 위해 일부러 보내지 않는다
3. **중간 입장 문제 = "필요한 것이 전부 과거에 있다."** onMetaData(스트림 정보),
   시퀀스 헤더(디코더 초기화), 직전 키프레임(차분 디코딩 기준) — 셋 다 스트림당
   사실상 한 번만 지나간다
4. **해법은 프리필이고, 순서가 규칙이다.** `consumer_dumps`가 송신 루프 시작 전에
   onMetaData → AAC sh → AVC sh → GOP를 재생 큐에 주입한다. GopCache는 "키프레임에서
   clear 후 재시작"으로 항상 [마지막 키프레임..현재]를 유지하고, 시퀀스 헤더는 GOP가
   아니라 MetaCache 소속이다
5. **과거를 재생하니 타임스탬프 위생이 필요하다.** consumer마다 개인 지터가 위생
   처리한 델타를 누적해 0 기반 타임라인을 재구성한다 — GOP 재생분도
   재-publish 점프도 플레이어에게는 매끄럽다
6. **송신 루프는 몰아서 보내고(MW: 128개/350ms), 0ms poll로 이따금 받는다** —
   컨트롤 메시지 소비와 FIN 감지를 겸한다. closeStream/pause는 의도적 드롭

이것으로 와이어 포맷 시리즈의 본편이 끝났다. 핸드셰이크(Part 2)부터 청크(Part 3),
컨트롤(Part 4), AMF0(Part 5), 그리고 커맨드 3부작(Part 6~8)까지 — OBS가 보낸
바이트가 ffplay에 도착하기까지의 모든 프로토콜 경로를 지나왔다. 보너스인 다음
파트는 와이어를 벗어나 서버 내부로 들어간다: 1 publisher → N player 팬아웃이 어떻게
페이로드 복사 0회로 동작하는지(`SrsSharedPtrMessage`), 지터 보정의 알고리즘 전문,
그리고 느려터진 플레이어를 라이브로 끌고 오는 `SrsMessageQueue::shrink`까지 —
Part 1~8이 메커니즘이었다면 마지막 파트는 그 위의 정책이다.

---

_이 글은 [srs_simple](../README.md) 프로젝트의 RTMP 이론 시리즈 Part 8이다.
코드 대조 기준: srs_simple `src/protocol/srs_protocol_rtmp_stack.{hpp,cpp}`
(`SrsRtmpServer::identify_client:1762`, `identify_create_stream_client:2031`,
`identify_play_client:2121`, `start_play:1830`, `SrsPlayPacket::decode:2804`,
`SrsSampleAccessPacket:2991`, onStatus 상수 hpp:80-88),
`src/app/srs_app_rtmp_conn.cpp`(`stream_service_cycle:199`, `playing:272`,
`do_playing:300`, `process_play_control_msg:374`),
`src/app/srs_app_source.cpp`(`SrsRtmpJitter::correct:48`,
`SrsLiveConsumer::enqueue:319`, `dump_packets:352`, `wait:381`,
`SrsGopCache::cache:439`, `dump:512`, `SrsMetaCache::dumps:582`,
`create_consumer:997`, `consumer_dumps:1009`) / 원본 SRS 6.0
`trunk/src/protocol/srs_protocol_rtmp_stack.cpp:2519`(start_play),
`trunk/src/app/srs_app_rtmp_conn.cpp:702`(playing),
`trunk/src/app/srs_app_source.cpp:2703`(consumer_dumps), `:611/686`(GopCache
cache/dump, 키프레임 clear는 :653), `:1626`(MetaCache::dumps, audio 우선은 :1636),
`:74-132`(jitter).
실측 시퀀스: `utest/srs_utest_server.cpp`의 `PlayBootstrap:385`,
`utest/srs_utest_source.cpp`의 `LiveSourceFanoutAndMidJoin:231`,
`MetaCacheDumpsOrder:173`, `GopCacheClearOnKeyframe:128`, `GopCachePureAudio:159`._
