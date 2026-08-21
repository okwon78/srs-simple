// srs_simple — 원본: trunk/src/kernel/srs_kernel_ts.cpp
// 원본의 SrsTsPacket/AdaptationField/PSI 클래스 트리 대신 encode_pat_pmt/encode_pes가
// 188바이트 버퍼를 직접 조립한다 — CLAUDE.md §5.6 S10. 바이트 레이아웃은 원본과 동일.
#include <srs_kernel_ts.hpp>

#include <string.h>

#include <vector>

#include <srs_kernel_error.hpp>
#include <srs_kernel_file.hpp>
#include <srs_kernel_log.hpp>
#include <srs_kernel_stream.hpp>

using namespace std;

// The MPEG-2 CRC32 (poly 0x04C11DB7, init 0xFFFFFFFF, 비트반사/최종 XOR 없음).
// PSI(PAT/PMT) 섹션의 table_id부터 CRC 직전까지를 계산한다.
uint32_t srs_crc32_mpegts(const void* buf, int size)
{
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t crc = 0xffffffff;

    for (int i = 0; i < size; i++) {
        crc ^= ((uint32_t)p[i]) << 24;
        for (int j = 0; j < 8; j++) {
            crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
        }
    }

    return crc;
}

SrsTsMessage::SrsTsMessage()
{
    dts = pts = 0;
    sid = (SrsTsPESStreamId)0x00;
    write_pcr = false;
    payload = new SrsSimpleStream();
}

SrsTsMessage::~SrsTsMessage()
{
    srs_freep(payload);
}

bool SrsTsMessage::is_audio()
{
    // 110x xxxx (Table 2-18)
    return ((sid >> 5) & 0x07) == 0x06;
}

bool SrsTsMessage::is_video()
{
    // 1110 xxxx (Table 2-18)
    return ((sid >> 4) & 0x0f) == 0x0e;
}

SrsTsChannel::SrsTsChannel()
{
    pid = 0;
    stream = SrsTsStreamReserved;
    continuity_counter = 0;
}

SrsTsChannel::~SrsTsChannel()
{
}

SrsTsContext::SrsTsContext()
{
    vcodec = SrsVideoCodecIdForbidden;
    acodec = SrsAudioCodecIdForbidden;
}

SrsTsContext::~SrsTsContext()
{
    std::map<int, SrsTsChannel*>::iterator it;
    for (it = pids.begin(); it != pids.end(); ++it) {
        SrsTsChannel* channel = it->second;
        srs_freep(channel);
    }
    pids.clear();
}

void SrsTsContext::reset()
{
    // 코덱을 잊으면 다음 encode가 코덱 변경으로 판단해 PAT/PMT부터 다시 쓴다.
    // continuity counter는 세그먼트를 넘어 이어간다 (플레이어가 연속 재생하므로).
    vcodec = SrsVideoCodecIdForbidden;
    acodec = SrsAudioCodecIdForbidden;
}

SrsTsChannel* SrsTsContext::channel(int pid, SrsTsStream stream)
{
    std::map<int, SrsTsChannel*>::iterator it = pids.find(pid);
    if (it != pids.end()) {
        return it->second;
    }

    SrsTsChannel* c = new SrsTsChannel();
    c->pid = pid;
    c->stream = stream;
    pids[pid] = c;
    return c;
}

srs_error_t SrsTsContext::encode(ISrsWriter* writer, SrsTsMessage* msg, SrsVideoCodecId vc, SrsAudioCodecId ac)
{
    srs_error_t err = srs_success;

    // 세그먼트 시작(reset 직후) 또는 코덱 변경이면 PAT/PMT를 먼저 쓴다.
    // HLS 세그먼트는 독립 디코딩 가능해야 하므로 매 세그먼트가 PAT/PMT로 시작한다.
    if (vcodec != vc || acodec != ac) {
        vcodec = vc;
        acodec = ac;
        if ((err = encode_pat_pmt(writer, TS_VIDEO_AVC_PID, SrsTsStreamVideoH264, TS_AUDIO_AAC_PID, SrsTsStreamAudioAAC)) != srs_success) {
            return srs_error_wrap(err, "encode pat pmt");
        }
    }

    // pure audio 스트림은 PCR을 실을 비디오가 없으므로 audio 메시지가 PCR을 싣는다.
    if (vcodec != SrsVideoCodecIdAVC && msg->is_audio()) {
        msg->write_pcr = true;
    }

    int16_t pid = msg->is_audio() ? TS_AUDIO_AAC_PID : TS_VIDEO_AVC_PID;
    if ((err = encode_pes(writer, msg, pid)) != srs_success) {
        return srs_error_wrap(err, "encode pes");
    }

    return err;
}

