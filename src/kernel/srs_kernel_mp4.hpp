// srs_simple — 원본: trunk/src/kernel/srs_kernel_mp4.hpp (9,000줄+)
// fMP4(CMAF) 인코더: LL-HLS의 init.mp4와 m4s(moof+mdat) 파트를 만든다 (S12).
// 원본 SRS에는 LL-HLS가 없어 DASH 경로의 두 인코더 이름을 미러링한다 (PLANS.md D1):
//
//   SrsMp4M2tsInitEncoder     init.mp4 = ftyp + moov (시퀀스 헤더 → avcC/esds)
//   SrsMp4M2tsSegmentEncoder  m4s = styp + moof + mdat (raw AVCC/AAC 샘플 누적 후 flush)
//
// 원본 대비 단순화 (S10 TS 먹서와 같은 방식 — CLAUDE.md §5.6):
//   - 디코더(SrsMp4Decoder)와 박스 클래스 트리(SrsMp4Box 계층 100여 종) 제거
//     → 인코더가 SrsBuffer로 박스 바이트를 직접 조립 (바이트 레이아웃은 원본과 동일)
//   - sidx 제거 — DASH의 AV 싱크용이며 LL-HLS 파트에는 불필요
//   - H.264 + AAC만 (HEVC 분기 제거)
//
// 원본과 다른 확장 (muxed 단일 파일 — PLANS.md D2, CLAUDE.md §5.6 S12):
//   - 원본 DASH는 트랙별 파일(init encoder는 trak 1개, segment encoder는 traf 1개).
//     우리는 video(tid=1)+audio(tid=2)를 한 파일에 싣는다:
//     init은 write(format) 오버로드가 trak 2개를 한 moov에, segment는 flush가
//     샘플이 있는 트랙마다 traf를 쓴다 (video traf가 tid, audio traf는 tid+1).
//   - tfdt는 원본처럼 basetime이 아니라 트랙별 첫 샘플 dts — muxed에서는 트랙마다
//     시작 dts가 달라 basetime 공유가 타임라인을 틀어지게 한다
//   - trun sample_flags는 원본의 "첫 샘플만 0x02000000" 대신 프레임 단위:
//     키프레임 0x02000000, 비키프레임 0x01010000(non-sync), 오디오 0x02000000
//     — LL-HLS 파트는 키프레임으로 시작하지 않을 수 있어 정확한 마킹이 필요하다
#ifndef SRS_KERNEL_MP4_HPP
#define SRS_KERNEL_MP4_HPP

#include <srs_core.hpp>

#include <string>
#include <vector>

#include <srs_kernel_codec.hpp>
#include <srs_kernel_io.hpp>

class SrsBuffer;

// The handler type in mdia/hdlr box, fourcc. (원본: srs_kernel_mp4.hpp SrsMp4HandlerType)
enum SrsMp4HandlerType
{
    SrsMp4HandlerTypeForbidden = 0x00,
    // 'vide'
    SrsMp4HandlerTypeVIDE = 0x76696465,
    // 'soun'
    SrsMp4HandlerTypeSOUN = 0x736f756e,
};

// A cached sample of one fMP4 fragment. (원본: SrsMp4Sample — offset/adjust 등 제거,
// type은 SrsFrameType 대신 handler type을 그대로 든다)
// 원본과 달리 샘플 바이트를 들지 않는다(메타데이터만) — 바이트는 도착 즉시
// 인코더의 트랙별 연속 버퍼(mdat 본문)에 이어 붙인다 (CLAUDE.md §5.6 S18:
// 샘플마다 new[]+memcpy 하던 것을 파트당 버퍼 1개로 — hub 스레드 힙 할당 제거).
class SrsMp4Sample
{
public:
    // The type of sample, audio(SOUN) or video(VIDE).
    SrsMp4HandlerType type;
    // The dts/pts in tbn(=1000, ms).
    uint64_t dts;
    uint64_t pts;
    // For video, the frame type, whether keyframe.
    SrsVideoAvcFrameType frame_type;
    // The sample size in bytes (바이트 자체는 인코더의 트랙 버퍼에 있다).
    uint32_t nb_data;
public:
    SrsMp4Sample();
    virtual ~SrsMp4Sample();
};

// The samples of one fMP4 fragment. (원본: SrsMp4SampleManager — moov(stbl) 쓰기와
// 디코드용 load 제거, moof(trun) 조립은 SrsMp4M2tsSegmentEncoder::flush가 직접 한다)
// 샘플은 포인터가 아니라 값으로 보관한다 — 메타데이터뿐이라 힙 객체가 필요 없다 (S18).
class SrsMp4SampleManager
{
public:
    std::vector<SrsMp4Sample> samples;
public:
    SrsMp4SampleManager();
    virtual ~SrsMp4SampleManager();
public:
    // Append the sample to the tail of the manager.
    virtual void append(const SrsMp4Sample& sample);
};

