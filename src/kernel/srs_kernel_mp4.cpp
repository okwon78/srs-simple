// srs_simple — 원본: trunk/src/kernel/srs_kernel_mp4.cpp
// fMP4 인코더 구현. 박스 클래스 트리 없이 SrsBuffer로 바이트를 직접 조립한다 (S12).
// 박스 레이아웃 근거: ISO_IEC_14496-12-base-format-2012.pdf (원본 박스 클래스의 encode와 동일).
#include <srs_kernel_mp4.hpp>

#include <string.h>

#include <srs_kernel_buffer.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

using namespace std;

// ---- 박스 조립 헬퍼 (파일-로컬) ----
// 박스 프리앰블: size(4B)는 아직 모르므로 0으로 쓰고 시작 오프셋을 반환,
// srs_mp4_box_close가 (현재 위치 - 시작)을 역산해 패치한다.
// 원본은 nb_bytes()로 전체 크기를 선계산하지만(클래스 트리 필요), 우리는 후패치가 단순하다.
static int srs_mp4_box_open(SrsBuffer* b, const char* type)
{
    int start = b->pos();
    b->write_4bytes(0);
    b->write_bytes((char*)type, 4);
    return start;
}

static void srs_mp4_box_close(SrsBuffer* b, int start)
{
    int size = b->pos() - start;
    uint8_t* p = (uint8_t*)b->data() + start;
    p[0] = (uint8_t)((size >> 24) & 0xff);
    p[1] = (uint8_t)((size >> 16) & 0xff);
    p[2] = (uint8_t)((size >> 8) & 0xff);
    p[3] = (uint8_t)(size & 0xff);
}

// The full box: 박스 프리앰블 + version(1B) + flags(3B).
static int srs_mp4_full_box_open(SrsBuffer* b, const char* type, uint8_t version, uint32_t flags)
{
    int start = srs_mp4_box_open(b, type);
    b->write_1bytes((int8_t)version);
    b->write_3bytes((int32_t)flags);
    return start;
}

// The unity matrix of mvhd/tkhd.
static void srs_mp4_write_matrix(SrsBuffer* b)
{
    b->write_4bytes(0x00010000); b->write_4bytes(0); b->write_4bytes(0);
    b->write_4bytes(0); b->write_4bytes(0x00010000); b->write_4bytes(0);
    b->write_4bytes(0); b->write_4bytes(0); b->write_4bytes(0x40000000);
}

SrsMp4Sample::SrsMp4Sample()
{
    type = SrsMp4HandlerTypeForbidden;
    dts = pts = 0;
    frame_type = SrsVideoAvcFrameTypeForbidden;
    nb_data = 0;
}

SrsMp4Sample::~SrsMp4Sample()
{
}

SrsMp4SampleManager::SrsMp4SampleManager()
{
}

SrsMp4SampleManager::~SrsMp4SampleManager()
{
}

void SrsMp4SampleManager::append(const SrsMp4Sample& sample)
{
    samples.push_back(sample);
}

SrsMp4M2tsInitEncoder::SrsMp4M2tsInitEncoder()
{
    writer = NULL;
}

SrsMp4M2tsInitEncoder::~SrsMp4M2tsInitEncoder()
{
}

srs_error_t SrsMp4M2tsInitEncoder::initialize(ISrsWriter* w)
{
    writer = w;
    return srs_success;
}

srs_error_t SrsMp4M2tsInitEncoder::write(SrsFormat* format, bool video, int tid)
{
    // 원본 시그니처 — 트랙 하나짜리 init.mp4 (원본: srs_kernel_mp4.cpp:6258).
    if (video) {
        return do_write(format, true, tid, false, 0);
    }
    return do_write(format, false, 0, true, tid);
}

srs_error_t SrsMp4M2tsInitEncoder::write(SrsFormat* format)
{
    // Muxed 확장(D2): video tid=1, audio tid=2 고정.
    return do_write(format, true, 1, true, 2);
}

