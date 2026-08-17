// srs_simple — 원본: trunk/src/kernel/srs_kernel_flv.hpp
// RTMP 메시지의 메모리 표현: 상수 + SrsMessageHeader + 수신용 SrsCommonMessage + 송신용 SrsSharedPtrMessage.
// FLV 파일 먹서/디코더(SrsFlvTransmuxer 등)는 라이브 경로에 불필요해 제거.
// srs_chunk_header_c0/c3는 원본 kernel/srs_kernel_utility.cpp에 있으나 여기로 병합 (CLAUDE.md §5.6).
#ifndef SRS_KERNEL_FLV_HPP
#define SRS_KERNEL_FLV_HPP

#include <srs_core.hpp>

#include <string>

class SrsBuffer;

// 5. Protocol Control Messages
// RTMP reserves message type IDs 1-7 for protocol control messages.
#define RTMP_MSG_SetChunkSize                   0x01
#define RTMP_MSG_AbortMessage                   0x02
#define RTMP_MSG_Acknowledgement                0x03
#define RTMP_MSG_UserControlMessage             0x04
#define RTMP_MSG_WindowAcknowledgementSize      0x05
#define RTMP_MSG_SetPeerBandwidth               0x06
// 3. Types of messages
// 3.1. Command message: AMF0 encoding = 20, AMF3 encoding = 17.
//      connect, createStream, publish, play 등의 RPC와 그 응답(_result, onStatus).
#define RTMP_MSG_AMF3CommandMessage             17 // 0x11
#define RTMP_MSG_AMF0CommandMessage             20 // 0x14
// 3.2. Data message: Metadata(onMetaData) 등. AMF0 = 18, AMF3 = 15.
#define RTMP_MSG_AMF0DataMessage                18 // 0x12
#define RTMP_MSG_AMF3DataMessage                15 // 0x0F
// 3.4. Audio message
#define RTMP_MSG_AudioMessage                   8 // 0x08
// 3.5. Video message
#define RTMP_MSG_VideoMessage                   9 // 0x09
// 3.6. Aggregate message (판별자만 유지 — 수신 시 거부용, CLAUDE.md §1 비목표)
#define RTMP_MSG_AggregateMessage               22 // 0x16

// The chunk stream id used for some under-layer message,
// For example, the PC(protocol control) message.
#define RTMP_CID_ProtocolControl                0x02
// The AMF0/AMF3 command message, invoke method and return the result, over NetConnection.
// generally use 0x03.
#define RTMP_CID_OverConnection                 0x03
// The AMF0/AMF3 command message, over NetConnection, the midst state(we guess).
// rarely used, e.g. onStatus(NetStream.Play.Reset).
#define RTMP_CID_OverConnection2                0x04
// The stream message(amf0/amf3), over NetStream.
// generally use 0x05.
#define RTMP_CID_OverStream                     0x05
// The stream message(video), over NetStream
// generally use 0x06.
#define RTMP_CID_Video                          0x06
// The stream message(audio), over NetStream.
// generally use 0x07.
#define RTMP_CID_Audio                          0x07

// 6.1. Chunk Format
// timestamp 3바이트 필드가 이 값(0xFFFFFF)이면 4바이트 extended timestamp가 뒤따른다.
#define RTMP_EXTENDED_TIMESTAMP                 0xFFFFFF

// 청크 헤더 최대 크기 (원본: kernel/srs_kernel_consts.hpp)
// fmt=0: basic(3) + message header(11) + extended timestamp(4) — basic 1바이트만 쓰므로 실제 최대 12/16.
#define SRS_CONSTS_RTMP_MAX_FMT0_HEADER_SIZE 16
// fmt=3: basic(1) + extended timestamp(4)
#define SRS_CONSTS_RTMP_MAX_FMT3_HEADER_SIZE 5

// 4.1. Message Header
class SrsMessageHeader
{
public:
    // 3bytes.
    // Three-byte field that contains a timestamp delta of the message.
    // @remark, only used for decoding message from chunk stream.
    int32_t timestamp_delta;
    // 3bytes.
    // Three-byte field that represents the size of the payload in bytes.
    // It is set in big-endian format.
    int32_t payload_length;
    // 1byte.
    // One byte field to represent the message type. A range of type IDs
    // (1-7) are reserved for protocol control messages.
    int8_t message_type;
    // 4bytes.
    // Four-byte field that identifies the stream of the message. These
    // bytes are set in little-endian format.
    int32_t stream_id;

    // Four-byte field that contains a timestamp of the message.
    // The 4 bytes are packed in the big-endian order.
    // @remark, used as calc timestamp when decode and encode time.
    // @remark, we use 64bits for large time for jitter detect.
    int64_t timestamp;
public:
    // Get the prefered cid(chunk stream id) which sendout over.
    // set at decoding, and canbe used for directly send message.
    int prefer_cid;
public:
    SrsMessageHeader();
    virtual ~SrsMessageHeader();
public:
    bool is_audio();
    bool is_video();
    bool is_amf0_command();
    bool is_amf0_data();
    bool is_amf3_command();
    bool is_amf3_data();
    bool is_window_ackledgement_size();
    bool is_ackledgement();
    bool is_set_chunk_size();
    bool is_user_control_message();
    bool is_set_peer_bandwidth();
    bool is_aggregate();
public:
    // Create a amf0 script header, set the size and stream_id.
    void initialize_amf0_script(int size, int stream);
    // Create a audio header, set the size, timestamp and stream_id.
    void initialize_audio(int size, uint32_t time, int stream);
    // Create a video header, set the size, timestamp and stream_id.
    void initialize_video(int size, uint32_t time, int stream);
};

