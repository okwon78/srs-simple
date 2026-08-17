// srs_simple — 원본: trunk/src/kernel/srs_kernel_flv.cpp (+ srs_kernel_utility.cpp의 c0/c3)
#include <srs_kernel_flv.hpp>

#include <srs_kernel_error.hpp>

SrsMessageHeader::SrsMessageHeader()
{
    message_type = 0;
    payload_length = 0;
    timestamp_delta = 0;
    stream_id = 0;

    timestamp = 0;
    // we always use the connection chunk-id
    prefer_cid = RTMP_CID_OverConnection;
}

SrsMessageHeader::~SrsMessageHeader()
{
}

bool SrsMessageHeader::is_audio()
{
    return message_type == RTMP_MSG_AudioMessage;
}

bool SrsMessageHeader::is_video()
{
    return message_type == RTMP_MSG_VideoMessage;
}

bool SrsMessageHeader::is_amf0_command()
{
    return message_type == RTMP_MSG_AMF0CommandMessage;
}

bool SrsMessageHeader::is_amf0_data()
{
    return message_type == RTMP_MSG_AMF0DataMessage;
}

bool SrsMessageHeader::is_amf3_command()
{
    return message_type == RTMP_MSG_AMF3CommandMessage;
}

bool SrsMessageHeader::is_amf3_data()
{
    return message_type == RTMP_MSG_AMF3DataMessage;
}

bool SrsMessageHeader::is_window_ackledgement_size()
{
    return message_type == RTMP_MSG_WindowAcknowledgementSize;
}

bool SrsMessageHeader::is_ackledgement()
{
    return message_type == RTMP_MSG_Acknowledgement;
}

bool SrsMessageHeader::is_set_chunk_size()
{
    return message_type == RTMP_MSG_SetChunkSize;
}

bool SrsMessageHeader::is_user_control_message()
{
    return message_type == RTMP_MSG_UserControlMessage;
}

bool SrsMessageHeader::is_set_peer_bandwidth()
{
    return message_type == RTMP_MSG_SetPeerBandwidth;
}

bool SrsMessageHeader::is_aggregate()
{
    return message_type == RTMP_MSG_AggregateMessage;
}

void SrsMessageHeader::initialize_amf0_script(int size, int stream)
{
    message_type = RTMP_MSG_AMF0DataMessage;
    payload_length = (int32_t)size;
    timestamp_delta = (int32_t)0;
    timestamp = (int64_t)0;
    stream_id = (int32_t)stream;

    // amf0 script use connection2 chunk-id
    prefer_cid = RTMP_CID_OverConnection2;
}

void SrsMessageHeader::initialize_audio(int size, uint32_t time, int stream)
{
    message_type = RTMP_MSG_AudioMessage;
    payload_length = (int32_t)size;
    timestamp_delta = (int32_t)time;
    timestamp = (int64_t)time;
    stream_id = (int32_t)stream;

    // audio chunk-id
    prefer_cid = RTMP_CID_Audio;
}

void SrsMessageHeader::initialize_video(int size, uint32_t time, int stream)
{
    message_type = RTMP_MSG_VideoMessage;
    payload_length = (int32_t)size;
    timestamp_delta = (int32_t)time;
    timestamp = (int64_t)time;
    stream_id = (int32_t)stream;

    // video chunk-id
    prefer_cid = RTMP_CID_Video;
}

SrsCommonMessage::SrsCommonMessage()
{
    payload = NULL;
    size = 0;
}

SrsCommonMessage::~SrsCommonMessage()
{
    srs_freepa(payload);
}

void SrsCommonMessage::create_payload(int size)
{
    srs_freepa(payload);

    payload = new char[size];
}

srs_error_t SrsCommonMessage::create(SrsMessageHeader* pheader, char* body, int size)
{
    // drop previous payload.
    srs_freepa(payload);

    this->header = *pheader;
    this->payload = body;
    this->size = size;

    return srs_success;
}

SrsSharedMessageHeader::SrsSharedMessageHeader()
{
    payload_length = 0;
    message_type = 0;
    prefer_cid = 0;
}

SrsSharedMessageHeader::~SrsSharedMessageHeader()
{
}

SrsSharedPtrMessage::SrsSharedPtrPayload::SrsSharedPtrPayload()
{
    payload = NULL;
    size = 0;
    shared_count = 0;
}

SrsSharedPtrMessage::SrsSharedPtrPayload::~SrsSharedPtrPayload()
{
    srs_freepa(payload);
}

SrsSharedPtrMessage::SrsSharedPtrMessage() : timestamp(0), stream_id(0), size(0), payload(NULL)
{
    ptr = NULL;
}

SrsSharedPtrMessage::~SrsSharedPtrMessage()
{
    if (ptr) {
        if (ptr->shared_count == 0) {
            srs_freep(ptr);
        } else {
            ptr->shared_count--;
        }
    }
}

srs_error_t SrsSharedPtrMessage::create(SrsCommonMessage* msg)
{
    srs_error_t err = srs_success;

    if ((err = create(&msg->header, msg->payload, msg->size)) != srs_success) {
        return srs_error_wrap(err, "create message");
    }

    // to prevent double free of payload:
    // initialize already attach the payload of msg,
    // detach the payload to transfer the owner to shared ptr.
    msg->payload = NULL;
    msg->size = 0;

    return err;
}