srs_error_t SrsMp4M2tsInitEncoder::do_write(SrsFormat* format, bool has_video, int vid, bool has_audio, int aid)
{
    srs_error_t err = srs_success;

    if (has_video && (!format->vcodec || !format->vcodec->is_avc_codec_ok())) {
        return srs_error_new(ERROR_MP4_ILLEGAL_MOOF, "no video sequence header");
    }
    if (has_audio && (!format->acodec || !format->acodec->is_aac_codec_ok())) {
        return srs_error_new(ERROR_MP4_ILLEGAL_MOOF, "no audio sequence header");
    }

    int nb_data = 2048;
    if (has_video) nb_data += (int)format->vcodec->avc_extra_data.size();
    if (has_audio) nb_data += (int)format->acodec->aac_extra_data.size();
    vector<char> data(nb_data);
    SrsBuffer b(&data[0], nb_data);

    // Write ftyp box. major iso5(CMAF), compat iso6/mp41 (원본 :6263).
    if (true) {
        int ftyp = srs_mp4_box_open(&b, "ftyp");
        b.write_bytes((char*)"iso5", 4);
        b.write_4bytes(512); // minor_version
        b.write_bytes((char*)"iso6", 4);
        b.write_bytes((char*)"mp41", 4);
        srs_mp4_box_close(&b, ftyp);
    }

    // Write moov box.
    if (true) {
        int moov = srs_mp4_box_open(&b, "moov");

        // mvhd. timescale 1000(ms), duration 0 — fragmented라 mvex/moof가 정의한다.
        if (true) {
            int mvhd = srs_mp4_full_box_open(&b, "mvhd", 0, 0);
            b.write_4bytes(0); // creation_time
            b.write_4bytes(0); // modification_time
            b.write_4bytes(1000); // timescale — use tbn ms
            b.write_4bytes(0); // duration_in_tbn
            b.write_4bytes(0x00010000); // rate 1.0
            b.write_2bytes(0x0100); // volume 1.0
            b.write_2bytes(0); // reserved
            b.write_4bytes(0); b.write_4bytes(0); // reserved
            srs_mp4_write_matrix(&b);
            for (int i = 0; i < 6; i++) b.write_4bytes(0); // pre_defined
            int next_tid = (has_audio ? aid : vid) + 1;
            b.write_4bytes(next_tid); // next_track_ID
            srs_mp4_box_close(&b, mvhd);
        }

        if (has_video) {
            write_video_trak(&b, format->vcodec, vid);
        }
        if (has_audio) {
            write_audio_trak(&b, format->acodec, aid);
        }

        // mvex: trex per track — fragmented 필수 (moof의 traf가 이 트랙을 참조).
        if (true) {
            int mvex = srs_mp4_box_open(&b, "mvex");
            int tids[2];
            int nb_tids = 0;
            if (has_video) tids[nb_tids++] = vid;
            if (has_audio) tids[nb_tids++] = aid;
            for (int i = 0; i < nb_tids; i++) {
                int trex = srs_mp4_full_box_open(&b, "trex", 0, 0);
                b.write_4bytes(tids[i]); // track_ID
                b.write_4bytes(1); // default_sample_description_index
                b.write_4bytes(0); // default_sample_duration
                b.write_4bytes(0); // default_sample_size
                b.write_4bytes(0); // default_sample_flags
                srs_mp4_box_close(&b, trex);
            }
            srs_mp4_box_close(&b, mvex);
        }

        srs_mp4_box_close(&b, moov);
    }

    if ((err = writer->write(b.data(), b.pos(), NULL)) != srs_success) {
        return srs_error_wrap(err, "write init mp4");
    }

    return err;
}

