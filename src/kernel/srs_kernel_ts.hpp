// srs_simple — 원본: trunk/src/kernel/srs_kernel_ts.hpp (3,200줄+)
// MPEG-TS 먹서: RTMP 프레임(SrsFrame) → PES 페이로드 → 188바이트 TS 패킷.
//
//   SrsTsMessageCache  FLV/RTMP 페이로드를 PES 페이로드로 변환
//                      (AAC raw → ADTS 프레임, AVCC NALU → annex-b + AUD/SPS/PPS 삽입)
//   SrsTsMessage       PES 1개 분량의 페이로드 + 타이밍(90kHz dts/pts)
//   SrsTsContext       PAT/PMT/PES를 TS 패킷으로 직렬화. pid별 continuity counter 관리
//   SrsTsContextWriter 세그먼트 파일 1개에 대한 쓰기 파사드
//
// 원본 대비 단순화 (CLAUDE.md §5.6 S10):
//   - 디코더(demux) 경로 전체 제거 — HLS 쓰기 전용
//   - SrsTsPacket/SrsTsHeader/SrsTsAdaptationField/SrsTsPayloadPAT/PMT 클래스 트리 제거
//     → SrsTsContext가 188바이트 버퍼를 직접 조립 (encode_pat_pmt/encode_pes)
//   - H.264 + AAC만 (HEVC/MP3 제거), 고정 PID, AES 암호화 없음
#ifndef SRS_KERNEL_TS_HPP
#define SRS_KERNEL_TS_HPP

#include <srs_core.hpp>

#include <map>

#include <srs_kernel_codec.hpp>
#include <srs_kernel_io.hpp>

class SrsSimpleStream;
class SrsFileWriter;
class SrsAudioFrame;
class SrsVideoFrame;

// The ts packet size, 188 bytes fixed.
#define SRS_TS_PACKET_SIZE 188

// The pid and program number of ts (원본: srs_kernel_ts.cpp 상단 상수).
#define TS_PMT_NUMBER 1
#define TS_PMT_PID 0x1001
#define TS_VIDEO_AVC_PID 0x100
#define TS_AUDIO_AAC_PID 0x101

// The type of ts stream in PMT.
// @doc ISO_IEC_13818-1-2000.pdf, page 66, Table 2-29
enum SrsTsStream
{
    SrsTsStreamReserved = 0x00,
    // ISO/IEC 13818-7 Audio with ADTS transport syntax
    SrsTsStreamAudioAAC = 0x0f,
    // H.264
    SrsTsStreamVideoH264 = 0x1b,
};

// The stream_id of PES packet.
// @doc ISO_IEC_13818-1-2000.pdf, page 53, Table 2-18
enum SrsTsPESStreamId
{
    // 110x xxxx: ISO/IEC 13818-3 audio stream number 0
    SrsTsPESStreamIdAudioCommon = 0xc0,
    // 1110 xxxx: H.262/H.264 video stream number 0
    SrsTsPESStreamIdVideoCommon = 0xe0,
};

// The ts message, PES 1개 분량의 페이로드.
// SrsTsMessageCache가 만들고 SrsTsContext::encode가 소비한다.
class SrsTsMessage
{
public:
    // The dts/pts in 90kHz clock (RTMP ms * 90).
    int64_t dts;
    int64_t pts;
    // The PES stream id.
    SrsTsPESStreamId sid;
    // Whether to write PCR in the first ts packet (video IDR, 또는 pure-audio의 audio).
    bool write_pcr;
    // The PES payload: ADTS 프레임(audio) 또는 annex-b NALU들(video).
    SrsSimpleStream* payload;
public:
    SrsTsMessage();
    virtual ~SrsTsMessage();
public:
    virtual bool is_audio();
    virtual bool is_video();
};

// The ts channel: pid별 상태 (여기서는 continuity counter만).
class SrsTsChannel
{
public:
    int pid;
    SrsTsStream stream;
    // The continuity counter, 페이로드가 있는 패킷마다 증가 (mod 16).
    uint8_t continuity_counter;
public:
    SrsTsChannel();
    virtual ~SrsTsChannel();
};

