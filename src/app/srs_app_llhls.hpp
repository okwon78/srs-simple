// srs_simple — 원본 SRS 대응물 없음 (SRS 6.0에는 LL-HLS가 없다).
// LL-HLS(Low-Latency HLS) 앱 계층: RTMP 프레임 → fMP4 파트(moof+mdat, 0.5s) →
// 인메모리 롤링 윈도우. 구조는 OvenMediaEngine(../OvenMediaEngine)을 차용하고
// 이름은 SRS 컨벤션(SrsLlHls*)을 따른다 — PLANS.md §4 진행 규칙, D1~D5.
//
//   SrsLlHls          SrsOriginHub가 부르는 진입점 (기존 SrsHls와 대칭)
//                       ↔ OME LLHlsStream::SendVideoFrame/SendAudioFrame
//   SrsLlHlsMuxer     파트/세그먼트 컷 정책 + S12 fMP4 인코더 구동
//                       ↔ OME FMP4Packager::AppendSample
//   SrsLlHlsStorage   인메모리 세그먼트/파트 보관 + 게시 통지
//                       ↔ OME FMP4Storage (옵저버 콜백 대신 condition_variable —
//                         1-connection-1-thread라 HTTP 스레드가 직접 대기하면
//                         그것이 곧 블로킹 리로드다, PLANS.md D4)
//   SrsLlHlsChunklist m3u8 텍스트 생성기 (S14)
//                       ↔ OME LLHlsChunklist::MakeChunklist. storage가 게시마다
//                         락 안에서 재생성해 캐시한다 — notify_all보다 먼저 갱신되므로
//                         블로킹 리로드로 깨어난 HTTP 스레드(S15)는 항상 방금 게시된
//                         파트가 실린 플레이리스트를 읽는다
//   SrsLlHlsSegment / SrsLlHlsPart
//                       ↔ OME FMP4Segment / FMP4Partial
//
// 기존 TS-HLS(S10, 디스크 + nginx)와 달리 디스크를 거치지 않는다 (PLANS.md D3):
// 파트는 수명이 짧고(0.5s) 블로킹 서빙(S15)과 결합해야 하므로 메모리에만 산다.
//
// 락 규율 (PLANS.md 리스크 5, CLAUDE.md §5.1):
//   - muxer는 hub 스레드(= source lock_ 안)에서만 실행된다 — 자체 락 불필요
//   - storage는 자체 mutex를 가진다. hub 스레드는 source lock 안에서 storage lock을
//     잡고, HTTP 스레드(S15)는 storage lock만 잡는다 — source lock 금지 (역전 방지)
#ifndef SRS_APP_LLHLS_HPP
#define SRS_APP_LLHLS_HPP

#include <srs_core.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <srs_kernel_io.hpp>

class SrsFormat;
class SrsRequest;
class SrsSharedPtrMessage;
class SrsOriginHub;
class SrsMp4M2tsSegmentEncoder;

// LL-HLS URI 규칙 — 플레이리스트(S14)와 HTTP 라우팅(S15)이 이 헬퍼 하나를 공유한다
// (한쪽만 바뀌어 어긋나는 사고 방지 — TASKS.md S14). m3u8이 /{app}/{stream}.m3u8에서
// 서빙되므로 m3u8 안의 상대 URI는 {stream}/으로 시작해 /{app}/{stream}/…로 해석된다.
extern std::string srs_llhls_init_uri(const std::string& stream);
extern std::string srs_llhls_part_uri(const std::string& stream, int64_t msn, int psn);
extern std::string srs_llhls_segment_uri(const std::string& stream, int64_t msn);

// fMP4 바이트를 메모리로 받는 ISrsWriter (파일을 쓰지 않는다 — PLANS.md D3).
class SrsLlHlsBufferWriter : public ISrsWriter
{
public:
    std::string data;
public:
    SrsLlHlsBufferWriter();
    virtual ~SrsLlHlsBufferWriter();
public:
    virtual srs_error_t write(void* buf, size_t size, ssize_t* nwrite);
    virtual srs_error_t writev(const iovec* iov, int iov_size, ssize_t* nwrite);
};