void SrsMp4M2tsInitEncoder::write_video_trak(SrsBuffer* b, SrsVideoCodecConfig* vcodec, int tid)
{
    int trak = srs_mp4_box_open(b, "trak");

    // tkhd. flags 3 = enabled | in_movie.
    // width/height는 SPS에서 파싱한 해상도 (S16 — avc_demux_sps). Chrome MSE는
    // avc1/tkhd의 해상도 0을 invalid decoder config로 거부한다 (§5.6 S16).
    if (true) {
        int tkhd = srs_mp4_full_box_open(b, "tkhd", 0, 3);
        b->write_4bytes(0); // creation_time
        b->write_4bytes(0); // modification_time
        b->write_4bytes(tid); // track_ID
        b->write_4bytes(0); // reserved
        b->write_4bytes(0); // duration
        b->write_4bytes(0); b->write_4bytes(0); // reserved
        b->write_2bytes(0); // layer
        b->write_2bytes(0); // alternate_group
        b->write_2bytes(0); // volume — video는 0
        b->write_2bytes(0); // reserved
        srs_mp4_write_matrix(b);
        b->write_4bytes(vcodec->width << 16); // width (16.16 fixed)
        b->write_4bytes(vcodec->height << 16); // height
        srs_mp4_box_close(b, tkhd);
    }

    int mdia = srs_mp4_box_open(b, "mdia");

    // mdhd. language 'und' = 0x55c4.
    if (true) {
        int mdhd = srs_mp4_full_box_open(b, "mdhd", 0, 0);
        b->write_4bytes(0); // creation_time
        b->write_4bytes(0); // modification_time
        b->write_4bytes(1000); // timescale
        b->write_4bytes(0); // duration
        b->write_2bytes(0x55c4); // language: und
        b->write_2bytes(0); // pre_defined
        srs_mp4_box_close(b, mdhd);
    }

    // hdlr.
    if (true) {
        int hdlr = srs_mp4_full_box_open(b, "hdlr", 0, 0);
        b->write_4bytes(0); // pre_defined
        b->write_bytes((char*)"vide", 4); // handler_type
        b->write_4bytes(0); b->write_4bytes(0); b->write_4bytes(0); // reserved
        b->write_bytes((char*)"VideoHandler", 12);
        b->write_1bytes(0); // null-terminated
        srs_mp4_box_close(b, hdlr);
    }

    int minf = srs_mp4_box_open(b, "minf");

    // vmhd. flags 1 (스펙 규정), graphicsmode/opcolor 0.
    if (true) {
        int vmhd = srs_mp4_full_box_open(b, "vmhd", 0, 1);
        b->write_2bytes(0); // graphicsmode
        b->write_2bytes(0); b->write_2bytes(0); b->write_2bytes(0); // opcolor
        srs_mp4_box_close(b, vmhd);
    }

    // dinf > dref > url (self-contained).
    if (true) {
        int dinf = srs_mp4_box_open(b, "dinf");
        int dref = srs_mp4_full_box_open(b, "dref", 0, 0);
        b->write_4bytes(1); // entry_count
        int url = srs_mp4_full_box_open(b, "url ", 0, 1);
        srs_mp4_box_close(b, url);
        srs_mp4_box_close(b, dref);
        srs_mp4_box_close(b, dinf);
    }

    int stbl = srs_mp4_box_open(b, "stbl");

    // stsd > avc1 > avcC. 샘플 자체는 moof에 있으므로 코덱 설정만 싣는다.
    if (true) {
        int stsd = srs_mp4_full_box_open(b, "stsd", 0, 0);
        b->write_4bytes(1); // entry_count

        int avc1 = srs_mp4_box_open(b, "avc1");
        for (int i = 0; i < 6; i++) b->write_1bytes(0); // reserved
        b->write_2bytes(1); // data_reference_index
        b->write_2bytes(0); // pre_defined
        b->write_2bytes(0); // reserved
        for (int i = 0; i < 3; i++) b->write_4bytes(0); // pre_defined
        b->write_2bytes(vcodec->width); // width — SPS에서 파싱 (S16, tkhd와 동일)
        b->write_2bytes(vcodec->height); // height
        b->write_4bytes(0x00480000); // horizresolution 72dpi
        b->write_4bytes(0x00480000); // vertresolution 72dpi
        b->write_4bytes(0); // reserved
        b->write_2bytes(1); // frame_count
        for (int i = 0; i < 32; i++) b->write_1bytes(0); // compressorname
        b->write_2bytes(0x0018); // depth
        b->write_2bytes(-1); // pre_defined

        // avcC: 시퀀스 헤더의 avcC 원문을 그대로 (원본도 avc_extra_data 그대로 — :6348).
        int avcC = srs_mp4_box_open(b, "avcC");
        b->write_bytes(&vcodec->avc_extra_data[0], (int)vcodec->avc_extra_data.size());
        srs_mp4_box_close(b, avcC);

        srs_mp4_box_close(b, avc1);
        srs_mp4_box_close(b, stsd);
    }

    // Fragmented라 샘플 테이블은 전부 빈 채움 (스펙상 필수 박스라 생략은 불가).
    if (true) {
        int stts = srs_mp4_full_box_open(b, "stts", 0, 0);
        b->write_4bytes(0); // entry_count
        srs_mp4_box_close(b, stts);

        int stsc = srs_mp4_full_box_open(b, "stsc", 0, 0);
        b->write_4bytes(0); // entry_count
        srs_mp4_box_close(b, stsc);

        int stsz = srs_mp4_full_box_open(b, "stsz", 0, 0);
        b->write_4bytes(0); // sample_size
        b->write_4bytes(0); // sample_count
        srs_mp4_box_close(b, stsz);

        int stco = srs_mp4_full_box_open(b, "stco", 0, 0);
        b->write_4bytes(0); // entry_count
        srs_mp4_box_close(b, stco);
    }

    srs_mp4_box_close(b, stbl);
    srs_mp4_box_close(b, minf);
    srs_mp4_box_close(b, mdia);
    srs_mp4_box_close(b, trak);
}

