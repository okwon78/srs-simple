// srs_simple — 원본: trunk/src/kernel/srs_kernel_codec.cpp
// S1~S9: SrsFlvVideo/SrsFlvAudio 판별자. S10(HLS): SrsFormat 코덱 파싱 추가.
#include <srs_kernel_codec.hpp>

#include <string.h>

#include <srs_kernel_buffer.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

SrsFlvVideo::SrsFlvVideo()
{
}

SrsFlvVideo::~SrsFlvVideo()
{
}

bool SrsFlvVideo::keyframe(char* data, int size)
{
    // 1bytes required.
    if (size < 1) {
        return false;
    }

    // See video_file_format_spec_v10_1.pdf
    // 0x80 비트는 enhanced-RTMP ext header 표시 — 레거시만 지원하므로 벗겨내고 판별.
    uint8_t frame_type = data[0] & 0x7f;
    frame_type = (frame_type >> 4) & 0x0F;

    return frame_type == SrsVideoAvcFrameTypeKeyFrame;
}

bool SrsFlvVideo::sh(char* data, int size)
{
    // Sequence header only for H.264.
    if (!h264(data, size)) {
        return false;
    }

    // 2bytes required.
    if (size < 2) {
        return false;
    }

    uint8_t frame_type = (data[0] >> 4) & 0x0F;
    uint8_t avc_packet_type = data[1];

    // Sequence header is the keyframe with AVCDecoderConfigurationRecord.
    return frame_type == SrsVideoAvcFrameTypeKeyFrame
        && avc_packet_type == SrsVideoAvcFrameTraitSequenceHeader;
}

bool SrsFlvVideo::h264(char* data, int size)
{
    // 1bytes required.
    if (size < 1) {
        return false;
    }

    char codec_id = data[0];
    codec_id = codec_id & 0x0F;

    return codec_id == SrsVideoCodecIdAVC;
}

SrsFlvAudio::SrsFlvAudio()
{
}

SrsFlvAudio::~SrsFlvAudio()
{
}

bool SrsFlvAudio::sh(char* data, int size)
{
    // Sequence header only for AAC.
    if (!aac(data, size)) {
        return false;
    }

    // 2bytes required.
    if (size < 2) {
        return false;
    }

    char aac_packet_type = data[1];

    return aac_packet_type == SrsAudioAacFrameTraitSequenceHeader;
}

bool SrsFlvAudio::aac(char* data, int size)
{
    // 1bytes required.
    if (size < 1) {
        return false;
    }

    char sound_format = data[0];
    sound_format = (sound_format >> 4) & 0x0F;

    return sound_format == SrsAudioCodecIdAAC;
}

// The aac sample rate table (원본: srs_kernel_codec.cpp의 srs_aac_srates).
int srs_aac_srates[] = {
    96000, 88200, 64000, 48000,
    44100, 32000, 24000, 22050,
    16000, 12000, 11025,  8000,
     7350,     0,     0,     0
};

SrsAacProfile srs_aac_rtmp2ts(SrsAacObjectType object_type)
{
    switch (object_type) {
        case SrsAacObjectTypeAacMain:
            return SrsAacProfileMain;
        // HE/HEv2의 코어는 LC — SBR/PS는 ADTS에서 암묵 시그널링된다.
        case SrsAacObjectTypeAacHE:
        case SrsAacObjectTypeAacHEV2:
        case SrsAacObjectTypeAacLC:
            return SrsAacProfileLC;
        case SrsAacObjectTypeAacSSR:
            return SrsAacProfileSSR;
        default:
            return SrsAacProfileReserved;
    }
}

SrsSample::SrsSample()
{
    size = 0;
    bytes = NULL;
}

SrsSample::~SrsSample()
{
}

SrsAudioCodecConfig::SrsAudioCodecConfig()
{
    id = SrsAudioCodecIdForbidden;
    aac_object = SrsAacObjectTypeForbidden;
    aac_sample_rate = 0;
    aac_channels = 0;
}

SrsAudioCodecConfig::~SrsAudioCodecConfig()
{
}

bool SrsAudioCodecConfig::is_aac_codec_ok()
{
    // 시퀀스 헤더(AudioSpecificConfig)를 파싱했는지 여부.
    return aac_object != SrsAacObjectTypeForbidden;
}

SrsVideoCodecConfig::SrsVideoCodecConfig()
{
    id = SrsVideoCodecIdForbidden;
    NAL_unit_length = 0;
}

SrsVideoCodecConfig::~SrsVideoCodecConfig()
{
}