// The message is raw data RTMP message, bytes oriented.
// The common message is read from underlay protocol sdk (수신 측, 페이로드 단일 소유),
// while the shared ptr message used to copy and send (송신 측) — CLAUDE.md §5.2.
class SrsCommonMessage
{
// 4.1. Message Header
public:
    SrsMessageHeader header;
// 4.2. Message Payload
public:
    // The current message parsed size,
    //       size <= header.payload_length
    // For the payload maybe sent in multiple chunks.
    int size;
    // The payload of message, the SrsCommonMessage never know about the detail of payload,
    // user must use SrsProtocol.decode_message to get concrete packet.
    // @remark, not all message payload can be decoded to packet. for example,
    //       video/audio packet use raw bytes, no video/audio packet.
    char* payload;
public:
    SrsCommonMessage();
    virtual ~SrsCommonMessage();
public:
    // Alloc the payload to specified size of bytes.
    virtual void create_payload(int size);
public:
    // Create common message, from the header and body.
    // @remark user should never free the body.
    // @param pheader, the header to copy to the message. NULL to ignore.
    virtual srs_error_t create(SrsMessageHeader* pheader, char* body, int size);
};

// The message header for shared ptr message.
// only the message for all msgs are same.
class SrsSharedMessageHeader
{
public:
    // 3bytes.
    // Three-byte field that represents the size of the payload in bytes.
    int32_t payload_length;
    // 1byte.
    // One byte field to represent the message type.
    int8_t message_type;
    // Get the prefered cid(chunk stream id) which sendout over.
    int prefer_cid;
public:
    SrsSharedMessageHeader();
    virtual ~SrsSharedMessageHeader();
};

// The shared ptr message.
// For audio/video/data message that need less memory copy.
// and only for output.
//
// Create first object by constructor and create(),
// use copy if need reference count message.
// 1MB 키프레임을 N명에게 팬아웃해도 payload 복사는 0회 — copy()는 refcount만 증가.
class SrsSharedPtrMessage
{
// 4.1. Message Header
public:
    // The header can shared, only set the timestamp and stream id.
    int64_t timestamp;
    int32_t stream_id;
// 4.2. Message Payload
public:
    // The current message parsed size,
    //       size <= header.payload_length
    int size;
    // The payload of message.
    char* payload;
private:
    class SrsSharedPtrPayload
    {
    public:
        // The shared message header.
        SrsSharedMessageHeader header;
        // The actual shared payload.
        char* payload;
        // The size of payload.
        int size;
        // The reference count
        int shared_count;
    public:
        SrsSharedPtrPayload();
        virtual ~SrsSharedPtrPayload();
    };
    SrsSharedPtrPayload* ptr;
public:
    SrsSharedPtrMessage();
    virtual ~SrsSharedPtrMessage();
public:
    // Create shared ptr message,
    // copy header, manage the payload of msg,
    // set the payload to NULL to prevent double free.
    // @remark payload of msg set to NULL if success.
    virtual srs_error_t create(SrsCommonMessage* msg);
    // Create shared ptr message, from the header and payload.
    // @remark user should never free the payload.
    // @param pheader, the header to copy to the message. NULL to ignore.
    virtual srs_error_t create(SrsMessageHeader* pheader, char* payload, int size);
    // Get current reference count.
    // when this object created, count set to 0.
    // if copy() this object, count increase 1.
    // if this or copy deleted, free payload when count is 0, or count--.
    // @remark, assert object is created.
    virtual int count();
    // check prefer cid and stream id.
    // @return whether stream id already set.
    virtual bool check(int stream_id);
public:
    virtual bool is_av();
    virtual bool is_audio();
    virtual bool is_video();
public:
    // generate the chunk header to cache.
    // @return the size of header.
    virtual int chunk_header(char* cache, int nb_cache, bool c0);
public:
    // copy current shared ptr message, use ref-count.
    // @remark, assert object is created.
    virtual SrsSharedPtrMessage* copy();
};

// 청크 헤더 직렬화 (원본: kernel/srs_kernel_utility.cpp:1184/1259)
// 송신은 항상 "첫 청크 fmt=0 + 이어지는 청크 fmt=3"만 사용한다 — CLAUDE.md §2.2.
// Generate the c0 chunk header for msg.
// @return the size of header. 0 if cache not enough.
extern int srs_chunk_header_c0(int prefer_cid, uint32_t timestamp, int32_t payload_length,
    int8_t message_type, int32_t stream_id, char* cache, int nb_cache);
// Generate the c3 chunk header for msg.
// @return the size of header. 0 if cache not enough.
extern int srs_chunk_header_c3(int prefer_cid, uint32_t timestamp, char* cache, int nb_cache);

#endif