void SrsMp4M2tsInitEncoder::write_audio_trak(SrsBuffer* b, SrsAudioCodecConfig* acodec, int tid)
{
    int trak = srs_mp4_box_open(b, "trak");

    // tkhd. audio는 volume 1.0.
    if (true) {
        int tkhd = srs_mp4_full_box_open(b, "tkhd", 0, 3);
        b->write_4bytes(0); // creation_time
        b->write_4bytes(0); // modification_time
        b->write_4bytes(tid); // track_ID
        b->write_4bytes(0); // reserved
        b->write_4bytes(0); // duration
        b->write_4bytes(0); b->write_4bytes(0); // reserved
        b->write_2bytes(0); // layer
        b->write_2bytes(0); // alternate_group
        b->write_2bytes(0x0100); // volume 1.0
        b->write_2bytes(0); // reserved
        srs_mp4_write_matrix(b);
        b->write_4bytes(0); // width
        b->write_4bytes(0); // height
        srs_mp4_box_close(b, tkhd);
    }

    int mdia = srs_mp4_box_open(b, "mdia");

    if (true) {
        int mdhd = srs_mp4_full_box_open(b, "mdhd", 0, 0);
        b->write_4bytes(0); // creation_time
        b->write_4bytes(0); // modification_time
        b->write_4bytes(1000); // timescale
        b->write_4bytes(0); // duration
        b->write_2bytes(0x55c4); // language: und
        b->write_2bytes(0); // pre_defined
        srs_mp4_box_close(b, mdhd);
    }

    if (true) {
        int hdlr = srs_mp4_full_box_open(b, "hdlr", 0, 0);
        b->write_4bytes(0); // pre_defined
        b->write_bytes((char*)"soun", 4); // handler_type
        b->write_4bytes(0); b->write_4bytes(0); b->write_4bytes(0); // reserved
        b->write_bytes((char*)"SoundHandler", 12);
        b->write_1bytes(0); // null-terminated
        srs_mp4_box_close(b, hdlr);
    }

    int minf = srs_mp4_box_open(b, "minf");

    // smhd.
    if (true) {
        int smhd = srs_mp4_full_box_open(b, "smhd", 0, 0);
        b->write_2bytes(0); // balance
        b->write_2bytes(0); // reserved
        srs_mp4_box_close(b, smhd);
    }

    if (true) {
        int dinf = srs_mp4_box_open(b, "dinf");
        int dref = srs_mp4_full_box_open(b, "dref", 0, 0);
        b->write_4bytes(1); // entry_count
        int url = srs_mp4_full_box_open(b, "url ", 0, 1);
        srs_mp4_box_close(b, url);
        srs_mp4_box_close(b, dref);
        srs_mp4_box_close(b, dinf);
    }

    int stbl = srs_mp4_box_open(b, "stbl");

    // stsd > mp4a > esds.
    if (true) {
        int stsd = srs_mp4_full_box_open(b, "stsd", 0, 0);
        b->write_4bytes(1); // entry_count

        int mp4a = srs_mp4_box_open(b, "mp4a");
        for (int i = 0; i < 6; i++) b->write_1bytes(0); // reserved
        b->write_2bytes(1); // data_reference_index
        b->write_4bytes(0); b->write_4bytes(0); // reserved (version/revision/vendor)
        b->write_2bytes(acodec->aac_channels); // channelcount
        b->write_2bytes(16); // samplesize
        b->write_2bytes(0); // pre_defined
        b->write_2bytes(0); // reserved
        // samplerate 16.16 fixed. 원본은 FLV SoundRate 테이블을 쓰지만(:6436) 우리는
        // ASC의 samplingFrequencyIndex — 실제 레이트라 더 정확하다 (FLV는 44.1k 근사).
        uint32_t srate = (uint32_t)srs_aac_srates[acodec->aac_sample_rate] << 16;
        b->write_4bytes((int32_t)srate);

        // esds: ES_Descriptor(0x03) > DecoderConfig(0x04) > DecSpecificInfo(0x05, ASC)
        //       + SLConfig(0x06). 길이는 전부 1바이트(base-128 불필요 — ASC ≤ 5B).
        if (true) {
            int esds = srs_mp4_full_box_open(b, "esds", 0, 0);
            int nb_asc = (int)acodec->aac_extra_data.size();

            b->write_1bytes(0x03); // ES_DescrTag
            b->write_1bytes(23 + nb_asc); // = 3 + (2+13+2+asc) + 3
            b->write_2bytes(0x02); // ES_ID (원본 :6453)
            b->write_1bytes(0x00); // flags: no depends/URL/OCR

            b->write_1bytes(0x04); // DecoderConfigDescrTag
            b->write_1bytes(15 + nb_asc); // = 13 + (2+asc)
            b->write_1bytes(0x40); // objectTypeIndication: Audio ISO/IEC 14496-3(AAC)
            b->write_1bytes(0x15); // streamType 5(audio) << 2 | reserved 1
            b->write_3bytes(0); // bufferSizeDB
            b->write_4bytes(0); // maxBitrate
            b->write_4bytes(0); // avgBitrate

            b->write_1bytes(0x05); // DecSpecificInfoTag
            b->write_1bytes(nb_asc);
            b->write_bytes(&acodec->aac_extra_data[0], nb_asc);

            b->write_1bytes(0x06); // SLConfigDescrTag
            b->write_1bytes(0x01);
            b->write_1bytes(0x02); // predefined: MP4

            srs_mp4_box_close(b, esds);
        }

        srs_mp4_box_close(b, mp4a);
        srs_mp4_box_close(b, stsd);
    }

    if (true) {
        int stts = srs_mp4_full_box_open(b, "stts", 0, 0);
        b->write_4bytes(0); // entry_count
        srs_mp4_box_close(b, stts);

        int stsc = srs_mp4_full_box_open(b, "stsc", 0, 0);
        b->write_4bytes(0); // entry_count
        srs_mp4_box_close(b, stsc);

        int stsz = srs_mp4_full_box_open(b, "stsz", 0, 0);
        b->write_4bytes(0); // sample_size
        b->write_4bytes(0); // sample_count
        srs_mp4_box_close(b, stsz);

        int stco = srs_mp4_full_box_open(b, "stco", 0, 0);
        b->write_4bytes(0); // entry_count
        srs_mp4_box_close(b, stco);
    }

    srs_mp4_box_close(b, stbl);
    srs_mp4_box_close(b, minf);
    srs_mp4_box_close(b, mdia);
    srs_mp4_box_close(b, trak);
}