srs_error_t SrsSharedPtrMessage::create(SrsMessageHeader* pheader, char* payload, int size)
{
    srs_error_t err = srs_success;

    if (size < 0) {
        return srs_error_new(ERROR_RTMP_MESSAGE_CREATE, "create message size=%d", size);
    }

    srs_assert(!ptr);
    ptr = new SrsSharedPtrPayload();

    // direct attach the data.
    if (pheader) {
        ptr->header.message_type = pheader->message_type;
        ptr->header.payload_length = size;
        ptr->header.prefer_cid = pheader->prefer_cid;
        this->timestamp = pheader->timestamp;
        this->stream_id = pheader->stream_id;
    }
    ptr->payload = payload;
    ptr->size = size;

    // message can access it.
    this->payload = ptr->payload;
    this->size = ptr->size;

    return err;
}

int SrsSharedPtrMessage::count()
{
    return ptr ? ptr->shared_count : 0;
}

bool SrsSharedPtrMessage::check(int stream_id)
{
    // Ignore error when message has no payload.
    if (!ptr) {
        return true;
    }

    // we donot use the complex basic header,
    // ensure the basic header is 1bytes.
    if (ptr->header.prefer_cid < 2 || ptr->header.prefer_cid > 63) {
        ptr->header.prefer_cid = RTMP_CID_ProtocolControl;
    }

    // we assume that the stream_id in a group must be the same.
    if (this->stream_id == stream_id) {
        return true;
    }
    this->stream_id = stream_id;

    return false;
}

bool SrsSharedPtrMessage::is_av()
{
    return ptr->header.message_type == RTMP_MSG_AudioMessage
        || ptr->header.message_type == RTMP_MSG_VideoMessage;
}

bool SrsSharedPtrMessage::is_audio()
{
    return ptr->header.message_type == RTMP_MSG_AudioMessage;
}

bool SrsSharedPtrMessage::is_video()
{
    return ptr->header.message_type == RTMP_MSG_VideoMessage;
}

int SrsSharedPtrMessage::chunk_header(char* cache, int nb_cache, bool c0)
{
    if (c0) {
        return srs_chunk_header_c0(ptr->header.prefer_cid, (uint32_t)timestamp,
            ptr->header.payload_length, ptr->header.message_type, stream_id, cache, nb_cache);
    } else {
        return srs_chunk_header_c3(ptr->header.prefer_cid, (uint32_t)timestamp, cache, nb_cache);
    }
}

SrsSharedPtrMessage* SrsSharedPtrMessage::copy()
{
    srs_assert(ptr);

    SrsSharedPtrMessage* copy = new SrsSharedPtrMessage();

    // Reference to this message instead.
    copy->ptr = ptr;
    ptr->shared_count++;

    copy->timestamp = timestamp;
    copy->stream_id = stream_id;
    copy->payload = ptr->payload;
    copy->size = ptr->size;

    return copy;
}

int srs_chunk_header_c0(int prefer_cid, uint32_t timestamp, int32_t payload_length,
    int8_t message_type, int32_t stream_id, char* cache, int nb_cache)
{
    // to directly set the field.
    char* pp = NULL;

    // generate the header.
    char* p = cache;

    // no header.
    if (nb_cache < SRS_CONSTS_RTMP_MAX_FMT0_HEADER_SIZE) {
        return 0;
    }

    // write new chunk stream header, fmt is 0
    *p++ = 0x00 | (prefer_cid & 0x3F);

    // chunk message header, 11 bytes
    // timestamp, 3bytes, big-endian
    if (timestamp < RTMP_EXTENDED_TIMESTAMP) {
        pp = (char*)&timestamp;
        *p++ = pp[2];
        *p++ = pp[1];
        *p++ = pp[0];
    } else {
        *p++ = (char)0xFF;
        *p++ = (char)0xFF;
        *p++ = (char)0xFF;
    }

    // message_length, 3bytes, big-endian
    pp = (char*)&payload_length;
    *p++ = pp[2];
    *p++ = pp[1];
    *p++ = pp[0];

    // message_type, 1bytes
    *p++ = message_type;

    // stream_id, 4bytes, little-endian
    pp = (char*)&stream_id;
    *p++ = pp[0];
    *p++ = pp[1];
    *p++ = pp[2];
    *p++ = pp[3];

    // chunk extended timestamp header, 0 or 4 bytes, big-endian
    // 6.1.3. Extended Timestamp
    // This field is transmitted only when the normal time stamp in the
    // chunk message header is set to 0x00ffffff.
    if (timestamp >= RTMP_EXTENDED_TIMESTAMP) {
        pp = (char*)&timestamp;
        *p++ = pp[3];
        *p++ = pp[2];
        *p++ = pp[1];
        *p++ = pp[0];
    }

    // always has header
    return (int)(p - cache);
}

int srs_chunk_header_c3(int prefer_cid, uint32_t timestamp, char* cache, int nb_cache)
{
    // to directly set the field.
    char* pp = NULL;

    // generate the header.
    char* p = cache;

    // no header.
    if (nb_cache < SRS_CONSTS_RTMP_MAX_FMT3_HEADER_SIZE) {
        return 0;
    }

    // write no message header chunk stream, fmt is 3
    // @remark, if prefer_cid > 0x3F, that is, use 2B/3B chunk header,
    // SRS will rollback to 1B chunk header.
    *p++ = 0xC0 | (prefer_cid & 0x3F);

    // chunk extended timestamp header, 0 or 4 bytes, big-endian
    // 스펙은 "Type 3 chunks MUST NOT have this field"라고 하지만,
    // adobe(FMLE/FMS/flash-player)는 fmt=3에도 extended timestamp를 항상 보낸다 — 원본 주석 참조.
    if (timestamp >= RTMP_EXTENDED_TIMESTAMP) {
        pp = (char*)&timestamp;
        *p++ = pp[3];
        *p++ = pp[2];
        *p++ = pp[1];
        *p++ = pp[0];
    }

    // always has header
    return (int)(p - cache);
}
