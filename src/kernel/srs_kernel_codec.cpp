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
    // Sequence header is only for H.264.
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
    // Sequence header is only for AAC.
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
    width = 0;
    height = 0;
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
    raw = NULL;
    nb_raw = 0;
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

    // The raw AAC payload after the 2-byte tag header (원본: srs_kernel_codec.cpp:2714).
    raw = buffer->head();
    nb_raw = buffer->left();

    if (aac_packet_type == SrsAudioAacFrameTraitSequenceHeader) {
        // AudioSpecificConfig — fMP4 esds용으로 원문도 보관 (원본 :2723).
        acodec->aac_extra_data.assign(buffer->head(), buffer->head() + buffer->left());
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

    // The raw AVCC payload(길이 프리픽스 포함) after the 5-byte tag header (원본 :1017).
    raw = buffer->head();
    nb_raw = buffer->left();

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

    // avcC 원문 보관 — fMP4 init.mp4의 avcC 박스가 그대로 싣는다 (원본 :1105).
    vcodec->avc_extra_data.assign(stream->head(), stream->head() + stream->left());

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

    // SPS에서 해상도를 파싱해 vcodec->width/height를 채운다 (S16).
    // 원본은 파싱 오류를 전파하지만, 여기서는 best-effort — 실패해도 시퀀스 헤더 자체는
    // 유효하므로 경고만 남기고 0을 유지한다 (TS-HLS 경로는 해상도가 필요 없다 — §5.6 S16).
    if ((err = avc_demux_sps()) != srs_success) {
        srs_warn("ignore sps parse error: %s", srs_error_desc(err).c_str());
        srs_freep(err);
    }

    return err;
}

// Remove the emulation bytes from the stream, and return the number of bytes of the rbsp.
// (원본 srs_kernel_codec.cpp:885 — 00 00 03 xx의 03 제거)
static int srs_rbsp_remove_emulation_bytes(SrsBuffer* stream, std::vector<uint8_t>& rbsp)
{
    int nb_rbsp = 0;
    while (!stream->empty()) {
        rbsp[nb_rbsp] = stream->read_1bytes();

        // .. 00 00 03 xx, the 03 byte should be dropped where xx represents any
        // 2 bit pattern: 00, 01, 10, or 11.
        if (nb_rbsp >= 2 && rbsp[nb_rbsp - 2] == 0 && rbsp[nb_rbsp - 1] == 0 && rbsp[nb_rbsp] == 3) {
            // read 1byte more.
            if (stream->empty()) {
                nb_rbsp++;
                break;
            }

            uint8_t ev = stream->read_1bytes();
            if (ev > 3) {
                nb_rbsp++;
            }
            rbsp[nb_rbsp] = ev;
        }

        nb_rbsp++;
    }

    return nb_rbsp;
}

// ue(v) in 9.1 Parsing process for Exp-Golomb codes. (원본 srs_kernel_utility.cpp:38 —
// utility 파일이 없으므로 파일-로컬로 병합, S3/S5/S6과 같은 이유)
static srs_error_t srs_avc_nalu_read_uev(SrsBitBuffer* stream, int32_t& v)
{
    srs_error_t err = srs_success;

    if (stream->empty()) {
        return srs_error_new(ERROR_AVC_NALU_UEV, "empty stream");
    }

    // Syntax elements coded as ue(v), me(v), or se(v) are Exp-Golomb-coded.
    //      leadingZeroBits = -1;
    //      for( b = 0; !b; leadingZeroBits++ )
    //          b = read_bits( 1 )
    // The variable codeNum is then assigned as follows:
    //      codeNum = (2<<leadingZeroBits) - 1 + read_bits( leadingZeroBits )
    int leadingZeroBits = -1;
    for (int8_t b = 0; !b && !stream->empty(); leadingZeroBits++) {
        b = stream->read_bit();
    }

    if (leadingZeroBits >= 31) {
        return srs_error_new(ERROR_AVC_NALU_UEV, "%dbits overflow 31bits", leadingZeroBits);
    }

    v = (1 << leadingZeroBits) - 1;
    for (int i = 0; i < (int)leadingZeroBits; i++) {
        if (stream->empty()) {
            return srs_error_new(ERROR_AVC_NALU_UEV, "no bytes for leadingZeroBits=%d", leadingZeroBits);
        }

        int32_t b = stream->read_bit();
        v += b << (leadingZeroBits - 1 - i);
    }

    return err;
}