// 파트 1개 = CMAF 청크(styp+moof+mdat) 1개. m3u8의 EXT-X-PART 한 줄이 된다.
// (OME FMP4Partial 대응)
// 수명은 shared_ptr로 공유한다 (S18): HTTP 스레드가 storage lock 안에서 페이로드를
// 복사하지 않고 포인터만 받아 나가, 락 밖에서 소켓에 쓴다 — 윈도우에서 밀려나
// 삭제돼도 응답 중인 스레드는 자기 참조로 안전하다 (SrsSharedPtrMessage의
// refcount 팬아웃과 같은 취지 — CLAUDE.md §5.2).
class SrsLlHlsPart
{
public:
    // The part sequence number in the parent segment (0-based).
    int psn;
    // The duration of this part.
    srs_utime_t duration;
    // Whether part starts with a keyframe — EXT-X-PART의 INDEPENDENT=YES.
    bool independent;
    // The fMP4 bytes(styp+moof+mdat).
    std::string payload;
public:
    SrsLlHlsPart();
    virtual ~SrsLlHlsPart();
};
typedef std::shared_ptr<SrsLlHlsPart> SrsLlHlsPartPtr;

// 세그먼트 1개 = 파트들의 부모. 완결되면 m3u8의 EXTINF 한 줄이 된다.
// 세그먼트 전체 바이트는 따로 들지 않는다 — 파트들의 연결이 곧 세그먼트다.
// (OME FMP4Segment 대응)
class SrsLlHlsSegment
{
public:
    // The media sequence number in m3u8 (EXT-X-MEDIA-SEQUENCE 기준).
    int64_t msn;
    // The duration, sum of part durations — 완결 후 EXTINF 값.
    srs_utime_t duration;
    // Whether segment is completed (마지막 파트까지 게시됨).
    bool completed;
    // The parts of this segment (shared — SrsLlHlsPart 주석).
    std::deque<SrsLlHlsPartPtr> parts;
public:
    SrsLlHlsSegment(int64_t seq);
    virtual ~SrsLlHlsSegment();
};

// m3u8(미디어 플레이리스트) 텍스트 생성기. (OME LLHlsChunklist::MakeChunklist 대응)
// 상태 없는 포맷터다 — 세그먼트/파트 메타데이터는 storage가 락 안에서 넘겨 준다
// (generate는 storage lock_ 안에서 불린다 — 파일 상단 락 규율 참조).
class SrsLlHlsChunklist
{
private:
    // 설정 스냅샷 (update_config에서 채운다 — 기존 muxer와 같은 이디엄).
    std::string stream_;
    srs_utime_t part_target_;
    srs_utime_t segment_target_;
public:
    SrsLlHlsChunklist();
    virtual ~SrsLlHlsChunklist();
public:
    virtual void update_config(const std::string& stream, srs_utime_t part_target, srs_utime_t segment_target);
    // m3u8 전문을 생성한다. 세그먼트가 없으면 빈 문자열.
    // @param active publish 중인지 — PRELOAD-HINT는 다음 파트가 실제로 올 때만 싣는다.
    virtual std::string generate(const std::deque<SrsLlHlsSegment*>& segments, bool active);
};