srs_error_t SrsTsContext::encode_pat_pmt(ISrsWriter* writer, int16_t vpid, SrsTsStream vs, int16_t apid, SrsTsStream as)
{
    srs_error_t err = srs_success;

    bool has_video = (vcodec == SrsVideoCodecIdAVC);
    bool has_audio = (acodec == SrsAudioCodecIdAAC);
    if (!has_video && !has_audio) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "no a/v codec for pat/pmt");
    }

    // ---- PAT (pid=0) ----
    // [TS header(4)] [pointer(1)] [섹션: table_id(1) + len(2) + tsid(2) + ver(1)
    //  + sec(1) + last(1) + program(4) + CRC(4)] [0xFF 스터핑]
    uint8_t pat[SRS_TS_PACKET_SIZE];
    memset(pat, 0xff, sizeof(pat));

    SrsTsChannel* pat_channel = channel(0x00, SrsTsStreamReserved);
    int p = 0;
    pat[p++] = 0x47;                                                // sync_byte
    pat[p++] = 0x40;                                                // PUSI=1, pid=0 상위
    pat[p++] = 0x00;                                                // pid=0 하위
    pat[p++] = 0x10 | (pat_channel->continuity_counter++ & 0x0f);   // payload only + cc
    pat[p++] = 0x00;                                                // pointer_field
    int section_start = p;
    pat[p++] = 0x00;                                                // table_id: PAT
    // section_length: 이 필드 다음부터 CRC 끝까지 = 5 + 4(program) + 4(CRC) = 13
    pat[p++] = 0xb0;                                                // syntax(1) + '011' + len 상위
    pat[p++] = 13;
    pat[p++] = 0x00;                                                // transport_stream_id = 1
    pat[p++] = 0x01;
    pat[p++] = 0xc1;                                                // '11' + version(0) + current(1)
    pat[p++] = 0x00;                                                // section_number
    pat[p++] = 0x00;                                                // last_section_number
    pat[p++] = (TS_PMT_NUMBER >> 8) & 0xff;                         // program_number
    pat[p++] = TS_PMT_NUMBER & 0xff;
    pat[p++] = 0xe0 | ((TS_PMT_PID >> 8) & 0x1f);                   // '111' + PMT pid
    pat[p++] = TS_PMT_PID & 0xff;

    uint32_t crc = srs_crc32_mpegts(pat + section_start, p - section_start);
    pat[p++] = (crc >> 24) & 0xff;
    pat[p++] = (crc >> 16) & 0xff;
    pat[p++] = (crc >> 8) & 0xff;
    pat[p++] = crc & 0xff;

    if ((err = writer->write(pat, SRS_TS_PACKET_SIZE, NULL)) != srs_success) {
        return srs_error_wrap(err, "write pat");
    }

    // ---- PMT (pid=TS_PMT_PID) ----
    uint8_t pmt[SRS_TS_PACKET_SIZE];
    memset(pmt, 0xff, sizeof(pmt));

    SrsTsChannel* pmt_channel = channel(TS_PMT_PID, SrsTsStreamReserved);
    p = 0;
    pmt[p++] = 0x47;
    pmt[p++] = 0x40 | ((TS_PMT_PID >> 8) & 0x1f);
    pmt[p++] = TS_PMT_PID & 0xff;
    pmt[p++] = 0x10 | (pmt_channel->continuity_counter++ & 0x0f);
    pmt[p++] = 0x00;                                                // pointer_field
    section_start = p;
    pmt[p++] = 0x02;                                                // table_id: PMT
    // section_length: 9(고정) + 5*ES개수 + 4(CRC)
    int nb_es = (has_video ? 1 : 0) + (has_audio ? 1 : 0);
    int section_length = 9 + 5 * nb_es + 4;
    pmt[p++] = 0xb0 | ((section_length >> 8) & 0x0f);
    pmt[p++] = section_length & 0xff;
    pmt[p++] = (TS_PMT_NUMBER >> 8) & 0xff;                         // program_number
    pmt[p++] = TS_PMT_NUMBER & 0xff;
    pmt[p++] = 0xc1;                                                // '11' + version(0) + current(1)
    pmt[p++] = 0x00;                                                // section_number
    pmt[p++] = 0x00;                                                // last_section_number
    // PCR_PID: 비디오가 있으면 비디오, 없으면(pure audio) 오디오.
    int16_t pcr_pid = has_video ? vpid : apid;
    pmt[p++] = 0xe0 | ((pcr_pid >> 8) & 0x1f);
    pmt[p++] = pcr_pid & 0xff;
    pmt[p++] = 0xf0;                                                // program_info_length = 0
    pmt[p++] = 0x00;
    // ES infos: stream_type(1) + '111'+elementary_PID(2) + ES_info_length(2)
    if (has_video) {
        pmt[p++] = vs;                                              // 0x1b: H.264
        pmt[p++] = 0xe0 | ((vpid >> 8) & 0x1f);
        pmt[p++] = vpid & 0xff;
        pmt[p++] = 0xf0;
        pmt[p++] = 0x00;
    }
    if (has_audio) {
        pmt[p++] = as;                                              // 0x0f: ADTS AAC
        pmt[p++] = 0xe0 | ((apid >> 8) & 0x1f);
        pmt[p++] = apid & 0xff;
        pmt[p++] = 0xf0;
        pmt[p++] = 0x00;
    }

    crc = srs_crc32_mpegts(pmt + section_start, p - section_start);
    pmt[p++] = (crc >> 24) & 0xff;
    pmt[p++] = (crc >> 16) & 0xff;
    pmt[p++] = (crc >> 8) & 0xff;
    pmt[p++] = crc & 0xff;

    if ((err = writer->write(pmt, SRS_TS_PACKET_SIZE, NULL)) != srs_success) {
        return srs_error_wrap(err, "write pmt");
    }

    return err;
}