// A fMP4 encoder, to write the init.mp4 with sequence header.
// (원본: srs_kernel_mp4.hpp:2145)
class SrsMp4M2tsInitEncoder
{
private:
    ISrsWriter* writer;
public:
    SrsMp4M2tsInitEncoder();
    virtual ~SrsMp4M2tsInitEncoder();
public:
    // Initialize the encoder with a writer w.
    virtual srs_error_t initialize(ISrsWriter* w);
    // Write the sequence header — 원본 시그니처: 트랙 1개(video 또는 audio)만 담은 init.mp4.
    virtual srs_error_t write(SrsFormat* format, bool video, int tid);
    // Muxed 확장(D2): video(tid=1) + audio(tid=2) 두 trak을 한 moov에.
    // format의 vcodec(avcC)/acodec(ASC)이 파싱돼 있어야 한다.
    virtual srs_error_t write(SrsFormat* format);
private:
    virtual srs_error_t do_write(SrsFormat* format, bool has_video, int vid, bool has_audio, int aid);
    // The trak box for video(avc1+avcC) or audio(mp4a+esds).
    virtual void write_video_trak(SrsBuffer* b, SrsVideoCodecConfig* vcodec, int tid);
    virtual void write_audio_trak(SrsBuffer* b, SrsAudioCodecConfig* acodec, int tid);
};

// A fMP4 encoder, to cache samples then flush as one fragment(moof+mdat),
// because the fMP4 should write trun box before mdat. One-shot: 파트/세그먼트마다
// 새 인코더를 만든다 (원본 DASH의 SrsFragmentedMp4도 fragment마다 새로 만든다).
// (원본: srs_kernel_mp4.hpp:2161)
class SrsMp4M2tsSegmentEncoder
{
private:
    ISrsWriter* writer;
    uint32_t sequence_number;
    srs_utime_t decode_basetime;
    uint32_t track_id;
    // audio traf의 track id. 0이면 미지정 — 파트 내용으로 추론한다(비디오 샘플이
    // 있으면 track_id+1, 없으면 track_id). muxed 스트림에서는 파트에 우연히 비디오가
    // 없어도(세그먼트 꼬리의 오디오 전용 파트) audio가 init의 오디오 트랙(2)을
    // 가리켜야 하므로, 호출자가 set_audio_tid로 스트림 구성 기준의 값을 명시한다.
    uint32_t audio_tid_;
private:
    uint32_t nb_audios;
    uint32_t nb_videos;
    uint64_t mdat_bytes;
    SrsMp4SampleManager* samples;
    // mdat 본문을 트랙별로 미리 이어 붙인다 — mdat은 [video 샘플들][audio 샘플들]
    // 순서이고 trun의 data_offset은 트랙당 하나라 트랙별로만 연속이면 된다.
    // write_sample이 여기에 memcpy 1회, flush는 moof/mdat 헤더와 함께 writev 1회 (S18).
    std::string video_data_;
    std::string audio_data_;
public:
    SrsMp4M2tsSegmentEncoder();
    virtual ~SrsMp4M2tsSegmentEncoder();
public:
    // Initialize the encoder with a writer w, then write the styp box.
    // @param tid muxed 확장: video traf의 track id(=1). audio traf는 tid+1을 쓴다.
    //            트랙이 하나뿐이면 그 traf가 tid를 쓴다 (원본 시그니처 유지 — D2).
    virtual srs_error_t initialize(ISrsWriter* w, uint32_t sequence, srs_utime_t basetime, uint32_t tid);
    // Muxed 확장(D2): audio traf의 track id를 스트림 구성 기준으로 고정한다
    // (muxed A/V면 2, 오디오 단독 스트림이면 1). 미호출 시 파트 내용으로 추론하는데,
    // muxed 스트림의 오디오 전용 파트가 비디오 트랙에 실리는 함정이 있다 — audio_tid_ 주석.
    virtual void set_audio_tid(uint32_t tid);
    // Cache a sample.
    // @param ht, The sample handler type, audio/soun or video/vide.
    // @param ft, The frame type. For video, it's SrsVideoAvcFrameType.
    // @param dts/pts in milliseconds (tbn=1000).
    // @remark All samples are RAW AAC/AVC data(길이 프리픽스 포함 AVCC),
    //         because the sequence header is written to init.mp4.
    virtual srs_error_t write_sample(SrsMp4HandlerType ht, uint16_t ft,
        uint32_t dts, uint32_t pts, uint8_t* sample, uint32_t nb_sample);
    // Flush the encoder, to write the moof and mdat.
    // @param dts The end dts in ms of this fragment(다음 프레임의 dts) —
    //            각 트랙 마지막 샘플의 duration 계산에 쓴다.
    virtual srs_error_t flush(uint64_t& dts);
private:
    // Write one traf(tfhd+tfdt+trun) for track ht. trun의 data_offset 자리는
    // 0으로 두고 위치만 기록한다 — moof 크기 확정 후 flush가 역산해 패치.
    virtual void write_traf(SrsBuffer* b, SrsMp4HandlerType ht, uint32_t tid,
        uint64_t end_dts, int* pdata_offset_pos);
};

#endif