// 인메모리 롤링 윈도우 + 게시 통지. (OME FMP4Storage 대응)
// hub 스레드가 append_part/set_init으로 쓰고, HTTP 스레드(S15)가 조회한다.
// wait_for가 블로킹 리로드/프리로드 힌트 홀드의 대기 지점. m3u8은 게시마다
// 재생성해 캐시하므로 HTTP 스레드는 get_playlist로 읽기만 한다 (S14).
class SrsLlHlsStorage
{
private:
    // 아래 상태 전부를 보호한다. HTTP 스레드는 이 락만 잡는다 (source lock 금지).
    std::mutex lock_;
    // 파트/세그먼트 게시와 unpublish마다 notify_all — 홀드된 요청이 깨어난다.
    std::condition_variable cond_;
    // Whether the stream is publishing. false면 wait_for가 즉시 반환한다.
    bool active_;
private:
    // The init.mp4 bytes (EXT-X-MAP이 가리키는 초기화 세그먼트).
    std::string init_;
    // The rolling window of segments. back()이 진행 중(미완결) 세그먼트일 수 있다.
    std::deque<SrsLlHlsSegment*> segments_;
    // The max segments to keep (llhls_segment_count).
    int max_segments_;
    // The next msn to assign. 재publish에도 이어진다 — msn은 스트림 수명 동안 단조 증가.
    int64_t next_msn_;
    // The latest published part position. (-1, -1) until the first part.
    int64_t latest_msn_;
    int latest_psn_;
private:
    // The m3u8 generator and its cached output (S14). 게시(append_part/set_init/
    // on_unpublish)마다 락 안에서 재생성한다 — notify_all 전에 항상 최신.
    SrsLlHlsChunklist* chunklist_;
    std::string playlist_;
public:
    SrsLlHlsStorage();
    virtual ~SrsLlHlsStorage();
public:
    virtual void update_config(int max_segments, const std::string& stream, srs_utime_t part_target, srs_utime_t segment_target);
    virtual void on_publish();
    // 게시 중단: active_ 해제 + notify_all — 홀드 중인 요청 전부 기상 (PLANS.md 리스크 6).
    virtual void on_unpublish();
    // Whether the stream is publishing — S15 홀드 루프가 unpublish를 감지해 끝낸다.
    virtual bool active();
public:
    virtual void set_init(const std::string& v);
    virtual bool get_init(std::string& v);
    // 캐시된 m3u8 (S14). 세그먼트가 하나도 없으면 false.
    virtual bool get_playlist(std::string& v);
    // Append one part. 열린 세그먼트가 없으면 새 msn으로 연다.
    // @param payload 파트 바이트 — rvalue면 복사 없이 넘겨받는다(muxer의 hot path, S18).
    // @param close_segment 이 파트가 부모 세그먼트의 마지막 파트 — 세그먼트 완결.
    virtual void append_part(std::string&& payload, srs_utime_t duration, bool independent, bool close_segment);
    virtual void append_part(const std::string& payload, srs_utime_t duration, bool independent, bool close_segment);
public:
    // 조회. 없으면(만료 포함) false. 포인터 반환판이 HTTP 서빙용(락 안 무복사 — S18),
    // string 반환판은 편의용(복사).
    virtual bool get_part(int64_t msn, int psn, SrsLlHlsPartPtr& part);
    virtual bool get_part(int64_t msn, int psn, std::string& payload);
    // 완결된 세그먼트 = 파트들의 연결. 미완결/없음이면 false.
    // 포인터 목록판은 호출자가 writev로 이어 쓴다 (세그먼트 바이트를 따로 만들지 않는다).
    virtual bool get_segment(int64_t msn, std::vector<SrsLlHlsPartPtr>& parts);
    virtual bool get_segment(int64_t msn, std::string& payload);
    // The oldest/latest msn in the window, -1 if empty.
    virtual int64_t first_msn();
    virtual int64_t latest_msn();
    // The latest published part position. false if no part yet.
    virtual bool latest(int64_t& msn, int& psn);
    virtual int size();
public:
    // 파트 (msn, psn)이 게시될 때까지 대기 (psn<0: msn의 아무 파트나).
    // 블로킹 리로드(_HLS_msn/_HLS_part)와 프리로드 힌트 GET(S15)의 대기 지점.
    // 항상 타임아웃부 — 게시/타임아웃/unpublish로 깨어난다 (PLANS.md 리스크 6).
    // @return 대상이 게시되었는지 (만료된 과거 msn도 true — 호출자는 조회 후 404).
    virtual bool wait_for(int64_t msn, int psn, srs_utime_t timeout);
private:
    // 호출자가 lock_을 잡은 상태여야 한다.
    virtual bool reached(int64_t msn, int psn);
    // msn은 윈도우 안에서 연속(next_msn_++ 후 앞에서만 pop)이라 front 기준 O(1) 인덱싱.
    virtual SrsLlHlsSegment* find(int64_t msn);
    virtual void shrink();
    virtual void refresh_playlist();
};