// Encode a 33bits PTS/DTS to 5 bytes with markers.
// @param fb the 4bits marker: '0010'(PTS only의 PTS), '0011'(PTS+DTS의 PTS), '0001'(DTS)
// @doc ISO_IEC_13818-1-2000.pdf, page 56, 2.4.3.7 PES packet
static void srs_ts_encode_33bits_dts_pts(uint8_t* p, uint8_t fb, int64_t v)
{
    // PTS/DTS는 33비트로 순환한다 (약 26.5시간).
    v &= 0x1ffffffffLL;

    // fb(4) | v[32:30](3) | marker(1)
    p[0] = ((fb << 4) & 0xf0) | (((v >> 30) & 0x07) << 1) | 0x01;
    // v[29:22](8)
    p[1] = (v >> 22) & 0xff;
    // v[21:15](7) | marker(1)
    p[2] = ((v >> 14) & 0xfe) | 0x01;
    // v[14:7](8)
    p[3] = (v >> 7) & 0xff;
    // v[6:0](7) | marker(1)
    p[4] = ((v << 1) & 0xfe) | 0x01;
}

srs_error_t SrsTsContext::encode_pes(ISrsWriter* writer, SrsTsMessage* msg, int16_t pid)
{
    srs_error_t err = srs_success;

    if (msg->payload->length() <= 0) {
        return err;
    }

    SrsTsChannel* ch = channel(pid, msg->is_audio() ? SrsTsStreamAudioAAC : SrsTsStreamVideoH264);

    // ---- PES 헤더 조립 ----
    // [start code(3)=000001] [stream_id(1)] [PES_packet_length(2)]
    // [flags(2)] [PES_header_data_length(1)] [PTS(5)] [DTS(5, pts!=dts일 때만)]
    uint8_t pes_header[19];
    int ph = 0;
    pes_header[ph++] = 0x00;
    pes_header[ph++] = 0x00;
    pes_header[ph++] = 0x01;
    pes_header[ph++] = (uint8_t)msg->sid;

    bool write_dts = (msg->dts != msg->pts);
    int header_data_length = write_dts ? 10 : 5;

    // PES_packet_length: 이 필드 다음부터 페이로드 끝까지. 16비트를 넘으면 0(unbounded, 비디오만 허용).
    int pes_packet_length = 3 + header_data_length + msg->payload->length();
    if (pes_packet_length > 0xffff) {
        pes_packet_length = 0;
    }
    pes_header[ph++] = (pes_packet_length >> 8) & 0xff;
    pes_header[ph++] = pes_packet_length & 0xff;

    pes_header[ph++] = 0x80;                                        // '10' + 플래그들 0
    pes_header[ph++] = write_dts ? 0xc0 : 0x80;                     // PTS_DTS_flags
    pes_header[ph++] = header_data_length;

    srs_ts_encode_33bits_dts_pts(pes_header + ph, write_dts ? 0x03 : 0x02, msg->pts);
    ph += 5;
    if (write_dts) {
        srs_ts_encode_33bits_dts_pts(pes_header + ph, 0x01, msg->dts);
        ph += 5;
    }

    // PES 전체 = 헤더 + 페이로드. 188바이트 TS 패킷들로 분할한다.
    vector<char> pes(ph + msg->payload->length());
    memcpy(pes.data(), pes_header, ph);
    memcpy(pes.data() + ph, msg->payload->bytes(), msg->payload->length());

    int total = (int)pes.size();
    int pos = 0;
    bool first = true;
    while (pos < total) {
        uint8_t pkt[SRS_TS_PACKET_SIZE];
        int left = total - pos;

        pkt[0] = 0x47;                                              // sync_byte
        pkt[1] = (first ? 0x40 : 0x00) | ((pid >> 8) & 0x1f);       // PUSI + pid
        pkt[2] = pid & 0xff;

        // adaptation field가 필요한 두 경우:
        //   1) 첫 패킷에 PCR을 실을 때 — len(1)+flags(1)+PCR(6) = 8바이트
        //   2) 남은 페이로드가 184바이트 미만 — 0xFF 스터핑으로 패킷을 채운다
        bool write_pcr = first && msg->write_pcr;
        int af_size = write_pcr ? 8 : 0;
        if (184 - af_size > left) {
            af_size = 184 - left;
        }

        pkt[3] = (af_size > 0 ? 0x30 : 0x10) | (ch->continuity_counter++ & 0x0f);

        int p = 4;
        if (af_size == 1) {
            // 딱 1바이트 부족: adaptation_field_length=0 (flags 없이 길이 바이트만).
            pkt[p++] = 0x00;
        } else if (af_size > 1) {
            pkt[p++] = af_size - 1;                                 // adaptation_field_length
            uint8_t flags = 0x00;
            if (write_pcr) {
                // PCR_flag + random_access_indicator(키프레임 첫 패킷 — 비디오만)
                flags |= 0x10;
                if (msg->is_video()) {
                    flags |= 0x40;
                }
            }
            pkt[p++] = flags;
            if (write_pcr) {
                // PCR: base(33bit) + reserved(6bit, all 1) + extension(9bit, 0)
                int64_t pcr = msg->dts & 0x1ffffffffLL;
                pkt[p++] = (pcr >> 25) & 0xff;
                pkt[p++] = (pcr >> 17) & 0xff;
                pkt[p++] = (pcr >> 9) & 0xff;
                pkt[p++] = (pcr >> 1) & 0xff;
                pkt[p++] = (uint8_t)(((pcr & 0x01) << 7) | 0x7e);
                pkt[p++] = 0x00;
            }
            // 나머지는 0xFF 스터핑.
            while (p < 4 + af_size) {
                pkt[p++] = 0xff;
            }
        }

        int body = SRS_TS_PACKET_SIZE - p;
        memcpy(pkt + p, pes.data() + pos, body);
        pos += body;
        first = false;

        if ((err = writer->write(pkt, SRS_TS_PACKET_SIZE, NULL)) != srs_success) {
            return srs_error_wrap(err, "write ts packet");
        }
    }

    return err;
}