SrsMp4M2tsSegmentEncoder::SrsMp4M2tsSegmentEncoder()
{
    writer = NULL;
    nb_audios = nb_videos = 0;
    samples = new SrsMp4SampleManager();
    sequence_number = 0;
    decode_basetime = 0;
    mdat_bytes = 0;
    track_id = 0;
    audio_tid_ = 0;
}

SrsMp4M2tsSegmentEncoder::~SrsMp4M2tsSegmentEncoder()
{
    srs_freep(samples);
}

srs_error_t SrsMp4M2tsSegmentEncoder::initialize(ISrsWriter* w, uint32_t sequence, srs_utime_t basetime, uint32_t tid)
{
    srs_error_t err = srs_success;

    writer = w;
    track_id = tid;
    sequence_number = sequence;
    decode_basetime = basetime;

    // Write styp box. MSE의 media segment는 [styp] moof mdat — styp은 선택이지만
    // 원본(:6521)대로 쓴다.
    if (true) {
        char data[24];
        SrsBuffer b(data, sizeof(data));
        int styp = srs_mp4_box_open(&b, "styp");
        b.write_bytes((char*)"msdh", 4);
        b.write_4bytes(0); // minor_version
        b.write_bytes((char*)"msdh", 4);
        b.write_bytes((char*)"msix", 4);
        srs_mp4_box_close(&b, styp);

        if ((err = writer->write(data, b.pos(), NULL)) != srs_success) {
            return srs_error_wrap(err, "write styp");
        }
    }

    return err;
}

