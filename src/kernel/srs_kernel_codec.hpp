// srs_simple — 원본: trunk/src/kernel/srs_kernel_codec.hpp
// S1~S9: FLV 태그 첫 1~2바이트로 시퀀스 헤더/키프레임을 판별하는 부분만 유지.
// S10(HLS): TS 먹싱에 필요한 코덱 파싱(SrsFormat 계열)을 원본에서 추가로 가져왔다.
//   - avcC(AVCDecoderConfigurationRecord) → SPS/PPS, NALU 길이 프리픽스 크기
//   - AVCC 페이로드 → NALU 샘플 목록 (annex-b 변환은 srs_kernel_ts가 한다)
//   - AudioSpecificConfig → ADTS 헤더 생성에 필요한 object/sample_rate/channels
// 레거시 FLV 헤더만 지원 (enhanced-RTMP/HEVC ext header 제외 — CLAUDE.md §1 비목표).
// 원본 대비 제거: SPS 비트스트림 파싱(해상도/fps), HEVC/AV1, MP3/Opus — CLAUDE.md §5.6 S10.
#ifndef SRS_KERNEL_CODEC_HPP
#define SRS_KERNEL_CODEC_HPP

#include <srs_core.hpp>

#include <vector>

class SrsBuffer;

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
    // ffmpeg가 스트림 끝에 보내는 end of sequence (payload 없음)
    SrsVideoAvcFrameTraitSequenceHeaderEOF = 2,
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

// The avc nalu type. (원본 전체 목록 0~31 중 필요한 값만 — 이름/값은 원본과 동일)
// @doc ISO_IEC_14496-10-AVC-2003.pdf, page 44, 7.3.1 NAL unit syntax
enum SrsAvcNaluType
{
    SrsAvcNaluTypeForbidden = 0,
    // Coded slice of a non-IDR picture slice_layer_without_partitioning_rbsp( )
    SrsAvcNaluTypeNonIDR = 1,
    // Coded slice of an IDR picture slice_layer_without_partitioning_rbsp( )
    SrsAvcNaluTypeIDR = 5,
    // Supplemental enhancement information (SEI) sei_rbsp( )
    SrsAvcNaluTypeSEI = 6,
    // Sequence parameter set seq_parameter_set_rbsp( )
    SrsAvcNaluTypeSPS = 7,
    // Picture parameter set pic_parameter_set_rbsp( )
    SrsAvcNaluTypePPS = 8,
    // Access unit delimiter access_unit_delimiter_rbsp( )
    SrsAvcNaluTypeAccessUnitDelimiter = 9,
};

// The aac object type, for RTMP sequence header (AudioSpecificConfig audioObjectType).
// @doc ISO_IEC_14496-3-AAC-2001.pdf, page 23, 1.5.1.1 Audio object type definition
enum SrsAacObjectType
{
    SrsAacObjectTypeForbidden = 0,
    SrsAacObjectTypeAacMain = 1,
    SrsAacObjectTypeAacLC = 2,
    SrsAacObjectTypeAacSSR = 3,
    // AAC HE = LC+SBR
    SrsAacObjectTypeAacHE = 5,
    // AAC HEv2 = LC+SBR+PS
    SrsAacObjectTypeAacHEV2 = 29,
};

// The profile for adts (ADTS 헤더의 2비트 profile 필드 — ASC의 object type과 값이 다르다).
// @doc ISO_IEC_13818-7-AAC-2004.pdf, page 40, Table 31
enum SrsAacProfile
{
    SrsAacProfileMain = 0,
    SrsAacProfileLC = 1,
    SrsAacProfileSSR = 2,
    // Reserved profile.
    SrsAacProfileReserved = 3,
};

// Convert aac object type in RTMP sequence header to aac profile of ADTS.
extern SrsAacProfile srs_aac_rtmp2ts(SrsAacObjectType object_type);

// The audio sample rate table of AudioSpecificConfig samplingFrequencyIndex.
// @doc ISO_IEC_14496-3-AAC-2001.pdf, page 33, Table 1.6.2
extern int srs_aac_srates[];

// The sample in frame: a NALU for video, or an AAC raw frame for audio.
// bytes는 메시지 페이로드 내부를 가리키는 뷰일 뿐, 소유하지 않는다.
class SrsSample
{
public:
    // The size of unit.
    int size;
    // The ptr of unit, user must free it.
    char* bytes;
public:
    SrsSample();
    ~SrsSample();
};

// The audio codec info, parsed from the AAC sequence header (AudioSpecificConfig).
class SrsAudioCodecConfig
{
public:
    SrsAudioCodecId id;
public:
    // The audio specific config fields, for ADTS header generation (srs_kernel_ts).
    SrsAacObjectType aac_object;
    // The samplingFrequencyIndex, index of srs_aac_srates.
    uint8_t aac_sample_rate;
    // The channelConfiguration.
    uint8_t aac_channels;
public:
    SrsAudioCodecConfig();
    virtual ~SrsAudioCodecConfig();
public:
    virtual bool is_aac_codec_ok();
};