// (원본 srs_kernel_utility.cpp:76)
static srs_error_t srs_avc_nalu_read_bit(SrsBitBuffer* stream, int8_t& v)
{
    srs_error_t err = srs_success;

    if (stream->empty()) {
        return srs_error_new(ERROR_AVC_NALU_UEV, "empty stream");
    }

    v = stream->read_bit();

    return err;
}

srs_error_t SrsFormat::avc_demux_sps()
{
    srs_error_t err = srs_success;

    if (vcodec->sequenceParameterSetNALUnit.empty()) {
        return err;
    }

    char* sps = &vcodec->sequenceParameterSetNALUnit[0];
    int nbsps = (int)vcodec->sequenceParameterSetNALUnit.size();

    SrsBuffer stream(sps, nbsps);

    // for NALU, 7.3.1 NAL unit syntax
    // ISO_IEC_14496-10-AVC-2012.pdf, page 61.
    if (!stream.require(1)) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "decode SPS");
    }
    int8_t nutv = stream.read_1bytes();

    // forbidden_zero_bit shall be equal to 0.
    int8_t forbidden_zero_bit = (nutv >> 7) & 0x01;
    if (forbidden_zero_bit) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "forbidden_zero_bit shall be equal to 0");
    }

    // nal_ref_idc not equal to 0 specifies that the content of the NAL unit contains a sequence
    // parameter set or a picture parameter set or a slice of a reference picture.
    int8_t nal_ref_idc = (nutv >> 5) & 0x03;
    if (!nal_ref_idc) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "for sps, nal_ref_idc shall be not be equal to 0");
    }

    // nal_unit_type specifies the type of RBSP data structure contained in the NAL unit.
    SrsAvcNaluType nal_unit_type = (SrsAvcNaluType)(nutv & 0x1f);
    if (nal_unit_type != SrsAvcNaluTypeSPS) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "for sps, nal_unit_type shall be equal to 7");
    }

    // decode the rbsp from the sps.
    // rbsp[ i ] a raw byte sequence payload is specified as an ordered sequence of bytes.
    std::vector<uint8_t> rbsp(vcodec->sequenceParameterSetNALUnit.size());
    int nb_rbsp = srs_rbsp_remove_emulation_bytes(&stream, rbsp);

    return avc_demux_sps_rbsp((char*)&rbsp[0], nb_rbsp);
}