bool SrsVideoCodecConfig::is_avc_codec_ok()
{
    // 시퀀스 헤더(avcC)에서 SPS를 얻었는지 여부.
    return !sequenceParameterSetNALUnit.empty();
}

SrsFrame::SrsFrame()
{
    dts = 0;
    cts = 0;
    nb_samples = 0;
}

SrsFrame::~SrsFrame()
{
}

srs_error_t SrsFrame::add_sample(char* bytes, int size)
{
    srs_error_t err = srs_success;

    if (nb_samples >= SrsMaxNbSamples) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "Frame samples overflow, max=%d", SrsMaxNbSamples);
    }

    SrsSample* sample = &samples[nb_samples++];
    sample->bytes = bytes;
    sample->size = size;

    return err;
}

SrsAudioFrame::SrsAudioFrame()
{
    aac_packet_type = SrsAudioAacFrameTraitSequenceHeader;
    acodec_ = NULL;
}

SrsAudioFrame::~SrsAudioFrame()
{
}

SrsAudioCodecConfig* SrsAudioFrame::acodec()
{
    return acodec_;
}

SrsVideoFrame::SrsVideoFrame()
{
    frame_type = SrsVideoAvcFrameTypeForbidden;
    avc_packet_type = SrsVideoAvcFrameTraitSequenceHeader;
    has_idr = false;
    vcodec_ = NULL;
}

SrsVideoFrame::~SrsVideoFrame()
{
}

SrsVideoCodecConfig* SrsVideoFrame::vcodec()
{
    return vcodec_;
}

SrsFormat::SrsFormat()
{
    audio = NULL;
    video = NULL;
    acodec = NULL;
    vcodec = NULL;
}

SrsFormat::~SrsFormat()
{
    srs_freep(audio);
    srs_freep(video);
    srs_freep(acodec);
    srs_freep(vcodec);
}

srs_error_t SrsFormat::initialize()
{
    return srs_success;
}

srs_error_t SrsFormat::on_audio(int64_t timestamp, char* data, int size)
{
    srs_error_t err = srs_success;

    if (!data || size <= 0) {
        return err;
    }

    // Lazy create the audio frame and codec config.
    if (!acodec) {
        acodec = new SrsAudioCodecConfig();
    }
    if (!audio) {
        audio = new SrsAudioFrame();
        audio->acodec_ = acodec;
    }

    SrsBuffer buffer(data, size);
    return audio_aac_demux(&buffer, timestamp);
}

srs_error_t SrsFormat::audio_aac_demux(SrsBuffer* buffer, int64_t timestamp)
{
    srs_error_t err = srs_success;

    // Reset the frame for each message.
    audio->nb_samples = 0;
    audio->dts = timestamp;
    audio->cts = 0;

    // @doc video_file_format_spec_v10_1.pdf, page 76, E.4.2 Audio Tags
    // SoundFormat(4bit) | SoundRate(2bit) | SoundSize(1bit) | SoundType(1bit)
    int8_t sound_format = buffer->read_1bytes();
    sound_format = (sound_format >> 4) & 0x0f;

    acodec->id = (SrsAudioCodecId)sound_format;

    // AAC만 파싱한다 — 그 외 코덱은 id만 기록하고 성공 리턴 (호출자가 드롭).
    // 원본은 MP3 분기(ERROR_HLS_TRY_MP3)가 있으나 AAC 전용으로 축소 — CLAUDE.md §5.6 S10.
    if (acodec->id != SrsAudioCodecIdAAC) {
        return err;
    }

    if (buffer->empty()) {
        return err;
    }

    // @doc video_file_format_spec_v10_1.pdf, page 77, E.4.2.1 AACAUDIODATA
    int8_t aac_packet_type = buffer->read_1bytes();
    audio->aac_packet_type = (SrsAudioAacFrameTrait)aac_packet_type;

    if (aac_packet_type == SrsAudioAacFrameTraitSequenceHeader) {
        // AudioSpecificConfig
        if ((err = audio_aac_sequence_header_demux(buffer->head(), buffer->left())) != srs_success) {
            return srs_error_wrap(err, "demux aac sh");
        }
        return err;
    }

    // Raw AAC frame data in UI8[], 시퀀스 헤더 파싱 전이면 디코딩 불가 — 드롭.
    if (!acodec->is_aac_codec_ok()) {
        srs_warn("aac ignore raw for sh not decoded");
        return err;
    }

    if ((err = audio->add_sample(buffer->head(), buffer->left())) != srs_success) {
        return srs_error_wrap(err, "add audio sample");
    }

    return err;
}

