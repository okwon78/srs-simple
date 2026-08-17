# RTMP 깊이 읽기 (3) — 청크 스트림: RTMP의 심장

> **시리즈 안내**: 이 시리즈는 교육용 RTMP 서버 [srs_simple](../README.md)의 코드를 읽기 위해 필요한
> RTMP 프로토콜 이론을 핸드셰이크부터 모든 패킷까지 정리한다. srs_simple은 원본
> [SRS](https://github.com/ossrs/srs)의 클래스/파일/메서드 이름을 1:1로 미러링하므로,
> 여기서 익힌 내용은 원본 SRS를 읽을 때 그대로 통한다.
>
> **시리즈 목차**
>
> 1. [RTMP 조감도 — 메시지, 청크, 스트림](part1-overview.md)
> 2. [핸드셰이크 — 연결의 첫 3,073+1,536 바이트](part2-handshake.md)
> 3. **청크 스트림 — RTMP의 심장** (이 글)
> 4. [프로토콜 컨트롤 메시지 5종](part4-control-messages.md)
> 5. [AMF0 — 커맨드의 언어](part5-amf0.md)
> 6. [커맨드 흐름 (1) — connect와 스트림 생성](part6-netconnection.md)
> 7. [커맨드 흐름 (2) — publish와 미디어 메시지](part7-publish.md)
> 8. [커맨드 흐름 (3) — play와 중간 입장 문제](part8-play.md)
> 9. [(보너스) 서버 내부 — 팬아웃, 캐시, 지터](part9-server-internals.md)

이 글은 Part 2까지 읽었다고 가정한다. 핸드셰이크(Part 2)가 끝난 소켓에는 클라이언트의 첫 청크 — connect 커맨드의 첫 조각 — 가
도착해 있다. 이 파트는 그 바이트를 메시지로 재조립하는 가운데 층, Part 1에서 예고한
**청크 스트림 층의 전부**다. 예고했던 대로 시리즈에서 가장 밀도 높은 구간이다: 헤더가 1바이트에서
18바이트까지 늘었다 줄었다 하는 차등 인코딩(fmt 0~3), 상태를 들고 다니는 재조립기, 그리고
구현체마다 해석이 갈려 값 비교로 감지해야 하는 extended timestamp까지.

주인공은 `SrsProtocol`의 수신 체인 하나다:

```text
recv_message                     완성된 메시지 하나가 나올 때까지 루프
 └─ recv_interlaced_message      청크 1개 처리
     ├─ read_basic_header        fmt(2bit) + csid → 어느 재조립 흐름인가
     ├─ read_message_header      fmt별 0/3/7/11바이트 + extended timestamp
     └─ read_message_payload     chunk_size만큼 누적, 다 차면 메시지 완성
```

이 네 함수([srs_protocol_rtmp_stack.cpp](../src/protocol/srs_protocol_rtmp_stack.cpp))가
이 글의 뼈대다. 원본 SRS에서도 함수 이름과 구조가 완전히 같다
(원본 `protocol/srs_protocol_rtmp_stack.cpp:328/786/883/934/1188`).

---

## 1. 문제 정의: 헤더를 어디까지 압축할 수 있는가

Part 1에서 청크가 존재하는 이유(인터리빙 — 큰 키프레임이 작은 오디오를 굶기지 않게)를 봤다.
그런데 인터리빙에는 비용이 있다. 모든 조각 앞에 "이 조각이 어느 메시지의 것인지"를 알려주는
헤더가 붙어야 한다. 메시지 헤더 전체(timestamp 3 + length 3 + type 1 + stream_id 4 = 11바이트)를
조각마다 반복하면, 기본 청크 크기 128바이트 기준 오버헤드가 거의 10%다.

RTMP의 답은 **차등 인코딩(differential encoding)**이다. 관찰은 이렇다: 같은 흐름(csid)의
연속된 메시지는 헤더가 거의 같다. 오디오 스트림이라면 type은 늘 8이고, stream_id는 늘 1이고,
payload 길이도 대개 같고, timestamp만 일정한 간격(delta)으로 증가한다. 그러니 **바뀐 필드만
보내고 나머지는 "직전과 같음"으로 처리**하면 된다. 얼마나 생략하는지를 basic header의
2비트 `fmt`가 선언한다:

```text
청크 1개의 구성:
┌──────────────┬─────────────────┬────────────────────┬───────────────────┐
│ basic header │ message header  │ extended timestamp │ chunk data        │
│ 1~3 bytes    │ fmt별 11/7/3/0B  │ 0 또는 4 bytes      │ ≤ chunk_size      │
└──────────────┴─────────────────┴────────────────────┴───────────────────┘
```

| fmt | 크기 | 담는 것                                       | 뜻                                |
| --- | ---- | --------------------------------------------- | --------------------------------- |
| 0   | 11B  | timestamp(절대값) + length + type + stream_id | 전부 새로 선언 (새 흐름의 시작)   |
| 1   | 7B   | timestamp **delta** + length + type           | stream_id는 직전과 같음           |
| 2   | 3B   | timestamp **delta**만                         | length/type/sid 전부 직전과 같음  |
| 3   | 0B   | 없음                                          | 전부 직전과 같음 (delta까지 상속) |

극단이 fmt=3이다. 헤더가 basic header 1바이트로 끝난다. FMLE가 보내는 연속 오디오 프레임은
메시지 하나가 통째로 `0xC4` 1바이트 + 페이로드다 — 11바이트짜리 헤더가 1바이트로 줄었다.

여기서 이 층의 핵심 성질이 나온다: **차등 인코딩은 수신자가 상태를 유지해야만 성립한다.**
"직전과 같음"이 성립하려면 수신자가 csid별로 직전 헤더를 기억하고 있어야 한다. 그 기억이
`SrsChunkStream`이고(§3.2), 청크 스택이 "상태 기계"인 이유다.

---

## 2. Basic header — 첫 1바이트에 담긴 두 가지

모든 청크의 첫 바이트는 같은 구조다:

```text
 0 1 2 3 4 5 6 7
+-+-+-+-+-+-+-+-+
|fmt|   cs id   |     fmt: 상위 2비트, csid: 하위 6비트
+-+-+-+-+-+-+-+-+
```

파싱은 마스크와 시프트 한 번씩이다 ([srs_protocol_rtmp_stack.cpp:645-647](../src/protocol/srs_protocol_rtmp_stack.cpp#L645-L647)):

```cpp
fmt = in_buffer->read_1byte();
cid = fmt & 0x3f;
fmt = (fmt >> 6) & 0x03;
```

첫 바이트만 보면 청크의 정체를 읽을 수 있게 된다. 이 시리즈와 테스트 코드에 반복해서 나오는 값들:

| 첫 바이트 | fmt | csid | 정체                                                 |
| --------- | --- | ---- | ---------------------------------------------------- |
| `0x02`    | 0   | 2    | 프로토콜 컨트롤 (Part 4)                             |
| `0x03`    | 0   | 3    | connect 등 커맨드의 첫 청크                          |
| `0x04`    | 0   | 4    | 미디어 메시지의 첫 청크                              |
| `0x42`    | 1   | 2    | librtmp의 ping (§3.4에서 재등장)                     |
| `0xC4`    | 3   | 4    | "전부 직전과 같음" — 연속 청크 또는 FMLE 연속 오디오 |

### 2.1 6비트가 모자랄 때: 2·3바이트 형식

csid 6비트로는 0~63까지만 표현된다. 그래서 **0과 1을 확장 마커로 예약**한다
([srs_protocol_rtmp_stack.cpp:649-671](../src/protocol/srs_protocol_rtmp_stack.cpp#L649-L671)):

```text
csid 필드 = 2~63  →  그 값이 곧 csid            (1바이트 형식)
csid 필드 = 0     →  다음 1바이트 b1, csid = 64 + b1          (64~319)
csid 필드 = 1     →  다음 2바이트 b1 b2, csid = 64 + b1 + b2*256  (64~65599)
```

확장 바이트가 0이 아니라 **64부터 시작**하는 것에 주의 — 0~63은 1바이트 형식으로 이미 표현
가능하므로 낭비하지 않겠다는 설계다. 그 부작용으로 64~319 구간은 2바이트/3바이트 양쪽으로
표현 가능하다(스펙도 이를 인정한다 — 코드 위 주석 블록에 스펙 원문이 인용되어 있다,
[srs_protocol_rtmp_stack.cpp:593-636](../src/protocol/srs_protocol_rtmp_stack.cpp#L593-L636)).
파서는 어느 쪽이든 같은 csid로 수렴시키면 된다.

실전에서 OBS/ffmpeg는 csid를 2~7 범위에서만 쓰므로 2·3바이트 형식을 만날 일은 거의 없지만,
파서는 갖춰야 한다. utest의 `RecvLargeCidBasicHeader`가 두 형식을 못박는다
([srs_utest_protocol.cpp:743-790](../utest/srs_utest_protocol.cpp#L743-L790)):
`00 64` → csid 164, `01 0a 02` → csid 64+10+2·256 = 586.

---

## 3. Message header — 상태 기계의 심장

### 3.1 fmt별 필드 배치

basic header에서 fmt를 알았으니 이어지는 message header의 크기가 결정된다. 코드에서 이 결정은
배열 조회 한 줄이다 ([srs_protocol_rtmp_stack.cpp:747-748](../src/protocol/srs_protocol_rtmp_stack.cpp#L747-L748)):

```cpp
static char mh_sizes[] = {11, 7, 3, 0};
int mh_size = mh_sizes[(int)fmt];
```

fmt=0의 11바이트를 펼치면:

```text
┌───────────────┬────────────────┬──────┬──────────────────┐
│ timestamp     │ payload_length │ type │ stream_id        │
│ 3B, 빅엔디언    │ 3B, 빅엔디언     │ 1B   │ 4B, 리틀엔디언 (!) │
└───────────────┴────────────────┴──────┴──────────────────┘
```

fmt=1은 여기서 stream_id를 뺀 7바이트(timestamp 자리는 delta), fmt=2는 delta 3바이트만,
fmt=3은 0바이트다.

두 가지 저수준 디테일이 눈에 띈다. 첫째, **stream_id만 리틀엔디언**이다. RTMP의 다른 모든
멀티바이트 필드는 빅엔디언인데 이 필드 하나만 반대다 — 스펙이 그렇게 정해 버렸고, 모두가
따른다. 둘째, 3바이트 timestamp를 int32로 읽는 코드가 특이하다
([srs_protocol_rtmp_stack.cpp:765-769](../src/protocol/srs_protocol_rtmp_stack.cpp#L765-L769)):

```cpp
char* pp = (char*)&chunk->header.timestamp_delta;
pp[2] = *p++;    // 와이어의 빅엔디언 3바이트를
pp[1] = *p++;    // 리틀엔디언 머신의 int32 메모리에
pp[0] = *p++;    // 역순으로 꽂는다
pp[3] = 0;
```

`SrsBuffer`의 read_3bytes를 쓰지 않고 포인터로 직접 꽂는 이 스타일은 원본 그대로다
(원본 주석: "see also: ngx_rtmp_recv" — nginx-rtmp의 파서를 참고한 흔적).

### 3.2 `SrsChunkStream` — csid별 기억

차등 인코딩의 "직전"을 기억하는 객체가 `SrsChunkStream`이다
([srs_protocol_rtmp_stack.hpp:333-358](../src/protocol/srs_protocol_rtmp_stack.hpp#L333-L358)):

```cpp
class SrsChunkStream
{
public:
    char fmt;                       // 직전 basic header의 fmt
    int cid;                        // 이 흐름의 csid
    SrsMessageHeader header;        // 직전 청크까지 누적된 헤더 (상속의 원본)
    bool has_extended_timestamp;    // 이 메시지가 extended timestamp를 쓰는가
    SrsCommonMessage* msg;          // 조립 중인 미완성 메시지 (없으면 NULL)
    int64_t msg_count;              // 이 흐름에서 완성한 메시지 수
    int32_t extended_timestamp;     // 직전 extended timestamp 값 (§4의 감지용)
};
```

`SrsProtocol`은 이것을 csid로 인덱싱해 보관한다 —
`std::map<int, SrsChunkStream*> chunk_streams`
([srs_protocol_rtmp_stack.hpp:158](../src/protocol/srs_protocol_rtmp_stack.hpp#L158)).
처음 보는 csid가 오면 그 자리에서 만든다
([srs_protocol_rtmp_stack.cpp:562-571](../src/protocol/srs_protocol_rtmp_stack.cpp#L562-L571)).
원본은 여기에 "csid < 16이면 map 대신 배열 캐시를 먼저 조회"하는 성능 최적화(`cs_cache`)를
얹는데, srs_simple은 map만 남겼다 (CLAUDE.md §5.6) — 로직은 동일하다.

이제 헤더 파싱의 본질을 한 문장으로 말할 수 있다: **read_message_header는 와이어에서 읽은
필드로 `chunk->header`를 갱신하고, 생략된 필드는 갱신하지 않는 것으로 상속을 구현한다.**
fmt=3이면 아무것도 읽지 않으니 전부 상속된다. timestamp만 누적 연산이 필요하다:

```cpp
if (fmt == RTMP_FMT_TYPE0) {
    chunk->header.timestamp = chunk->header.timestamp_delta;   // 절대값 대입
} else {
    chunk->header.timestamp += chunk->header.timestamp_delta;  // delta 누적
}
```

([srs_protocol_rtmp_stack.cpp:794-805](../src/protocol/srs_protocol_rtmp_stack.cpp#L794-L805))

### 3.3 fmt=3의 이중성 — 연속 청크인가, 새 메시지인가

fmt=3에는 함정이 하나 있다. `0xC4`는 두 가지 상황에서 온다:

1. **큰 메시지의 연속 청크**: 200바이트 메시지가 128+72로 쪼개졌을 때의 두 번째 조각.
   조립 중인 메시지(`chunk->msg != NULL`)에 이어 붙이면 되고, timestamp는 건드리지 않는다.
2. **헤더가 완전히 같은 새 메시지**: FMLE가 오디오 프레임을 보낼 때, 두 번째 프레임부터는
   메시지 전체가 `0xC4` + 페이로드다. 이때는 **직전 delta를 다시 한 번 누적**해야 한다.

두 경우를 가르는 것이 `is_first_chunk_of_msg = !chunk->msg`
([srs_protocol_rtmp_stack.cpp:715](../src/protocol/srs_protocol_rtmp_stack.cpp#L715))이다.
조립 중인 메시지가 없는데 fmt=3이 왔다면 새 메시지이고, delta를 적용한다
([srs_protocol_rtmp_stack.cpp:835-840](../src/protocol/srs_protocol_rtmp_stack.cpp#L835-L840)).
원본 코드의 주석이 이 시나리오를 구체적인 바이트로 박제해 두었다
([srs_protocol_rtmp_stack.cpp:692-712](../src/protocol/srs_protocol_rtmp_stack.cpp#L692-L712)):
fmt=0로 timestamp=26, delta 없이 온 오디오 뒤에 `0xC4`가 오면 두 번째 메시지의 timestamp는
26+26=52다. "delta조차 상속된다"는 뜻이다 — fmt=0의 timestamp 필드값(26)이
`timestamp_delta`에 남아 있다가 재사용된다.

utest의 `RecvFmt3FreshMessageAppliesDelta`가 정확히 이 주석을 재현한다
([srs_utest_protocol.cpp:465-507](../utest/srs_utest_protocol.cpp#L465-L507)):
`0xC4` 메시지 두 개가 연달아 오면 timestamp가 26 → 52 → 78로 등차 증가한다.

### 3.4 프로토콜 위반 감지 — 그리고 예외 하나

상태 기계에는 규칙이 있고, 규칙에는 위반 검사가 따른다. 세 가지가 코드에 있다:

- **신규 청크 스트림은 fmt=0으로 시작해야 한다** (`msg_count == 0 && fmt != 0` →
  `ERROR_RTMP_CHUNK_START`). 상속할 "직전"이 없으니 당연하다. 그런데 예외가 하나 있다 —
  **librtmp는 ping을 fmt=1로 시작**한다(`0x42`). 헤더에 delta/length/type이 다 있으니
  stream_id=0으로 가정하면 파싱이 되므로, 에러 대신 경고만 하고 수용한다
  ([srs_protocol_rtmp_stack.cpp:719-733](../src/protocol/srs_protocol_rtmp_stack.cpp#L719-L733)).
  Part 2의 "스펙보다 호환성"이 여기서도 반복된다.
- **조립 중에는 fmt=0이 올 수 없다** (`chunk->msg && fmt == 0` → 에러,
  [:737-739](../src/protocol/srs_protocol_rtmp_stack.cpp#L737-L739)). fmt=0은 새 메시지
  선언인데 이전 메시지가 미완성이라면 프로토콜이 깨진 것이다.
- **조립 중 payload_length 변경 금지** (fmt=1이 다른 length를 들고 오면
  `ERROR_RTMP_PACKET_SIZE`, [:820-822](../src/protocol/srs_protocol_rtmp_stack.cpp#L820-L822)).
  이미 `payload_length`만큼 버퍼를 할당해 채우는 중이므로(§5) 길이가 흔들리면 안 된다.

첫째 검사(fmt=3 시작 → 에러, fmt=1 시작 → 경고 후 수용)와 셋째 검사(길이 변경 → 에러)는
utest `RecvProtocolErrors`에 케이스가 있다
([srs_utest_protocol.cpp:794-873](../utest/srs_utest_protocol.cpp#L794-L873)).

---

## 4. Extended timestamp — 프로토콜에서 가장 지저분한 모서리

### 4.1 기본 규칙

timestamp 필드는 3바이트라 최댓값이 0xFFFFFF(약 4.66시간)다. 그보다 큰 값은 필드에
**0xFFFFFF를 마커로 채우고**, message header 직후에 4바이트 extended timestamp를 덧붙인다:

```text
│ ff ff ff │ length │ type │ stream_id │ 01 23 45 67 │ payload...
  ▲ "진짜 값은 뒤에 있음"                 ▲ 실제 timestamp = 0x01234567
```

수신 코드는 `timestamp_delta >= 0xFFFFFF`이면 `has_extended_timestamp`를 세우고 4바이트를
추가로 읽는다 ([srs_protocol_rtmp_stack.cpp:784](../src/protocol/srs_protocol_rtmp_stack.cpp#L784),
[:842-857](../src/protocol/srs_protocol_rtmp_stack.cpp#L842-L857)). 읽은 값은 31비트로
접는다(`&= 0x7fffffff`) — 스펙 본문은 "32비트, 약 50일에 롤오버"라 말하지만 스펙의 예시와
FLV 스펙은 31비트를 가정하므로, 안전한 교집합인 31비트를 쓴다는 것이 원본 주석의 결론이다
([srs_protocol_rtmp_stack.cpp:904-924](../src/protocol/srs_protocol_rtmp_stack.cpp#L904-L924)).

여기까지는 지저분하지 않다. 문제는 다음 질문이다: **연속 청크(fmt=3)에도 extended timestamp가
붙는가?**

### 4.2 스펙이 갈라진 곳: adobe는 보내고, ffmpeg는 안 보낸다

스펙 초판 기준으로 fmt=3 청크에는 extended timestamp가 없어야 한다. 그런데 Adobe가 스펙을
바꿨고(자사 제품 FMLE/FMS/Flash는 **fmt=3에도 항상 보낸다**), ffmpeg/librtmp는 옛 해석대로
**안 보낸다**. nginx-rtmp는 fmt=3을 아예 만들지 않는 방식으로 문제를 회피한다. 수신자는
양쪽을 다 받아야 한다 — 그런데 fmt=3 헤더는 0바이트라서, 다음 4바이트가 extended timestamp인지
페이로드 첫 4바이트인지 알려주는 **어떤 신호도 없다**.

SRS의 해법은 정직한 휴리스틱이다: **일단 4바이트를 읽어 보고, 직전에 기억해 둔 extended
timestamp와 값이 다르면 페이로드였다고 판단해 되돌린다**
([srs_protocol_rtmp_stack.cpp:879-901](../src/protocol/srs_protocol_rtmp_stack.cpp#L879-L901)):

```cpp
uint32_t chunk_extended_timestamp = (uint32_t)chunk->extended_timestamp;

if (!is_first_chunk_of_msg && chunk_extended_timestamp > 0
    && chunk_extended_timestamp != timestamp) {
    // 연속 청크(0xC3)가 extended timestamp를 다시 보내지 않는 구현(ffmpeg/librtmp)이면,
    // 방금 읽은 4바이트는 페이로드다 — 값 비교로 감지해서 되돌린다.
    mh_size -= 4;
    in_buffer->skip(-4);
} else {
    chunk->extended_timestamp = timestamp;
    ...
}
```

같은 메시지의 연속 청크라면 timestamp가 같을 수밖에 없다는 성질을 이용한 것이다. adobe
스타일이면 읽은 4바이트가 기억해 둔 값과 일치해 소비되고, ffmpeg 스타일이면 페이로드 바이트가
우연히 같은 값일 리 없으니(이론적 오탐 확률은 있다) `skip(-4)`로 커서를 되돌린다. 이것이
가능한 것은 수신 버퍼 `SrsFastStream`이 소켓에서 미리 읽어 둔 바이트 위에서 커서만 움직이는
구조이기 때문이다 (`grow`로 채우고 `read_slice`로 소비,
[srs_protocol_stream.cpp:42-61](../src/protocol/srs_protocol_stream.cpp#L42)) — 소켓은
되돌릴 수 없지만 버퍼는 되돌릴 수 있다.

비교 기준을 `chunk->header.timestamp`가 아니라 별도 필드 `chunk->extended_timestamp`에
저장하는 것도 이유가 있다: fmt=1/2에서는 extended timestamp 자리에 **delta**가 오므로,
누적된 header.timestamp와는 값이 다르다. 원본이 이 버그를 나중에 고쳤고(PR #4356, 필드
주석에 링크가 있다), srs_simple도 고쳐진 버전을 미러링한다.

utest는 양쪽 스타일을 모두 검증한다: `RecvExtendedTimestampRepeatedInC3`(adobe 스타일,
[srs_utest_protocol.cpp:590-625](../utest/srs_utest_protocol.cpp#L590-L625))와
`RecvExtendedTimestampNotRepeatedInC3`(ffmpeg 스타일,
[:629-684](../utest/srs_utest_protocol.cpp#L629-L684)). 후자에는 미묘한 준비가 필요하다 —
감지 로직이 4바이트를 미리 읽으므로, 페이로드가 2바이트만 남은 상황에서는 **뒤따르는 다음
메시지의 첫 2바이트를 빌려서** 비교한 뒤 되돌린다. 테스트가 후속 메시지를 붙여 두고, 되돌린
바이트에서 그 메시지가 온전히 파싱되는 것까지 확인하는 이유다.

송신 쪽 SRS는 **항상 adobe 스타일**이다: fmt=3 청크에도 extended timestamp를 붙인다
([srs_kernel_flv.cpp:384-393](../src/kernel/srs_kernel_flv.cpp#L384-L393) — 주석에
"스펙은 MUST NOT이라 하지만 adobe 호환을 위해 항상 보낸다"). 받는 쪽은 다 받아주고, 보내는
쪽은 가장 널리 받아들여지는 방언 하나를 고른다 — 견고성 원칙(be liberal in what you accept,
be conservative in what you send)의 교과서적 적용이다.

---

## 5. 재조립과 배출: read_message_payload와 recv_message

### 5.1 페이로드 누적

헤더가 끝나면 페이로드 조각을 모을 차례다. `read_message_payload`는 단순하다
([srs_protocol_rtmp_stack.cpp:939-979](../src/protocol/srs_protocol_rtmp_stack.cpp#L939-L979)):

```cpp
int payload_size = chunk->header.payload_length - chunk->msg->size;  // 남은 양
payload_size = srs_min(payload_size, in_chunk_size);                 // 이번 청크 몫

if (!chunk->msg->payload) {
    chunk->msg->create_payload(chunk->header.payload_length);        // 전체 크기로 1회 할당
}

memcpy(chunk->msg->payload + chunk->msg->size, in_buffer->read_slice(payload_size), payload_size);
chunk->msg->size += payload_size;

if (chunk->header.payload_length == chunk->msg->size) {              // 다 찼는가?
    *pmsg = chunk->msg;
    chunk->msg = NULL;                                               // 메시지를 흐름에서 분리
    return err;
}
```

읽을 양은 "메시지의 남은 양과 `in_chunk_size` 중 작은 쪽"이다 — 청크 크기의 정의가 코드로는
이 `srs_min` 한 줄이다. 버퍼는 첫 조각에서 `payload_length` 전체 크기로 한 번만 할당한다
(§3.4의 "조립 중 길이 변경 금지"가 이 할당을 보호한다). 다 차면 메시지를 `chunk->msg`에서
떼어 위로 올리고, `chunk->msg`는 NULL — 이 순간 `is_first_chunk_of_msg`(§3.3)가 다시 참이
되어, 다음 fmt=3을 "새 메시지"로 해석할 준비가 된다. 상태 기계가 한 바퀴 돈 것이다.

`in_chunk_size`의 기본값은 스펙이 정한 128
([srs_protocol_rtmp_stack.hpp:35](../src/protocol/srs_protocol_rtmp_stack.hpp#L35))이고,
상대가 SetChunkSize를 보내면 `on_recv_message`가 갱신한다
([srs_protocol_rtmp_stack.cpp:1024-1043](../src/protocol/srs_protocol_rtmp_stack.cpp#L1024-L1043)).
OBS는 접속 직후 4096을 보낸다 — 갱신을 빼먹으면 OBS의 첫 4096바이트 청크에서 재조립이
어긋난다. SetChunkSize를 포함한 컨트롤 메시지들의 의미는 Part 4의 주제다.

### 5.2 인터리빙의 실제 모습

Part 1의 인터리빙 그림이 이 상태 기계에서 어떻게 굴러가는지, utest
`RecvInterlacedChunkStreams`([srs_utest_protocol.cpp:687-740](../utest/srs_utest_protocol.cpp#L687-L740))의
시나리오로 따라가 보자. 와이어에 이 순서로 도착한다:

```text
[cid6 fmt0 헤더, video 200B 선언][video 128B]   ← cid6: 조립 중 (128/200)
[cid7 fmt0 헤더, audio 4B][audio 4B]            ← cid7: 즉시 완성!
[0xC6 (cid6 fmt3)][video 나머지 72B]             ← cid6: 완성 (200/200)
```

`recv_message`를 부르면 **오디오가 먼저 나온다**. 나중에 시작한 메시지가 먼저 완성되는 것 —
이것이 인터리빙의 목적 그 자체다(작은 오디오가 큰 비디오를 추월한다). cid6의 미완성 메시지는
그동안 `chunk_streams[6]->msg`에 얌전히 누워 있다.

이것을 가능하게 하는 호출 구조가 `recv_message`의 루프다
([srs_protocol_rtmp_stack.cpp:212-248](../src/protocol/srs_protocol_rtmp_stack.cpp#L212-L248)):
`recv_interlaced_message`는 **청크 하나**를 처리하고, 메시지가 완성됐을 때만 msg를 채워 준다.
완성이 안 됐으면(NULL) 루프가 다음 청크를 계속 읽는다. 완성된 메시지는 `on_recv_message`
훅을 거친 뒤(자동 Acknowledgement 송신, SetChunkSize/PingRequest 반영 — Part 4) 호출자에게
반환된다. 즉 **호출자는 청크의 존재를 전혀 모른다.** `recv_message`의 서명이 이 층의 추상화
경계다: 아래로는 바이트와 조각, 위로는 완성된 메시지만.

---

## 6. 송신 경로 — fmt=0과 fmt=3만으로 충분하다

받는 쪽은 4가지 fmt를 모두 파싱해야 하지만(클라이언트가 뭘 보낼지 모르니까), **보내는 쪽은
방언을 고를 자유가 있다**. SRS의 선택은 극단적으로 단순하다: **메시지의 첫 청크는 항상 fmt=0,
이어지는 청크는 항상 fmt=3.** fmt=1/2는 아예 만들지 않는다. 메시지당 몇 바이트를 더 쓰는
대신, 송신 경로에서 "직전 메시지와 뭐가 같은지" 비교하는 상태 관리가 통째로 사라진다.

송신 루프가 그 결정을 그대로 보여준다
([srs_protocol_rtmp_stack.cpp:275-328](../src/protocol/srs_protocol_rtmp_stack.cpp#L275-L328)):

```cpp
char* p = msg->payload;
char* pend = msg->payload + msg->size;

while (p < pend) {
    // 첫 청크는 fmt=0(c0), 이어지는 청크는 fmt=3(c3)
    int nbh = msg->chunk_header(c0c3_cache, nb_cache, p == msg->payload);

    iovs[0].iov_base = c0c3_cache;   iovs[0].iov_len = nbh;            // 헤더
    int payload_size = srs_min(out_chunk_size, (int)(pend - p));
    iovs[1].iov_base = p;            iovs[1].iov_len = payload_size;   // 페이로드 조각

    p += payload_size;
    skt->writev(iovs, 2, NULL);
}
```

`p == msg->payload`(아직 한 바이트도 안 보냈는가)가 c0/c3 선택의 전부다. 페이로드는
**복사하지 않는다** — writev의 두 번째 iov가 메시지 버퍼 안쪽을 직접 가리킨다. 헤더 16바이트짜리
캐시 하나(`out_c0c3_caches`)에 매 청크의 헤더를 새로 찍어 쓴다.

헤더를 찍는 함수는 `srs_chunk_header_c0`/`c3`
([srs_kernel_flv.cpp:304-397](../src/kernel/srs_kernel_flv.cpp#L304-L397))다. c0는
§3.1의 11바이트 배치를 역방향으로 쓰는 것이고(timestamp가 0xFFFFFF 이상이면 마커를 채우고
말미에 4바이트 추가), c3는 `0xC0 | csid` 1바이트(+ extended timestamp면 4바이트, §4.2)다.
이 두 함수가 protocol이 아니라 kernel(`srs_kernel_flv.cpp`)에 있는 것이 의아할 수 있는데,
원본에서 `kernel/srs_kernel_utility.cpp:1184/1259` 소속이기 때문이다 — srs_simple은 utility
파일을 만들지 않아 flv 파일에 병합했다 (CLAUDE.md §5.6).

아웃바운드 청크 크기는 서버가 connect 응답 직전에 SetChunkSize로 60000을 선언하고
(`_srs_config.chunk_size`, Part 4·6), 자기 선언을 `on_send_packet`이 `out_chunk_size`에
반영한다 ([srs_protocol_rtmp_stack.cpp:1077-1081](../src/protocol/srs_protocol_rtmp_stack.cpp#L1077-L1081)).
in과 out이 **독립된 협상**이라는 점을 기억하자 — 서버는 60000으로 보내면서, OBS가 선언한
4096으로 받는다.

원본과의 차이도 이 지점에 있다. 원본의 `do_send_messages`는 수백 개 청크의 iovec을 모아
`writev` 한 번에 보내는 배칭(`SRS_PERF_COMPLEX_SEND`)이 겹겹이 있는 성능 코드다. srs_simple은
청크마다 (헤더, 페이로드) 2-iov writev 한 번으로 축소했다 (CLAUDE.md §5.6) — 시스템 콜
횟수는 늘지만 와이어에 나가는 바이트는 동일하다. 원본 `:391`을 읽을 때는 "이 2-iov 루프에
배칭을 씌운 것"으로 읽으면 된다.

---

## 7. 실측: 라운드트립으로 검증하는 대칭성

청크 스택은 utest 밀도가 시리즈에서 가장 높은 곳이다(ProtocolStackTest 16개). 그중 송신과
수신의 대칭성을 한 번에 보여주는 것이 `SendMessageRoundTrip`
([srs_utest_protocol.cpp:986-1026](../utest/srs_utest_protocol.cpp#L986-L1026))이다:
300바이트 오디오 메시지를 송신 경로로 내보내고, 출력된 바이트를 **다른 `SrsProtocol` 인스턴스의
입력에 재주입**해 원본 메시지가 복원되는지 본다. 이때 출력 크기의 산수가 §6의 규칙 그대로다:

```text
300바이트, out_chunk_size=128:
  12B (c0: basic 1 + mh 11) + 128B
+  1B (c3)                  + 128B
+  1B (c3)                  +  44B   = 314바이트
```

extended timestamp 버전(`SendExtendedTimestampRoundTrip`,
[:1029-1066](../utest/srs_utest_protocol.cpp#L1029-L1066))은 200바이트 비디오 메시지를
timestamp 0x01234567로 같은 방식으로 왕복시킨다 — c0가 16B(12+4), c3가 5B(1+4)로 커진다(§4.2의 "SRS는 c3에도 항상 보낸다"의 실측).
16 + 128 + 5 + 72 = 221바이트.

라운드트립 테스트의 가치는 회귀 방지 이상이다. 송신 코드와 수신 코드는 같은 와이어 포맷의
양면인데 구현은 완전히 분리되어 있다(`srs_chunk_header_c0` vs `read_message_header`).
한쪽에만 버그가 있으면 라운드트립이 즉시 깨진다. 반대로 **양쪽에 대칭인 버그**는 못 잡는다는
한계도 있다 — 그래서 수신 전용 테스트들(§2~4에서 인용한, 손으로 쓴 hex를 넣는 방식)이 따로
있는 것이다. 손으로 쓴 hex는 "스펙이 말하는 바이트"를, 라운드트립은 "구현의 자기 일관성"을
검증한다. 실전 검증의 정점은 진짜 OBS/ffmpeg다: OBS의 SetChunkSize 4096 반영, ffmpeg 스타일
extended timestamp 모두 실클라이언트 매트릭스에서 확인됐다 (README의 검증 기록).

---

## 8. 정리

청크 스트림에서 들고 갈 것:

1. **청크 층은 차등 인코딩 상태 기계다.** fmt 0~3이 "얼마나 생략하는가"를 선언하고, 수신자는
   csid별 `SrsChunkStream`에 직전 헤더를 기억해 생략분을 상속으로 복원한다. 갱신하지 않는
   것이 곧 상속이다.
2. **fmt=3은 두 얼굴이다.** 조립 중이면 연속 청크, 아니면 "헤더가 완전히 같은 새 메시지"이고,
   후자는 delta까지 상속해 누적한다. 가르는 기준은 `chunk->msg`의 존재 여부 하나다.
3. **extended timestamp는 값 비교로 감지한다.** fmt=3에 확장 타임스탬프를 adobe는 보내고
   ffmpeg는 안 보내는데 신호 필드가 없으므로, 4바이트를 읽어 직전 값과 다르면 `skip(-4)`로
   되돌린다. 받을 땐 관대하게, 보낼 땐 한 방언(항상 보냄)으로.
4. **송신은 fmt=0 + fmt=3만 쓴다.** 수신은 4종 전부를 파싱해야 하지만 송신은 방언을 고를 수
   있고, SRS는 몇 바이트를 내주고 상태 관리를 지웠다. `recv_message` 위로는 청크가 보이지
   않는다 — 이 함수의 서명이 층의 추상화 경계다.

이제 완성된 메시지가 위층에 올라온다. 그 첫 손님들이 type 1~6, 청크 층 자신을 관리하는
프로토콜 컨트롤 메시지들이다 — 방금 §5.1에서 스쳐 지나간 SetChunkSize가 왜 connect 응답보다
먼저 나가야 하는지(OBS 이슈 #454), 수신 바이트의 절반마다 자동으로 나가는 Acknowledgement가
무엇인지가 다음 파트다.

---

_이 글은 [srs_simple](../README.md) 프로젝트의 RTMP 이론 시리즈 Part 3이다.
코드 대조 기준: srs_simple `src/protocol/srs_protocol_rtmp_stack.{hpp,cpp}`(`SrsProtocol`,
`SrsChunkStream`), `src/kernel/srs_kernel_flv.cpp`(`srs_chunk_header_c0/c3`) / 원본 SRS 6.0
`trunk/src/protocol/srs_protocol_rtmp_stack.cpp:328`(recv_message), `:786`(recv_interlaced_message),
`:883`(read_basic_header), `:934`(read_message_header), `:1188`(read_message_payload),
`:391`(do_send_messages), `trunk/src/kernel/srs_kernel_utility.cpp:1184/1259`(c0/c3 직렬화)._