// The ts context, PAT/PMT/PES를 188바이트 TS 패킷으로 직렬화한다.
// 세그먼트가 열릴 때 reset()하면 다음 encode가 PAT/PMT부터 다시 쓴다
// (HLS 세그먼트는 독립 디코딩 가능해야 하므로 매 세그먼트가 PAT/PMT로 시작).
class SrsTsContext
{
private:
    // 마지막으로 PAT/PMT에 기록한 코덱 — 달라지면 PAT/PMT를 다시 쓴다.
    SrsVideoCodecId vcodec;
    SrsAudioCodecId acodec;
    // The pid => channel map.
    std::map<int, SrsTsChannel*> pids;
public:
    SrsTsContext();
    virtual ~SrsTsContext();
public:
    // Reset the context, then the next encode will write PAT/PMT first.
    virtual void reset();
public:
    // Encode the ts message to writer, in 188 bytes ts packets.
    // @param vc/ac 현재 스트림의 코덱 — PAT/PMT의 ES 구성과 PCR PID를 결정한다.
    virtual srs_error_t encode(ISrsWriter* writer, SrsTsMessage* msg, SrsVideoCodecId vc, SrsAudioCodecId ac);
private:
    virtual SrsTsChannel* channel(int pid, SrsTsStream stream);
    virtual srs_error_t encode_pat_pmt(ISrsWriter* writer, int16_t vpid, SrsTsStream vs, int16_t apid, SrsTsStream as);
    virtual srs_error_t encode_pes(ISrsWriter* writer, SrsTsMessage* msg, int16_t pid);
};

// The wrapper of ts context for one segment file:
// 세그먼트 파일 라이터와 현재 코덱을 붙들고 context->encode로 위임한다.
class SrsTsContextWriter
{
private:
    // The context, muxer가 소유하며 세그먼트를 넘어 공유한다 (continuity counter 유지).
    SrsTsContext* context;
    // The file writer, segment가 소유한다.
    SrsFileWriter* writer;
private:
    SrsAudioCodecId acodec_;
    SrsVideoCodecId vcodec_;
public:
    SrsTsContextWriter(SrsFileWriter* w, SrsTsContext* c, SrsAudioCodecId ac, SrsVideoCodecId vc);
    virtual ~SrsTsContextWriter();
public:
    // Write an audio/video frame to ts file.
    virtual srs_error_t write_audio(SrsTsMessage* audio);
    virtual srs_error_t write_video(SrsTsMessage* video);
public:
    virtual SrsVideoCodecId vcodec();
    virtual void set_vcodec(SrsVideoCodecId v);
    virtual SrsAudioCodecId acodec();
    virtual void set_acodec(SrsAudioCodecId v);
};

// The queue of ts message to build PES payload:
// FLV/RTMP 프레임을 받아 PES 페이로드(ADTS/annex-b)로 변환해 SrsTsMessage에 쌓는다.
// flush(원본은 SrsHlsMuxer::flush_audio/flush_video)가 꺼내 쓰고 비운다.
class SrsTsMessageCache
{
public:
    SrsTsMessage* audio;
    SrsTsMessage* video;
public:
    SrsTsMessageCache();
    virtual ~SrsTsMessageCache();
public:
    // Write audio frame to cache. @param dts in 90kHz.
    virtual srs_error_t cache_audio(SrsAudioFrame* frame, int64_t dts);
    // Write video frame to cache. @param dts in 90kHz.
    virtual srs_error_t cache_video(SrsVideoFrame* frame, int64_t dts);
private:
    // AAC raw → ADTS 프레임 (7바이트 ADTS 헤더 생성).
    virtual srs_error_t do_cache_aac(SrsAudioFrame* frame);
    // AVCC NALU → annex-b (+ AUD 삽입, IDR 앞 SPS/PPS 재삽입).
    virtual srs_error_t do_cache_avc(SrsVideoFrame* frame);
};

// The MPEG-2 CRC32 for PSI(PAT/PMT) section.
// (원본: kernel/srs_kernel_utility.cpp의 srs_crc32_mpegts — utility 파일이 없어 여기로)
extern uint32_t srs_crc32_mpegts(const void* buf, int size);

#endif