srs_error_t SrsFormat::audio_aac_sequence_header_demux(char* data, int size)
{
    srs_error_t err = srs_success;

    if (size < 2) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "audio sequence header requires 2bytes, size=%d", size);
    }

    // @doc ISO_IEC_14496-3-AAC-2001.pdf, page 33, 1.6.2.1 AudioSpecificConfig
    // Byte 0: audioObjectType(5bit) | samplingFrequencyIndex 상위 3bit
    // Byte 1: samplingFrequencyIndex 하위 1bit | channelConfiguration(4bit) | ...
    uint8_t profile_ObjectType = (uint8_t)data[0];
    uint8_t samplingFrequencyIndex = (uint8_t)data[1];

    acodec->aac_channels = (samplingFrequencyIndex >> 3) & 0x0f;
    samplingFrequencyIndex = ((profile_ObjectType << 1) & 0x0e) | ((samplingFrequencyIndex >> 7) & 0x01);
    profile_ObjectType = (profile_ObjectType >> 3) & 0x1f;

    acodec->aac_sample_rate = samplingFrequencyIndex;
    acodec->aac_object = (SrsAacObjectType)profile_ObjectType;

    if (acodec->aac_object == SrsAacObjectTypeForbidden) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "aac object type invalid");
    }
    if (acodec->aac_sample_rate > 12) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "aac sample rate index invalid, index=%d", acodec->aac_sample_rate);
    }

    return err;
}

srs_error_t SrsFormat::on_video(int64_t timestamp, char* data, int size)
{
    srs_error_t err = srs_success;

    if (!data || size <= 0) {
        return err;
    }

    // Lazy create the video frame and codec config.
    if (!vcodec) {
        vcodec = new SrsVideoCodecConfig();
    }
    if (!video) {
        video = new SrsVideoFrame();
        video->vcodec_ = vcodec;
    }

    SrsBuffer buffer(data, size);
    return video_avc_demux(&buffer, timestamp);
}

srs_error_t SrsFormat::video_avc_demux(SrsBuffer* buffer, int64_t timestamp)
{
    srs_error_t err = srs_success;

    // Reset the frame for each message.
    video->nb_samples = 0;
    video->has_idr = false;
    video->dts = timestamp;
    video->cts = 0;

    // @doc video_file_format_spec_v10_1.pdf, page 78, E.4.3 Video Tags
    // FrameType(4bit) | CodecID(4bit)
    int8_t frame_type = buffer->read_1bytes();
    int8_t codec_id = frame_type & 0x0f;
    frame_type = (frame_type >> 4) & 0x0f;

    video->frame_type = (SrsVideoAvcFrameType)frame_type;
    vcodec->id = (SrsVideoCodecId)codec_id;

    // H.264만 파싱한다 — 그 외 코덱은 id만 기록하고 성공 리턴 (호출자가 드롭).
    if (vcodec->id != SrsVideoCodecIdAVC) {
        return err;
    }

    if (!buffer->require(4)) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "avc requires 4bytes header");
    }

    // AVCPacketType(1B) + CompositionTime(3B, SI24)
    int8_t avc_packet_type = buffer->read_1bytes();
    int32_t composition_time = buffer->read_3bytes();
    // 부호 확장 — B-frame이면 음수가 될 수 있다.
    if (composition_time & 0x00800000) {
        composition_time |= (int32_t)0xff000000;
    }

    video->avc_packet_type = (SrsVideoAvcFrameTrait)avc_packet_type;
    video->cts = composition_time;

    if (avc_packet_type == SrsVideoAvcFrameTraitSequenceHeader) {
        if ((err = avc_demux_sps_pps(buffer)) != srs_success) {
            return srs_error_wrap(err, "demux avc sh");
        }
        return err;
    }

    if (avc_packet_type == SrsVideoAvcFrameTraitNALU) {
        // 시퀀스 헤더 파싱 전이면 NALU 길이 프리픽스 크기를 모른다 — 드롭.
        if (!vcodec->is_avc_codec_ok()) {
            srs_warn("avc ignore nalu for sh not decoded");
            return err;
        }

        if ((err = avc_demux_ibmf_format(buffer)) != srs_success) {
            return srs_error_wrap(err, "demux avc ibmf");
        }
        return err;
    }

    // Ignore other avc packet types, for example, sequence header EOF.
    return err;
}

