// srs_simple — 원본: trunk/src/kernel/srs_kernel_codec.cpp (SrsFlvVideo/SrsFlvAudio 판별자만)
#include <srs_kernel_codec.hpp>

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