// The video codec info, parsed from the AVC sequence header (avcC).
class SrsVideoCodecConfig
{
public:
    SrsVideoCodecId id;
public:
    // The lengthSizeMinusOne of avcC: NALU 길이 프리픽스가 (이 값+1)바이트다.
    // 0 → 1B, 1 → 2B, 3 → 4B (2는 스펙상 금지).
    int8_t NAL_unit_length;
    // The sps/pps NALU raw bytes, without length prefix.
    std::vector<char> sequenceParameterSetNALUnit;
    std::vector<char> pictureParameterSetNALUnit;
public:
    SrsVideoCodecConfig();
    virtual ~SrsVideoCodecConfig();
public:
    virtual bool is_avc_codec_ok();
};

// The max number of samples in a frame. (원본: kernel_codec.hpp SrsMaxNbSamples)
#define SrsMaxNbSamples 256

// The frame of audio/video: the parsed samples of one message payload.
class SrsFrame
{
public:
    // The DTS/PTS in milliseconds (FLV 타임스탬프).
    int64_t dts;
    // The composition time offset in ms (video만, B-frame 재정렬용. PTS = DTS + CTS).
    int32_t cts;
public:
    // The samples in frame.
    SrsSample samples[SrsMaxNbSamples];
    // The number of samples.
    int nb_samples;
public:
    SrsFrame();
    virtual ~SrsFrame();
public:
    // Add a sample to frame.
    virtual srs_error_t add_sample(char* bytes, int size);
};

// The audio frame: AAC raw data samples.
class SrsAudioFrame : public SrsFrame
{
public:
    SrsAudioAacFrameTrait aac_packet_type;
public:
    // The codec config of this frame, owned by SrsFormat.
    // (원본은 SrsFrame::initialize(SrsCodecConfig*)로 주입 — 여기서는 SrsFormat이 직접 대입)
    SrsAudioCodecConfig* acodec_;
public:
    SrsAudioFrame();
    virtual ~SrsAudioFrame();
public:
    virtual SrsAudioCodecConfig* acodec();
};

// The video frame: NALU samples.
class SrsVideoFrame : public SrsFrame
{
public:
    // The frame type of FLV tag (keyframe/inter frame).
    SrsVideoAvcFrameType frame_type;
    // The avc packet type (sequence header/NALU).
    SrsVideoAvcFrameTrait avc_packet_type;
    // Whether frame contains IDR NALU (nal_unit_type 5).
    bool has_idr;
public:
    // The codec config of this frame, owned by SrsFormat.
    SrsVideoCodecConfig* vcodec_;
public:
    SrsVideoFrame();
    virtual ~SrsVideoFrame();
public:
    virtual SrsVideoCodecConfig* vcodec();
};

// A codec format parser: FLV 태그 페이로드를 파싱해 코덱 설정(시퀀스 헤더)과
// 프레임 샘플(NALU/AAC raw)을 꺼낸다. HLS(TS 먹싱)의 입력이 된다.
// 원본은 SrsLiveSource가 SrsRtmpFormat(SrsFormat 파생)을 소유하지만,
// 여기서는 SrsOriginHub가 SrsFormat을 직접 소유한다 — CLAUDE.md §5.6 S10.
class SrsFormat
{
public:
    // The parsed frame of last on_audio/on_video. NULL until first frame.
    SrsAudioFrame* audio;
    SrsVideoFrame* video;
    // The parsed codec config. NULL until first frame; config는 시퀀스 헤더에서 채워진다.
    SrsAudioCodecConfig* acodec;
    SrsVideoCodecConfig* vcodec;
public:
    SrsFormat();
    virtual ~SrsFormat();
public:
    virtual srs_error_t initialize();
    // When got audio/video FLV tag payload.
    virtual srs_error_t on_audio(int64_t timestamp, char* data, int size);
    virtual srs_error_t on_video(int64_t timestamp, char* data, int size);
private:
    virtual srs_error_t audio_aac_demux(SrsBuffer* buffer, int64_t timestamp);
    virtual srs_error_t audio_aac_sequence_header_demux(char* data, int size);
    virtual srs_error_t video_avc_demux(SrsBuffer* buffer, int64_t timestamp);
    // Parse the H.264 SPS/PPS from sequence header (avcC).
    virtual srs_error_t avc_demux_sps_pps(SrsBuffer* stream);
    // Parse the NALU samples from AVCC (length-prefixed) payload.
    virtual srs_error_t avc_demux_ibmf_format(SrsBuffer* stream);
};

#endif