SrsTsContextWriter::SrsTsContextWriter(SrsFileWriter* w, SrsTsContext* c, SrsAudioCodecId ac, SrsVideoCodecId vc)
{
    writer = w;
    context = c;
    acodec_ = ac;
    vcodec_ = vc;
}

SrsTsContextWriter::~SrsTsContextWriter()
{
}

srs_error_t SrsTsContextWriter::write_audio(SrsTsMessage* audio)
{
    srs_error_t err = srs_success;

    if ((err = context->encode(writer, audio, vcodec_, acodec_)) != srs_success) {
        return srs_error_wrap(err, "ts: write audio");
    }

    return err;
}

srs_error_t SrsTsContextWriter::write_video(SrsTsMessage* video)
{
    srs_error_t err = srs_success;

    if ((err = context->encode(writer, video, vcodec_, acodec_)) != srs_success) {
        return srs_error_wrap(err, "ts: write video");
    }

    return err;
}

SrsVideoCodecId SrsTsContextWriter::vcodec()
{
    return vcodec_;
}

void SrsTsContextWriter::set_vcodec(SrsVideoCodecId v)
{
    vcodec_ = v;
}

SrsAudioCodecId SrsTsContextWriter::acodec()
{
    return acodec_;
}

void SrsTsContextWriter::set_acodec(SrsAudioCodecId v)
{
    acodec_ = v;
}