void SrsMp4M2tsSegmentEncoder::set_audio_tid(uint32_t tid)
{
    audio_tid_ = tid;
}

srs_error_t SrsMp4M2tsSegmentEncoder::write_sample(SrsMp4HandlerType ht,
    uint16_t ft, uint32_t dts, uint32_t pts, uint8_t* sample, uint32_t nb_sample
) {
    srs_error_t err = srs_success;

    SrsMp4Sample ps;

    // We should copy the sample data, which is a shared ptr from the video/audio message.
    // 샘플별 힙 버퍼 대신 트랙별 연속 버퍼에 이어 붙인다 — 이 memcpy 1회가 파트의
    // 유일한 샘플 복사다 (헤더 주석, CLAUDE.md §5.6 S18).
    if (ht == SrsMp4HandlerTypeVIDE) {
        ps.frame_type = (SrsVideoAvcFrameType)ft;
        nb_videos++;
        video_data_.append((const char*)sample, nb_sample);
    } else if (ht == SrsMp4HandlerTypeSOUN) {
        nb_audios++;
        audio_data_.append((const char*)sample, nb_sample);
    } else {
        return err;
    }

    ps.type = ht;
    ps.dts = dts;
    ps.pts = pts;
    ps.nb_data = nb_sample;

    samples->append(ps);
    mdat_bytes += nb_sample;

    return err;
}