// 파트/세그먼트 컷 정책 + S12 fMP4 인코더 구동. (OME FMP4Packager 대응)
// hub 스레드 전용 — 자체 락 없음 (파일 상단 락 규율 참조).
class SrsLlHlsMuxer
{
private:
    SrsRequest* req;
    // The storage, owned by SrsLlHls.
    SrsLlHlsStorage* storage;
private:
    // 설정 스냅샷 (기존 SrsHlsMuxer::update_config와 같은 이디엄).
    srs_utime_t part_target;
    srs_utime_t segment_target;
private:
    // The fMP4 encoder of the current part, one-shot per part (S12).
    SrsMp4M2tsSegmentEncoder* enc;
    // The buffer of the current part bytes.
    SrsLlHlsBufferWriter writer;
    // The moof mfhd sequence number, 파트마다 증가 (재publish에도 이어진다).
    uint32_t sequence_;
private:
    // 진행 중 파트 상태.
    bool part_open_;
    int64_t part_start_dts;
    int64_t last_dts;
    int part_nb_samples;
    bool part_has_video;
    bool part_independent;
    // 진행 중 세그먼트의 닫힌 파트 duration 합.
    srs_utime_t segment_duration;
private:
    // Whether we need to regenerate init.mp4 at the next segment boundary.
    bool init_dirty_;
    // Whether the stream has a configured video track — pure-audio 판단
    // (오디오 프레임은 전부 independent라 세그먼트 컷을 오디오가 주도한다).
    bool video_configured_;
    // 마지막으로 본 시퀀스 헤더 원문 — 동일 재전송을 무시하기 위한 비교용.
    std::vector<char> latest_avc_extra_;
    std::vector<char> latest_aac_extra_;
public:
    SrsLlHlsMuxer();
    virtual ~SrsLlHlsMuxer();
public:
    virtual srs_error_t initialize(SrsLlHlsStorage* s);
    // On publish, update the config snapshot and reset the part state.
    virtual srs_error_t update_config(SrsRequest* r);
    // When got sequence header. 변경이면 세그먼트를 닫고 init 재생성을 예약한다 —
    // 다른 코덱 설정의 샘플이 한 세그먼트(=한 EXT-X-MAP)에 섞이지 않게
    // (OME FMP4Packager::UpdateTrack의 Flush+컷). 동일 재전송은 무시.
    virtual srs_error_t on_sequence_header(SrsFormat* format);
    // Write one frame. @param dts in milliseconds (fMP4 timescale=1000 직결 — D5).
    virtual srs_error_t write_audio(SrsFormat* format, int64_t dts);
    virtual srs_error_t write_video(SrsFormat* format, int64_t dts);
    // On unpublish, flush the pending part as the last of its segment.
    virtual srs_error_t on_unpublish();
private:
    // 컷 판단 — 프레임을 파트에 넣기 전에 호출한다.
    // 세그먼트 컷: can_end_segment(키프레임 도착 || pure-audio) && 누적 ≥ segment_target
    //   — 기존 TS 경로 is_segment_overflow+wait_keyframe와 동일 정책.
    // 파트 컷: 스펙 "파트 ≤ PART-TARGET && (예외 빼고) ≥ 85%×PART-TARGET"을
    //   OME처럼 만족시킨다 — 이 프레임까지 duration이 target을 넘으면 컷,
    //   85%를 넘었고 다음 프레임(간격 추정)이 target을 넘길 것 같으면 미리 컷.
    virtual srs_error_t maybe_cut(int64_t dts, bool can_end_segment);
    virtual srs_error_t open_part(SrsFormat* format, int64_t dts);
    // @param end_dts 파트의 끝 dts(ms) — 마지막 샘플 duration 계산(S12 flush)과
    //                파트 duration의 기준.
    virtual srs_error_t flush_part(int64_t end_dts, bool close_segment);
    // init.mp4 재생성 → storage 교체. 사용 가능한 코덱(video/audio)만 싣는다.
    virtual srs_error_t write_init(SrsFormat* format);
};

// The LL-HLS transmuxer: SrsOriginHub의 구성 요소 (기존 SrsHls와 대칭).
class SrsLlHls
{
private:
    SrsLlHlsMuxer* muxer;
    SrsLlHlsStorage* storage_;
private:
    SrsRequest* req;
    // Whether the LL-HLS is enabled (publish 중이고 llhls_enabled).
    bool enabled;
    SrsOriginHub* hub;
public:
    SrsLlHls();
    virtual ~SrsLlHls();
public:
    virtual srs_error_t initialize(SrsOriginHub* h, SrsRequest* r);
    virtual srs_error_t on_publish();
    virtual void on_unpublish();
    // When we get an audio/video message with parsed format.
    virtual srs_error_t on_audio(SrsSharedPtrMessage* shared_audio, SrsFormat* format);
    virtual srs_error_t on_video(SrsSharedPtrMessage* shared_video, SrsFormat* format);
public:
    // The storage for playlist(S14) and HTTP serving(S15).
    virtual SrsLlHlsStorage* storage();
};

#endif