SrsTsMessageCache::SrsTsMessageCache()
{
    audio = NULL;
    video = NULL;
}

SrsTsMessageCache::~SrsTsMessageCache()
{
    srs_freep(audio);
    srs_freep(video);
}

srs_error_t SrsTsMessageCache::cache_audio(SrsAudioFrame* frame, int64_t dts)
{
    srs_error_t err = srs_success;

    // Create the ts audio message.
    if (!audio) {
        audio = new SrsTsMessage();
        audio->write_pcr = false;
        audio->dts = audio->pts = dts;
        audio->sid = SrsTsPESStreamIdAudioCommon;
    }

    if ((err = do_cache_aac(frame)) != srs_success) {
        return srs_error_wrap(err, "cache aac");
    }

    return err;
}

srs_error_t SrsTsMessageCache::cache_video(SrsVideoFrame* frame, int64_t dts)
{
    srs_error_t err = srs_success;

    // Create the ts video message.
    if (!video) {
        video = new SrsTsMessage();
        // IDR 키프레임의 첫 TS 패킷이 PCR을 싣는다.
        video->write_pcr = (frame->frame_type == SrsVideoAvcFrameTypeKeyFrame);
        video->dts = dts;
        video->pts = dts + (int64_t)frame->cts * 90;
        video->sid = SrsTsPESStreamIdVideoCommon;
    }

    if ((err = do_cache_avc(frame)) != srs_success) {
        return srs_error_wrap(err, "cache avc");
    }

    return err;
}

srs_error_t SrsTsMessageCache::do_cache_aac(SrsAudioFrame* frame)
{
    srs_error_t err = srs_success;

    SrsAudioCodecConfig* codec = frame->acodec();
    if (!codec || !codec->is_aac_codec_ok()) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "aac sequence header not decoded");
    }

    for (int i = 0; i < frame->nb_samples; i++) {
        SrsSample* sample = &frame->samples[i];
        int32_t size = sample->size;

        if (!sample->bytes || size <= 0) {
            return srs_error_new(ERROR_HLS_DECODE_ERROR, "invalid aac sample");
        }

        // AAC raw 프레임마다 7바이트 ADTS 헤더를 붙인다.
        // @doc ISO_IEC_13818-7-AAC-2004.pdf, page 26, 6.2 Audio Data Transport Stream
        // frame_length = ADTS 헤더 포함 전체 길이.
        int32_t frame_length = size + 7;

        // syncword(0xfff) + ID(1, MPEG-2) + layer(00) + protection_absent(1)
        uint8_t adts_header[7] = {0xff, 0xf9, 0x00, 0x00, 0x00, 0x0f, 0xfc};
        // profile(2) — ASC의 object type을 ADTS profile로 변환.
        adts_header[2] = (srs_aac_rtmp2ts(codec->aac_object) << 6) & 0xc0;
        // sampling_frequency_index(4)
        adts_header[2] |= (codec->aac_sample_rate << 2) & 0x3c;
        // channel_configuration(3): byte2 하위 1비트 + byte3 상위 2비트
        adts_header[2] |= (codec->aac_channels >> 2) & 0x01;
        adts_header[3] = (codec->aac_channels << 6) & 0xc0;
        // frame_length(13): byte3 하위 2비트 + byte4 + byte5 상위 3비트
        adts_header[3] |= (frame_length >> 11) & 0x03;
        adts_header[4] = (frame_length >> 3) & 0xff;
        adts_header[5] = (frame_length << 5) & 0xe0;
        // adts_buffer_fullness(11, 0x7ff=VBR): byte5 하위 5비트 + byte6 상위 6비트
        adts_header[5] |= 0x1f;

        audio->payload->append((const char*)adts_header, sizeof(adts_header));
        audio->payload->append(sample->bytes, size);
    }

    return err;
}