srs_error_t SrsMp4M2tsSegmentEncoder::flush(uint64_t& dts)
{
    srs_error_t err = srs_success;

    if (!nb_audios && !nb_videos) {
        return srs_error_new(ERROR_MP4_ILLEGAL_MOOF, "Missing audio and video track");
    }

    // 원본(:6586)은 여기서 sidx도 쓰지만 DASH 전용이라 제거 — 헤더 주석 참조.

    // Write moof: 버퍼에 조립 후 크기가 확정되면 trun의 data_offset을 역산해 패치.
    int nb_data = 256 + 16 * (int)samples->samples.size();
    vector<char> data(nb_data);
    SrsBuffer b(&data[0], nb_data);

    int moof = srs_mp4_box_open(&b, "moof");

    if (true) {
        int mfhd = srs_mp4_full_box_open(&b, "mfhd", 0, 0);
        b.write_4bytes(sequence_number);
        srs_mp4_box_close(&b, mfhd);
    }

    // Muxed(D2): 샘플이 있는 트랙마다 traf. video가 tid, audio는 set_audio_tid로
    // 지정된 값 — 파트 내용으로 추론하면 muxed 스트림의 오디오 전용 파트(세그먼트
    // 꼬리)가 비디오 트랙(1)에 실려 AAC가 h264로 디코딩되는 버그 (audio_tid_ 주석).
    // 미지정(0)이면 예전 추론 유지: 비디오 샘플이 있으면 tid+1, 없으면 tid.
    int video_offset_pos = -1;
    int audio_offset_pos = -1;
    if (nb_videos) {
        uint32_t vid = track_id;
        write_traf(&b, SrsMp4HandlerTypeVIDE, vid, dts, &video_offset_pos);
    }
    if (nb_audios) {
        uint32_t aid = audio_tid_ ? audio_tid_ : (nb_videos ? track_id + 1 : track_id);
        write_traf(&b, SrsMp4HandlerTypeSOUN, aid, dts, &audio_offset_pos);
    }

    srs_mp4_box_close(&b, moof);

    // mdat 페이로드는 video 샘플들 → audio 샘플들 순서. 각 trun의 data_offset은
    // moof 시작부터 그 트랙 첫 바이트까지 = moof 크기 + mdat 헤더(8) + 앞 트랙 크기.
    uint64_t video_bytes = (uint64_t)video_data_.size();

    int moof_bytes = b.pos();
    if (video_offset_pos >= 0) {
        uint8_t* p = (uint8_t*)b.data() + video_offset_pos;
        int32_t v = moof_bytes + 8;
        p[0] = (uint8_t)((v >> 24) & 0xff); p[1] = (uint8_t)((v >> 16) & 0xff);
        p[2] = (uint8_t)((v >> 8) & 0xff); p[3] = (uint8_t)(v & 0xff);
    }
    if (audio_offset_pos >= 0) {
        uint8_t* p = (uint8_t*)b.data() + audio_offset_pos;
        int32_t v = (int32_t)(moof_bytes + 8 + video_bytes);
        p[0] = (uint8_t)((v >> 24) & 0xff); p[1] = (uint8_t)((v >> 16) & 0xff);
        p[2] = (uint8_t)((v >> 8) & 0xff); p[3] = (uint8_t)(v & 0xff);
    }

    // Write moof + mdat(header + video bytes + audio bytes) in one writev —
    // 트랙 버퍼가 이미 연속이라 샘플 단위 write가 필요 없다 (S18).
    char mdat[8];
    SrsBuffer mb(mdat, sizeof(mdat));
    mb.write_4bytes((int32_t)(8 + mdat_bytes));
    mb.write_bytes((char*)"mdat", 4);

    iovec iovs[4];
    int nb_iovs = 0;
    iovs[nb_iovs].iov_base = b.data(); iovs[nb_iovs].iov_len = (size_t)b.pos(); nb_iovs++;
    iovs[nb_iovs].iov_base = mdat; iovs[nb_iovs].iov_len = sizeof(mdat); nb_iovs++;
    if (!video_data_.empty()) {
        iovs[nb_iovs].iov_base = (void*)video_data_.data(); iovs[nb_iovs].iov_len = video_data_.size(); nb_iovs++;
    }
    if (!audio_data_.empty()) {
        iovs[nb_iovs].iov_base = (void*)audio_data_.data(); iovs[nb_iovs].iov_len = audio_data_.size(); nb_iovs++;
    }

    if ((err = writer->writev(iovs, nb_iovs, NULL)) != srs_success) {
        return srs_error_wrap(err, "write moof+mdat");
    }

    return err;
}