srs_error_t SrsFormat::avc_demux_sps_pps(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // @doc ISO_IEC_14496-15-AVC-format-2012.pdf, page 16, 5.2.4.1 AVCDecoderConfigurationRecord
    if (!stream->require(6)) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "avcC requires 6bytes");
    }

    // configurationVersion(1B) + AVCProfileIndication(1B) + profile_compatibility(1B) + AVCLevelIndication(1B)
    stream->skip(4);

    // lengthSizeMinusOne(하위 2bit): NALU 길이 프리픽스 = 이 값+1 바이트.
    int8_t length_size_minus_one = stream->read_1bytes() & 0x03;
    vcodec->NAL_unit_length = length_size_minus_one;
    if (vcodec->NAL_unit_length == 2) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "sps lengthSizeMinusOne should never be 2");
    }

    // numOfSequenceParameterSets(하위 5bit)
    int8_t numOfSequenceParameterSets = stream->read_1bytes() & 0x1f;
    for (int i = 0; i < numOfSequenceParameterSets; i++) {
        if (!stream->require(2)) {
            return srs_error_new(ERROR_HLS_DECODE_ERROR, "decode sps length");
        }
        uint16_t sequenceParameterSetLength = (uint16_t)stream->read_2bytes();
        if (!stream->require(sequenceParameterSetLength)) {
            return srs_error_new(ERROR_HLS_DECODE_ERROR, "decode sps data");
        }
        // 첫 SPS만 유지 (원본도 first non-empty만 사용).
        if (sequenceParameterSetLength > 0 && vcodec->sequenceParameterSetNALUnit.empty()) {
            vcodec->sequenceParameterSetNALUnit.assign(stream->head(), stream->head() + sequenceParameterSetLength);
        }
        stream->skip(sequenceParameterSetLength);
    }

    if (!stream->require(1)) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "decode pps count");
    }
    int8_t numOfPictureParameterSets = stream->read_1bytes() & 0x1f;
    for (int i = 0; i < numOfPictureParameterSets; i++) {
        if (!stream->require(2)) {
            return srs_error_new(ERROR_HLS_DECODE_ERROR, "decode pps length");
        }
        uint16_t pictureParameterSetLength = (uint16_t)stream->read_2bytes();
        if (!stream->require(pictureParameterSetLength)) {
            return srs_error_new(ERROR_HLS_DECODE_ERROR, "decode pps data");
        }
        if (pictureParameterSetLength > 0 && vcodec->pictureParameterSetNALUnit.empty()) {
            vcodec->pictureParameterSetNALUnit.assign(stream->head(), stream->head() + pictureParameterSetLength);
        }
        stream->skip(pictureParameterSetLength);
    }

    if (!vcodec->is_avc_codec_ok()) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "no sps in avcC");
    }

    return err;
}

srs_error_t SrsFormat::avc_demux_ibmf_format(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // @doc ISO_IEC_14496-15-AVC-format-2012.pdf, page 20, 5.3.4.2 Sample format
    // 페이로드 = [길이 프리픽스(NAL_unit_length+1 바이트) + NALU] 반복.
    while (!stream->empty()) {
        if (!stream->require(vcodec->NAL_unit_length + 1)) {
            return srs_error_new(ERROR_HLS_AVC_SAMPLE_SIZE, "decode nalu size");
        }

        int32_t NALUnitLength = 0;
        if (vcodec->NAL_unit_length == 3) {
            NALUnitLength = stream->read_4bytes();
        } else if (vcodec->NAL_unit_length == 1) {
            NALUnitLength = (uint16_t)stream->read_2bytes();
        } else {
            NALUnitLength = (uint8_t)stream->read_1bytes();
        }

        if (NALUnitLength < 0 || !stream->require(NALUnitLength)) {
            return srs_error_new(ERROR_HLS_AVC_SAMPLE_SIZE, "invalid nalu size=%d", NALUnitLength);
        }
        if (NALUnitLength == 0) {
            continue;
        }

        // IDR(nal_unit_type=5)이면 TS 먹싱 때 SPS/PPS를 앞에 재삽입한다 (srs_kernel_ts).
        SrsAvcNaluType nalu_type = (SrsAvcNaluType)(stream->head()[0] & 0x1f);
        if (nalu_type == SrsAvcNaluTypeIDR) {
            video->has_idr = true;
        }

        if ((err = video->add_sample(stream->head(), NALUnitLength)) != srs_success) {
            return srs_error_wrap(err, "add video sample");
        }
        stream->skip(NALUnitLength);
    }

    return err;
}