// Append an annex-b start code before a NALU:
// 액세스 유닛의 첫 NALU는 4바이트(00 00 00 01), 이후는 3바이트(00 00 01).
static void srs_avc_insert_aud(SrsSimpleStream* payload, bool& aud_inserted)
{
    static uint8_t fresh_nalu_header[] = {0x00, 0x00, 0x00, 0x01};
    static uint8_t cont_nalu_header[] = {0x00, 0x00, 0x01};

    if (!aud_inserted) {
        aud_inserted = true;
        payload->append((const char*)fresh_nalu_header, 4);
    } else {
        payload->append((const char*)cont_nalu_header, 3);
    }
}

srs_error_t SrsTsMessageCache::do_cache_avc(SrsVideoFrame* frame)
{
    srs_error_t err = srs_success;

    SrsVideoCodecConfig* codec = frame->vcodec();
    if (!codec || !codec->is_avc_codec_ok()) {
        return srs_error_new(ERROR_HLS_DECODE_ERROR, "avc sequence header not decoded");
    }

    bool aud_inserted = false;

    // TS의 액세스 유닛은 AUD(access unit delimiter)로 시작해야 한다.
    // 인코더가 이미 넣었으면 생략, 없으면 기본 AUD(0x09 0xf0 — 모든 slice type 허용)를 삽입.
    bool has_aud = false;
    for (int i = 0; i < frame->nb_samples; i++) {
        SrsAvcNaluType nalu_type = (SrsAvcNaluType)(frame->samples[i].bytes[0] & 0x1f);
        if (nalu_type == SrsAvcNaluTypeAccessUnitDelimiter) {
            has_aud = true;
            break;
        }
    }
    if (!has_aud) {
        static uint8_t default_aud_nalu[] = {0x09, 0xf0};
        srs_avc_insert_aud(video->payload, aud_inserted);
        video->payload->append((const char*)default_aud_nalu, 2);
    }

    // IDR 앞에는 SPS/PPS가 있어야 세그먼트 중간부터도 디코딩 가능하다.
    // FLV에서는 시퀀스 헤더로 한 번만 오므로, 캐시해 둔 것을 여기서 재삽입한다.
    bool is_sps_pps_appended = false;
    for (int i = 0; i < frame->nb_samples; i++) {
        SrsSample* sample = &frame->samples[i];
        SrsAvcNaluType nalu_type = (SrsAvcNaluType)(sample->bytes[0] & 0x1f);

        if (nalu_type == SrsAvcNaluTypeIDR && !is_sps_pps_appended) {
            if (!codec->sequenceParameterSetNALUnit.empty()) {
                srs_avc_insert_aud(video->payload, aud_inserted);
                video->payload->append(codec->sequenceParameterSetNALUnit.data(), (int)codec->sequenceParameterSetNALUnit.size());
            }
            if (!codec->pictureParameterSetNALUnit.empty()) {
                srs_avc_insert_aud(video->payload, aud_inserted);
                video->payload->append(codec->pictureParameterSetNALUnit.data(), (int)codec->pictureParameterSetNALUnit.size());
            }
            is_sps_pps_appended = true;
        }

        srs_avc_insert_aud(video->payload, aud_inserted);
        video->payload->append(sample->bytes, sample->size);
    }

    return err;
}