void SrsMp4M2tsSegmentEncoder::write_traf(SrsBuffer* b, SrsMp4HandlerType ht, uint32_t tid,
    uint64_t end_dts, int* pdata_offset_pos)
{
    // 이 트랙의 샘플만 도착 순서(dts 오름차순)대로 모은다.
    vector<const SrsMp4Sample*> tses;
    vector<SrsMp4Sample>::const_iterator it;
    for (it = samples->samples.begin(); it != samples->samples.end(); ++it) {
        if (it->type == ht) {
            tses.push_back(&*it);
        }
    }

    int traf = srs_mp4_box_open(b, "traf");

    // tfhd. flags 0x020000 = default-base-is-moof: data_offset이 moof 시작 기준.
    if (true) {
        int tfhd = srs_mp4_full_box_open(b, "tfhd", 0, 0x020000);
        b->write_4bytes(tid);
        srs_mp4_box_close(b, tfhd);
    }

    // tfdt(v1, 64bit). 원본(:6633)은 basetime이지만 muxed는 트랙별 첫 샘플 dts —
    // 헤더 주석 참조. 호출자가 basetime = 첫 video dts로 주면 video는 원본과 같다.
    if (true) {
        int tfdt = srs_mp4_full_box_open(b, "tfdt", 1, 0);
        b->write_8bytes((int64_t)tses.front()->dts);
        srs_mp4_box_close(b, tfdt);
    }

    // trun. per-sample duration/size/flags/cts (원본 SrsMp4SampleManager::write:5048과
    // 같은 flags 구성). cts가 음수(B-frame)면 version 1.
    if (true) {
        uint8_t version = 0;
        for (size_t i = 0; i < tses.size(); i++) {
            if ((int64_t)(tses[i]->pts - tses[i]->dts) < 0) {
                version = 1;
                break;
            }
        }

        // flags: data-offset | sample-duration | sample-size | sample-flags | sample-cts
        int trun = srs_mp4_full_box_open(b, "trun", version, 0x000f01);
        b->write_4bytes((int32_t)tses.size()); // sample_count
        *pdata_offset_pos = b->pos(); // data_offset 자리 — flush가 패치
        b->write_4bytes(0);

        uint64_t previous_duration = 0;
        for (size_t i = 0; i < tses.size(); i++) {
            const SrsMp4Sample* sample = tses[i];

            // duration = 다음 샘플 dts와의 간격. 마지막은 end_dts로 닫되,
            // end_dts가 뒤(과거)면 직전 duration 재사용 (원본은 무검사 뺄셈 — 언더플로 방지).
            uint64_t duration;
            if (i + 1 < tses.size()) {
                duration = tses[i + 1]->dts - sample->dts;
            } else if (end_dts > sample->dts) {
                duration = end_dts - sample->dts;
            } else {
                duration = previous_duration;
            }
            previous_duration = duration;

            // sample_flags: 헤더 주석의 프레임 단위 마킹 (원본과 다른 지점).
            uint32_t flags;
            if (ht == SrsMp4HandlerTypeVIDE) {
                bool keyframe = (sample->frame_type == SrsVideoAvcFrameTypeKeyFrame);
                flags = keyframe ? 0x02000000 : 0x01010000;
            } else {
                flags = 0x02000000;
            }

            b->write_4bytes((int32_t)duration);
            b->write_4bytes((int32_t)sample->nb_data);
            b->write_4bytes((int32_t)flags);
            b->write_4bytes((int32_t)(sample->pts - sample->dts)); // cts (v1이면 signed)
        }
        srs_mp4_box_close(b, trun);
    }

    srs_mp4_box_close(b, traf);
}
