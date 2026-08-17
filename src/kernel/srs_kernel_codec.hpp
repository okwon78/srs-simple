// srs_simple — 원본: trunk/src/kernel/srs_kernel_codec.hpp
// FLV 태그 첫 1~2바이트로 시퀀스 헤더/키프레임을 판별하는 부분만 유지.
// 레거시 FLV 헤더만 지원 (enhanced-RTMP/HEVC ext header 제외 — CLAUDE.md §1 비목표).
// 전체 코덱 파싱(SrsFormat/SrsVideoFrame 등)은 라이브 릴레이에 불필요해 제거.
#ifndef SRS_KERNEL_CODEC_HPP
#define SRS_KERNEL_CODEC_HPP

#include <srs_core.hpp>

// The video codec id, FLV tag 첫 바이트의 하위 4비트.
// @doc video_file_format_spec_v10_1.pdf, E.4.3.1 VIDEODATA CodecID
enum SrsVideoCodecId
{
    SrsVideoCodecIdForbidden = 0,
    SrsVideoCodecIdAVC = 7,
};

// The video frame type, FLV tag 첫 바이트의 상위 4비트.
// @doc video_file_format_spec_v10_1.pdf, E.4.3.1 VIDEODATA Frame Type
enum SrsVideoAvcFrameType
{
    SrsVideoAvcFrameTypeForbidden = 0,
    SrsVideoAvcFrameTypeKeyFrame = 1,
    SrsVideoAvcFrameTypeInterFrame = 2,
};

// The video AVC packet type, FLV tag 두 번째 바이트.
// @doc video_file_format_spec_v10_1.pdf, E.4.3.1 AVCVIDEOPACKET AVCPacketType
enum SrsVideoAvcFrameTrait
{
    // sequence header = AVCDecoderConfigurationRecord (SPS/PPS)
    SrsVideoAvcFrameTraitSequenceHeader = 0,
    SrsVideoAvcFrameTraitNALU = 1,
};

// The audio codec id, FLV tag 첫 바이트의 상위 4비트 (SoundFormat).
// @doc video_file_format_spec_v10_1.pdf, E.4.2.1 AUDIODATA SoundFormat
enum SrsAudioCodecId
{
    SrsAudioCodecIdForbidden = 0,
    SrsAudioCodecIdAAC = 10,
};

// The audio AAC packet type, FLV tag 두 번째 바이트.
// @doc video_file_format_spec_v10_1.pdf, E.4.2.1 AACAUDIODATA AACPacketType
enum SrsAudioAacFrameTrait
{
    // sequence header = AudioSpecificConfig
    SrsAudioAacFrameTraitSequenceHeader = 0,
    SrsAudioAacFrameTraitRawData = 1,
};

// The helper to judge the flv video payload.
// 시퀀스 헤더(SPS/PPS)는 MetaCache로, 키프레임은 GopCache의 clear 트리거로 쓰인다 (S8).
class SrsFlvVideo
{
public:
    SrsFlvVideo();
    virtual ~SrsFlvVideo();
public:
    static bool keyframe(char* data, int size);
    // Whether the video is sequence header (AVC packet type = 0, keyframe).
    static bool sh(char* data, int size);
    // Whether the video is H.264.
    static bool h264(char* data, int size);
};

// The helper to judge the flv audio payload.
class SrsFlvAudio
{
public:
    SrsFlvAudio();
    virtual ~SrsFlvAudio();
public:
    // Whether the audio is sequence header (AAC packet type = 0).
    static bool sh(char* data, int size);
    // Whether the audio is AAC.
    static bool aac(char* data, int size);
};

#endif