srs_error_t SrsFormat::avc_demux_sps_rbsp(char* rbsp, int nb_rbsp)
{
    srs_error_t err = srs_success;

    // reparse the rbsp.
    SrsBuffer stream(rbsp, nb_rbsp);

    // for SPS, 7.3.2.1.1 Sequence parameter set data syntax
    // ISO_IEC_14496-10-AVC-2012.pdf, page 62.
    if (!stream.require(3)) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "sps shall atleast 3bytes");
    }
    uint8_t profile_idc = stream.read_1bytes();
    if (!profile_idc) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "sps the profile_idc invalid");
    }

    int8_t flags = stream.read_1bytes();
    if (flags & 0x03) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "sps the flags invalid");
    }

    uint8_t level_idc = stream.read_1bytes();
    if (!level_idc) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "sps the level_idc invalid");
    }

    SrsBitBuffer bs(&stream);

    int32_t seq_parameter_set_id = -1;
    if ((err = srs_avc_nalu_read_uev(&bs, seq_parameter_set_id)) != srs_success) {
        return srs_error_wrap(err, "read seq_parameter_set_id");
    }
    if (seq_parameter_set_id < 0) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "sps the seq_parameter_set_id invalid");
    }

    int32_t chroma_format_idc = -1;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244
        || profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118
        || profile_idc == 128) {
        if ((err = srs_avc_nalu_read_uev(&bs, chroma_format_idc)) != srs_success) {
            return srs_error_wrap(err, "read chroma_format_idc");
        }
        if (chroma_format_idc == 3) {
            int8_t separate_colour_plane_flag = -1;
            if ((err = srs_avc_nalu_read_bit(&bs, separate_colour_plane_flag)) != srs_success) {
                return srs_error_wrap(err, "read separate_colour_plane_flag");
            }
        }

        int32_t bit_depth_luma_minus8 = -1;
        if ((err = srs_avc_nalu_read_uev(&bs, bit_depth_luma_minus8)) != srs_success) {
            return srs_error_wrap(err, "read bit_depth_luma_minus8");
        }

        int32_t bit_depth_chroma_minus8 = -1;
        if ((err = srs_avc_nalu_read_uev(&bs, bit_depth_chroma_minus8)) != srs_success) {
            return srs_error_wrap(err, "read bit_depth_chroma_minus8");
        }

        int8_t qpprime_y_zero_transform_bypass_flag = -1;
        if ((err = srs_avc_nalu_read_bit(&bs, qpprime_y_zero_transform_bypass_flag)) != srs_success) {
            return srs_error_wrap(err, "read qpprime_y_zero_transform_bypass_flag");
        }

        int8_t seq_scaling_matrix_present_flag = -1;
        if ((err = srs_avc_nalu_read_bit(&bs, seq_scaling_matrix_present_flag)) != srs_success) {
            return srs_error_wrap(err, "read seq_scaling_matrix_present_flag");
        }
        if (seq_scaling_matrix_present_flag) {
            int nb_scmpfs = ((chroma_format_idc != 3)? 8:12);
            for (int i = 0; i < nb_scmpfs; i++) {
                int8_t seq_scaling_matrix_present_flag_i = -1;
                if ((err = srs_avc_nalu_read_bit(&bs, seq_scaling_matrix_present_flag_i)) != srs_success) {
                    return srs_error_wrap(err, "read seq_scaling_matrix_present_flag_i");
                }
                // 원본도 scaling list 본문은 파싱하지 않는다 — flag가 서 있으면
                // 이후 비트 오프셋이 틀어지므로 해상도 파싱을 포기한다.
                if (seq_scaling_matrix_present_flag_i) {
                    return srs_error_new(ERROR_HLS_DECODE_ERROR, "sps scaling list not supported");
                }
            }
        }
    }

    int32_t log2_max_frame_num_minus4 = -1;
    if ((err = srs_avc_nalu_read_uev(&bs, log2_max_frame_num_minus4)) != srs_success) {
        return srs_error_wrap(err, "read log2_max_frame_num_minus4");
    }

    int32_t pic_order_cnt_type = -1;
    if ((err = srs_avc_nalu_read_uev(&bs, pic_order_cnt_type)) != srs_success) {
        return srs_error_wrap(err, "read pic_order_cnt_type");
    }

    if (pic_order_cnt_type == 0) {
        int32_t log2_max_pic_order_cnt_lsb_minus4 = -1;
        if ((err = srs_avc_nalu_read_uev(&bs, log2_max_pic_order_cnt_lsb_minus4)) != srs_success) {
            return srs_error_wrap(err, "read log2_max_pic_order_cnt_lsb_minus4");
        }
    } else if (pic_order_cnt_type == 1) {
        int8_t delta_pic_order_always_zero_flag = -1;
        if ((err = srs_avc_nalu_read_bit(&bs, delta_pic_order_always_zero_flag)) != srs_success) {
            return srs_error_wrap(err, "read delta_pic_order_always_zero_flag");
        }

        int32_t offset_for_non_ref_pic = -1;
        if ((err = srs_avc_nalu_read_uev(&bs, offset_for_non_ref_pic)) != srs_success) {
            return srs_error_wrap(err, "read offset_for_non_ref_pic");
        }

        int32_t offset_for_top_to_bottom_field = -1;
        if ((err = srs_avc_nalu_read_uev(&bs, offset_for_top_to_bottom_field)) != srs_success) {
            return srs_error_wrap(err, "read offset_for_top_to_bottom_field");
        }

        int32_t num_ref_frames_in_pic_order_cnt_cycle = -1;
        if ((err = srs_avc_nalu_read_uev(&bs, num_ref_frames_in_pic_order_cnt_cycle)) != srs_success) {
            return srs_error_wrap(err, "read num_ref_frames_in_pic_order_cnt_cycle");
        }
        if (num_ref_frames_in_pic_order_cnt_cycle < 0) {
            return srs_error_new(ERROR_HLS_DECODE_ERROR, "sps the num_ref_frames_in_pic_order_cnt_cycle");
        }
        for (int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++) {
            int32_t offset_for_ref_frame_i = -1;
            if ((err = srs_avc_nalu_read_uev(&bs, offset_for_ref_frame_i)) != srs_success) {
                return srs_error_wrap(err, "read offset_for_ref_frame_i");
            }
        }
    }

    int32_t max_num_ref_frames = -1;
    if ((err = srs_avc_nalu_read_uev(&bs, max_num_ref_frames)) != srs_success) {
        return srs_error_wrap(err, "read max_num_ref_frames");
    }

    int8_t gaps_in_frame_num_value_allowed_flag = -1;
    if ((err = srs_avc_nalu_read_bit(&bs, gaps_in_frame_num_value_allowed_flag)) != srs_success) {
        return srs_error_wrap(err, "read gaps_in_frame_num_value_allowed_flag");
    }

    int32_t pic_width_in_mbs_minus1 = -1;
    if ((err = srs_avc_nalu_read_uev(&bs, pic_width_in_mbs_minus1)) != srs_success) {
        return srs_error_wrap(err, "read pic_width_in_mbs_minus1");
    }

    int32_t pic_height_in_map_units_minus1 = -1;
    if ((err = srs_avc_nalu_read_uev(&bs, pic_height_in_map_units_minus1)) != srs_success) {
        return srs_error_wrap(err, "read pic_height_in_map_units_minus1");
    }

    int8_t frame_mbs_only_flag = -1;
    if ((err = srs_avc_nalu_read_bit(&bs, frame_mbs_only_flag)) != srs_success) {
        return srs_error_wrap(err, "read frame_mbs_only_flag");
    }
    if (!frame_mbs_only_flag) {
        /* Skip mb_adaptive_frame_field_flag */
        int8_t mb_adaptive_frame_field_flag = -1;
        if ((err = srs_avc_nalu_read_bit(&bs, mb_adaptive_frame_field_flag)) != srs_success) {
            return srs_error_wrap(err, "read mb_adaptive_frame_field_flag");
        }
    }

    /* Skip direct_8x8_inference_flag */
    int8_t direct_8x8_inference_flag = -1;
    if ((err = srs_avc_nalu_read_bit(&bs, direct_8x8_inference_flag)) != srs_success) {
        return srs_error_wrap(err, "read direct_8x8_inference_flag");
    }

    /* We need the following value to evaluate offsets, if any */
    int8_t frame_cropping_flag = -1;
    if ((err = srs_avc_nalu_read_bit(&bs, frame_cropping_flag)) != srs_success) {
        return srs_error_wrap(err, "read frame_cropping_flag");
    }
    int32_t frame_crop_left_offset = 0, frame_crop_right_offset = 0,
            frame_crop_top_offset = 0, frame_crop_bottom_offset = 0;
    if (frame_cropping_flag) {
        if ((err = srs_avc_nalu_read_uev(&bs, frame_crop_left_offset)) != srs_success) {
            return srs_error_wrap(err, "read frame_crop_left_offset");
        }
        if ((err = srs_avc_nalu_read_uev(&bs, frame_crop_right_offset)) != srs_success) {
            return srs_error_wrap(err, "read frame_crop_right_offset");
        }
        if ((err = srs_avc_nalu_read_uev(&bs, frame_crop_top_offset)) != srs_success) {
            return srs_error_wrap(err, "read frame_crop_top_offset");
        }
        if ((err = srs_avc_nalu_read_uev(&bs, frame_crop_bottom_offset)) != srs_success) {
            return srs_error_wrap(err, "read frame_crop_bottom_offset");
        }
    }

    vcodec->width = ((pic_width_in_mbs_minus1 + 1) * 16) - frame_crop_left_offset * 2 - frame_crop_right_offset * 2;
    vcodec->height = ((2 - frame_mbs_only_flag) * (pic_height_in_map_units_minus1 + 1) * 16)
                    - (frame_crop_top_offset * 2) - (frame_crop_bottom_offset * 2);

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
