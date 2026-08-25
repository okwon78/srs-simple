// srs_simple — 원본: trunk/src/protocol/srs_protocol_rtmp_stack.hpp
// S5(1부): SrsPacket 베이스 + 컨트롤 패킷 5종 + SrsChunkStream + SrsProtocol (수신/송신).
// S6(2부): 커맨드 패킷 12종 + SrsRequest/SrsResponse + SrsRtmpServer (커맨드 흐름 파사드).
// 원본 대비 제거: iovec 배칭(SRS_PERF_COMPLEX_SEND), cs_cache(SRS_PERF_CHUNK_STREAM_CACHE),
// manual_response_queue(set_auto_response) — CLAUDE.md §5.6 S5.
// SrsRtmpClient와 Call/CloseStream/Pause 등 서버 라이브 경로에 불필요한 패킷 제거 — CLAUDE.md §5.6 S6.
#ifndef SRS_PROTOCOL_RTMP_STACK_HPP
#define SRS_PROTOCOL_RTMP_STACK_HPP

#include <srs_core.hpp>

#include <map>
#include <string>

#include <srs_kernel_error.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_protocol_io.hpp>

class SrsBuffer;
class SrsFastStream;
class SrsChunkStream;
class SrsCommonMessage;
class SrsSharedPtrMessage;
class SrsAmf0Any;
class SrsAmf0Object;
class SrsHandshakeBytes;
class SrsPacket;
class SrsRequest;
class SrsCreateStreamPacket;
class SrsFMLEStartPacket;
class SrsPublishPacket;
class SrsPlayPacket;

// The RTMP chunk size: 기본 128, 인바운드 SetChunkSize로 갱신 (원본: kernel/srs_kernel_consts.hpp)
#define SRS_CONSTS_RTMP_PROTOCOL_CHUNK_SIZE 128
// The valid chunk size range [128, 65536] — 밖이면 경고/거부 (이슈 #160/#541)
#define SRS_CONSTS_RTMP_MIN_CHUNK_SIZE 128
#define SRS_CONSTS_RTMP_MAX_CHUNK_SIZE 65536

// tcUrl의 기본값들 (원본: kernel/srs_kernel_consts.hpp — consts 파일을 만들지 않으므로 여기 정의)
#define SRS_CONSTS_RTMP_DEFAULT_PORT 1935
#define SRS_CONSTS_RTMP_DEFAULT_VHOST "__defaultVhost__"
#define SRS_CONSTS_RTMP_DEFAULT_APP "__defaultApp__"
// The onMetaData command names (원본: kernel/srs_kernel_consts.hpp)
#define SRS_CONSTS_RTMP_SET_DATAFRAME "@setDataFrame"
#define SRS_CONSTS_RTMP_ON_METADATA "onMetaData"

// The amf0 command message, command name macros
#define RTMP_AMF0_COMMAND_CONNECT "connect"
#define RTMP_AMF0_COMMAND_CREATE_STREAM "createStream"
#define RTMP_AMF0_COMMAND_CLOSE_STREAM "closeStream"
#define RTMP_AMF0_COMMAND_PLAY "play"
#define RTMP_AMF0_COMMAND_PAUSE "pause"
#define RTMP_AMF0_COMMAND_ON_BW_DONE "onBWDone"
#define RTMP_AMF0_COMMAND_ON_STATUS "onStatus"
#define RTMP_AMF0_COMMAND_RESULT "_result"
#define RTMP_AMF0_COMMAND_ERROR "_error"
#define RTMP_AMF0_COMMAND_RELEASE_STREAM "releaseStream"
#define RTMP_AMF0_COMMAND_FC_PUBLISH "FCPublish"
#define RTMP_AMF0_COMMAND_UNPUBLISH "FCUnpublish"
#define RTMP_AMF0_COMMAND_PUBLISH "publish"
#define RTMP_AMF0_DATA_SAMPLE_ACCESS "|RtmpSampleAccess"

// The signature for packets to client.
#define RTMP_SIG_FMS_VER "3,5,3,888"
#define RTMP_SIG_AMF0_VER 0
#define RTMP_SIG_CLIENT_ID "ASAICiss"

// The onStatus consts.
#define StatusLevel "level"
#define StatusCode "code"
#define StatusDescription "description"
#define StatusDetails "details"
#define StatusClientId "clientid"
// The status value
#define StatusLevelStatus "status"
// The status error
#define StatusLevelError "error"
// The code value
#define StatusCodeConnectSuccess "NetConnection.Connect.Success"
#define StatusCodeConnectRejected "NetConnection.Connect.Rejected"
#define StatusCodeStreamReset "NetStream.Play.Reset"
#define StatusCodeStreamStart "NetStream.Play.Start"
#define StatusCodeStreamPause "NetStream.Pause.Notify"
#define StatusCodeStreamUnpause "NetStream.Unpause.Notify"
#define StatusCodePublishStart "NetStream.Publish.Start"
#define StatusCodeDataStart "NetStream.Data.Start"
#define StatusCodeUnpublishSuccess "NetStream.Unpublish.Success"

// The decoded message payload.
// @remark we separate the packet from the message,
//        for the packet focuses on logic and domain data,
//        the message is bound to the protocol and focuses on the protocol, such as the header.
//         we could merge the message and packet, using an OOAD hierarchy where the packet extends the message,
//         but it's better for me to use composition -- the message uses the packet as payload.
class SrsPacket
{
public:
    SrsPacket();
    virtual ~SrsPacket();

public:
    // Convert the packet to a common message.
    virtual srs_error_t to_msg(SrsCommonMessage *msg, int stream_id);

public:
    // The subpacket can override this encode,
    // For example, video and audio will directly set the payload without a memory copy,
    // while other packets which need to serialize/encode to bytes do it by overriding
    // get_size and encode_packet.
    virtual srs_error_t encode(int &size, char *&payload);
    // Decode functions for concrete packet to override.
public:
    // The subpacket must override this to decode the packet from the stream.
    // @remark never invoke the super.decode, it always fails.
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    // The cid(chunk id) specifies the chunk to send data over.
    // Generally, each message prefers some cid, for example,
    // all protocol control messages prefer RTMP_CID_ProtocolControl,
    // SrsSetWindowAckSizePacket is a protocol control message.
    virtual int get_prefer_cid();
    // The subpacket must override this to provide the right message type.
    // The message type sets the RTMP message type in the header.
    virtual int get_message_type();

protected:
    // The subpacket can override this to calc the packet size.
    virtual int get_size();
    // The subpacket can override this to encode the payload to the stream.
    // @remark never invoke the super.encode_packet, it always fails.
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// The protocol provides the rtmp-message-protocol services,
// to recv RTMP messages from the RTMP chunk stream,
// and to send out RTMP messages over the RTMP chunk stream.
class SrsProtocol
{
private:
    class AckWindowSize
    {
    public:
        uint32_t window;
        // number of received bytes.
        int64_t nb_recv_bytes;
        // previously responded sequence number.
        uint32_t sequence_number;

        AckWindowSize();
    };
    // For peer in/out
private:
    // The underlying socket object, send/recv bytes.
    ISrsProtocolReadWriter *skt;
    // For peer in
private:
    // The chunk stream to decode RTMP messages.
    // 원본은 cid<16에 대해 cs_cache 배열을 추가로 두지만(성능), map만 사용한다 — CLAUDE.md §5.6.
    std::map<int, SrsChunkStream *> chunk_streams;
    // The bytes buffer cache, recv from skt, provides services for the stream.
    SrsFastStream *in_buffer;
    // The input chunk size, default to 128, set by peer packet.
    int32_t in_chunk_size;
    // The input ack window, to respond with an acknowledgement to the peer,
    // For example, to respond to the encoder, for the server got lots of packets.
    AckWindowSize in_ack_size;
    // The output ack window, to require the peer to respond with the ack.
    AckWindowSize out_ack_size;
    // The buffer length set by the peer(SetBufferLength).
    int32_t in_buffer_length;
    // Whether to print the protocol level debug info.
    // Generally we print the debug info when we get or send the first A/V packet.
    bool show_debug_info;
    // The requests sent out, used to build the response.
    // key: transactionId
    // value: the request command name
    // 커맨드(connect/createStream/FMLEStart)를 송신할 때 on_send_packet이 기록하고,
    // _result/_error 수신 시 do_decode_message가 tid로 원 커맨드를 찾아 응답 패킷 타입을 결정한다.
    std::map<double, std::string> requests;
    // For peer out
private:
    // The iovs cache for the simple send: header + payload.
    // 원본은 iovec 배칭(SRS_PERF_COMPLEX_SEND)으로 수백 개를 모아 보내지만, 청크당 writev 1회 — CLAUDE.md §5.6.
    iovec out_iovs[2];
    // The output header cache, for the c0/c3 chunk header of the chunk in flight.
    char out_c0c3_caches[SRS_CONSTS_RTMP_MAX_FMT0_HEADER_SIZE];
    // The output chunk size, default to 128, set by config.
    int32_t out_chunk_size;

public:
    SrsProtocol(ISrsProtocolReadWriter *io);
    virtual ~SrsProtocol();

public:
    // To set/get the recv timeout in srs_utime_t.
    // if it times out, recv/send message returns ERROR_SOCKET_TIMEOUT.
    virtual void set_recv_timeout(srs_utime_t tm);
    virtual srs_utime_t get_recv_timeout();
    // To set/get the send timeout in srs_utime_t.
    // if it times out, recv/send message returns ERROR_SOCKET_TIMEOUT.
    virtual void set_send_timeout(srs_utime_t tm);
    virtual srs_utime_t get_send_timeout();
    // Get recv/send bytes.
    virtual int64_t get_recv_bytes();
    virtual int64_t get_send_bytes();

public:
    // Set the input default ack size. This is generally set by the message from the peer,
    // but some encoders never send the ack message while defaulting to a non-zero size.
    // This will cause the encoder to block after publishing some messages to the server,
    // because it waits for the server to send an acknowledgement, but the server defaults to 0 which means there is no need
    // to ack the encoder. We can change the default input ack size. We will always respond with the
    // ack size whether the encoder sets it or not.
    virtual srs_error_t set_in_window_ack_size(int ack_size);

public:
    // Recv an RTMP message, which is bytes oriented.
    // the user can use decode_message to get the decoded RTMP packet.
    // @param pmsg, set the received message,
    //       always NULL if error,
    //       NULL for unknown packet but return success.
    //       never NULL if decode success.
    // @remark, drop message when msg is empty or payload length is empty.
    virtual srs_error_t recv_message(SrsCommonMessage **pmsg);
    // Decode a bytes oriented RTMP message to an RTMP packet,
    // @param ppacket, output decoded packet,
    //       always NULL if error, never NULL if success.
    // @return error when unknown packet, error when decode failed.
    virtual srs_error_t decode_message(SrsCommonMessage *msg, SrsPacket **ppacket);
    // Send the RTMP message and always free it.
    // the user must never free or use the msg after this method,
    // for it will always free the msg.
    // @param msg, the msg to send out, never NULL.
    // @param stream_id, the stream id of the packet to send over, 0 for a control message.
    virtual srs_error_t send_and_free_message(SrsSharedPtrMessage *msg, int stream_id);
    // Send the RTMP message and always free it.
    // the user must never free or use the msg after this method,
    // for it will always free the msg.
    // @param msgs, the msgs to send out, never NULL.
    // @param nb_msgs, the size of msgs to send out.
    // @param stream_id, the stream id of the packet to send over, 0 for a control message.
    virtual srs_error_t send_and_free_messages(SrsSharedPtrMessage **msgs, int nb_msgs, int stream_id);
    // Send the RTMP packet and always free it.
    // the user must never free or use the packet after this method,
    // for it will always free the packet.
    // @param packet, the packet to send out, never NULL.
    // @param stream_id, the stream id of the packet to send over, 0 for a control message.
    virtual srs_error_t send_and_free_packet(SrsPacket *packet, int stream_id);

public:
    // Expect a specified message, drop others until we get the specified one.
    // @pmsg, the user must free it. NULL if not success.
    // @ppacket, the user must free it, which is decoded from the payload of the message. NULL if not success.
    // @remark, only on success can the user use and must free the pmsg and ppacket.
    // For example:
    //          SrsCommonMessage* msg = NULL;
    //          SrsConnectAppResPacket* pkt = NULL;
    //          if ((ret = protocol->expect_message<SrsConnectAppResPacket>(protocol, &msg, &pkt)) != ERROR_SUCCESS) {
    //              return ret;
    //          }
    //          // Use then free msg and pkt
    //          srs_freep(msg);
    //          srs_freep(pkt);
    // the user should never recv a message and convert it, use this method instead.
    // if you need to set a timeout, use the set timeout of SrsProtocol.
    template <class T>
    srs_error_t expect_message(SrsCommonMessage **pmsg, T **ppacket)
    {
        *pmsg = NULL;
        *ppacket = NULL;

        srs_error_t err = srs_success;

        while (true)
        {
            SrsCommonMessage *msg = NULL;

            if ((err = recv_message(&msg)) != srs_success)
            {
                return srs_error_wrap(err, "recv message");
            }

            SrsPacket *packet = NULL;
            if ((err = decode_message(msg, &packet)) != srs_success)
            {
                srs_freep(msg);
                srs_freep(packet);
                return srs_error_wrap(err, "decode message");
            }

            T *pkt = dynamic_cast<T *>(packet);
            if (!pkt)
            {
                srs_freep(msg);
                srs_freep(packet);
                continue;
            }

            *pmsg = msg;
            *ppacket = pkt;
            break;
        }

        return err;
    }

private:
    // Send out the messages, do not free them,
    // The caller must free the param msgs.
    virtual srs_error_t do_send_messages(SrsSharedPtrMessage **msgs, int nb_msgs);
    // The underlying api to send and free a packet.
    virtual srs_error_t do_send_and_free_packet(SrsPacket *packet, int stream_id);
    // The imp for decode_message.
    // 컨트롤 메시지(1/3/4/5) + AMF0 커맨드 이름 → 패킷 클래스 디스패치.
    virtual srs_error_t do_decode_message(SrsMessageHeader &header, SrsBuffer *stream, SrsPacket **ppacket);
    // Recv a bytes oriented RTMP message from the protocol stack.
    // return an error if an error occurs and never set the pmsg,
    // return success and pmsg set to NULL if no entire message was got,
    // return success and pmsg set to entire message if got one.
    virtual srs_error_t recv_interlaced_message(SrsCommonMessage **pmsg);
    // Read the chunk basic header(fmt, cid) from the chunk stream.
    // the user can discover a SrsChunkStream by cid.
    virtual srs_error_t read_basic_header(char &fmt, int &cid);
    // Read the chunk message header(timestamp, payload_length, message_type, stream_id)
    // from the chunk stream and save to SrsChunkStream.
    virtual srs_error_t read_message_header(SrsChunkStream *chunk, char fmt);
    // Read the chunk payload, remove the used bytes in buffer,
    // if got entire message, set the pmsg.
    virtual srs_error_t read_message_payload(SrsChunkStream *chunk, SrsCommonMessage **pmsg);
    // When we recv a message, update the context.
    virtual srs_error_t on_recv_message(SrsCommonMessage *msg);
    // When a message is sent out, update the context.
    virtual srs_error_t on_send_packet(SrsMessageHeader *mh, SrsPacket *packet);

private:
    // Auto respond with the ack message.
    virtual srs_error_t response_acknowledgement_message();
    // Auto respond with the ping message.
    virtual srs_error_t response_ping_message(int32_t timestamp);

private:
    virtual void print_debug_info();
};

// incoming chunk streams may be interlaced,
// Use the chunk stream to cache the input RTMP chunk streams.
class SrsChunkStream
{
public:
    // Represents the basic header fmt,
    // which is used to identify the variant message header type.
    char fmt;
    // Represents the basic header cid,
    // which is the chunk stream id.
    int cid;
    // Cached message header
    SrsMessageHeader header;
    // Whether the chunk message header has extended timestamp.
    bool has_extended_timestamp;
    // The partially read message.
    SrsCommonMessage *msg;
    // Decoded msg count, to identify whether the chunk stream is fresh.
    int64_t msg_count;
    // Because the extended timestamp may be a delta timestamp, it can differ
    // from the timestamp in the header, so it should be stored as a distinct field
    // for comparison with the extended timestamp of subsequent chunks.
    // See https://github.com/ossrs/srs/pull/4356 for details.
    int32_t extended_timestamp;

public:
    SrsChunkStream(int _cid);
    virtual ~SrsChunkStream();
};

// The original request from the client.
// 원본 대비 제거: update_auth/as_http/protocol/ice_ufrag_/ice_pwd_ (RTC/HTTP 경로 전용) — CLAUDE.md §5.6.
class SrsRequest
{
public:
    // The client ip.
    std::string ip;

public:
    // Support pass vhost in RTMP URL, such as:
    //    rtmp://VHOST:port/app/stream
    //    rtmp://ip:port/app/stream?vhost=VHOST
    // While tcUrl is the url without the stream.
    std::string tcUrl;

public:
    std::string pageUrl;
    std::string swfUrl;
    double objectEncoding;
    // The data discovered from the request.
public:
    // Discovery from tcUrl and play/publish.
    std::string schema;
    // The vhost in tcUrl.
    std::string vhost;
    // The host in tcUrl.
    std::string host;
    // The port in tcUrl.
    int port;
    // The app in tcUrl, without param.
    std::string app;
    // The param in tcUrl(app).
    std::string param;
    // The stream in play/publish
    std::string stream;
    // For play live stream,
    // used to specify stopping when the duration is exceeded.
    // in srs_utime_t.
    srs_utime_t duration;
    // The token in the connect request,
    // used for edge traverse to origin authentication,
    // @see https://github.com/ossrs/srs/issues/104
    SrsAmf0Object *args;

public:
    SrsRequest();
    virtual ~SrsRequest();

public:
    // Deep copy the request, for the source to use it to support reload,
    // for when the source is initialized, the request is valid,
    // when it is reloaded, the request may be invalid, so we need to copy it.
    virtual SrsRequest *copy();
    // Get the stream identifier, vhost/app/stream.
    virtual std::string get_stream_url();
    // To strip the url, the user must strip when updating the url.
    virtual void strip();
};

// The response to the client.
class SrsResponse
{
public:
    // The stream id to respond to the client createStream.
    int stream_id;

public:
    SrsResponse();
    virtual ~SrsResponse();
};

// The rtmp client type.
// 원본의 HLS/FLV/RTC/SRT/Haivision 값은 해당 경로가 없으므로 제거 (이름/값은 원본과 동일).
enum SrsRtmpConnType
{
    SrsRtmpConnUnknown = 0x0000,
    // All players.
    SrsRtmpConnPlay = 0x0100,
    // All publishers.
    SrsRtmpConnFMLEPublish = 0x0200,
    SrsRtmpConnFlashPublish = 0x0201,
};
std::string srs_client_type_string(SrsRtmpConnType type);

// The rtmp provides rtmp-command-protocol services,
// a high level protocol with media stream oriented services,
// such as connect to vhost/app, play stream, get audio/video data.
// 원본 대비 제거: SrsRtmpClient(클라이언트 역할), proxy_real_ip, redirect, on_bw_done,
// haivision/flash publish, on_play_client_pause — CLAUDE.md §5.6 S6.
class SrsRtmpServer
{
private:
    SrsHandshakeBytes *hs_bytes;
    SrsProtocol *protocol;
    ISrsProtocolReadWriter *io;

public:
    SrsRtmpServer(ISrsProtocolReadWriter *skt);
    virtual ~SrsRtmpServer();
    // Protocol methods proxy
public:
    // To set/get the recv timeout in srs_utime_t.
    // if it times out, recv/send message returns ERROR_SOCKET_TIMEOUT.
    virtual void set_recv_timeout(srs_utime_t tm);
    virtual srs_utime_t get_recv_timeout();
    // To set/get the send timeout in srs_utime_t.
    // if it times out, recv/send message returns ERROR_SOCKET_TIMEOUT.
    virtual void set_send_timeout(srs_utime_t tm);
    virtual srs_utime_t get_send_timeout();
    // Get recv/send bytes.
    virtual int64_t get_recv_bytes();
    virtual int64_t get_send_bytes();
    // Recv an RTMP message, which is bytes oriented.
    // the user can use decode_message to get the decoded RTMP packet.
    virtual srs_error_t recv_message(SrsCommonMessage **pmsg);
    // Decode a bytes oriented RTMP message to an RTMP packet,
    virtual srs_error_t decode_message(SrsCommonMessage *msg, SrsPacket **ppacket);
    // Send the RTMP message and always free it.
    virtual srs_error_t send_and_free_message(SrsSharedPtrMessage *msg, int stream_id);
    // Send the RTMP message and always free it.
    virtual srs_error_t send_and_free_messages(SrsSharedPtrMessage **msgs, int nb_msgs, int stream_id);
    // Send the RTMP packet and always free it.
    virtual srs_error_t send_and_free_packet(SrsPacket *packet, int stream_id);

public:
    // Do handshake with client. (원본은 복잡→심플 폴백, 우리는 심플만 — CLAUDE.md §2.1)
    virtual srs_error_t handshake();
    // Do connect app with the client, to discover the tcUrl.
    virtual srs_error_t connect_app(SrsRequest *req);
    // Set the output ack size to the client, the client will send an ack for each ack window
    virtual srs_error_t set_window_ack_size(int ack_size);
    // Set the default input ack size value.
    virtual srs_error_t set_in_window_ack_size(int ack_size);
    // @type: The sender can mark this message hard (0), soft (1), or dynamic (2)
    // using the Limit type field.
    virtual srs_error_t set_peer_bandwidth(int bandwidth, int type);
    // @param server_ip the ip of the server.
    virtual srs_error_t response_connect_app(SrsRequest *req, const char *server_ip = NULL);
    // Recv some messages to identify the client.
    // @stream_id, the client will createStream to play or publish by flash,
    //         the stream_id is used to respond to the createStream request.
    // @type, output the client type.
    // @stream_name, output the client publish/play stream name. @see: SrsRequest.stream
    // @duration, output the play client duration. @see: SrsRequest.duration
    virtual srs_error_t identify_client(int stream_id, SrsRtmpConnType &type, std::string &stream_name, srs_utime_t &duration);
    // Set the chunk size when the client type is identified.
    virtual srs_error_t set_chunk_size(int chunk_size);
    // When the client type is play, respond with packets:
    // StreamBegin,
    // onStatus(NetStream.Play.Reset), onStatus(NetStream.Play.Start).,
    // |RtmpSampleAccess(false, false),
    // onStatus(NetStream.Data.Start).
    virtual srs_error_t start_play(int stream_id);
    // When the client type is publish, respond with packets:
    // releaseStream response
    // FCPublish
    // FCPublish response
    // createStream response
    // onFCPublish(NetStream.Publish.Start)
    // onStatus(NetStream.Publish.Start)
    virtual srs_error_t start_fmle_publish(int stream_id);
    // process the FMLE unpublish event.
    // @unpublish_tid the unpublish request transaction id.
    virtual srs_error_t fmle_unpublish(int stream_id, double unpublish_tid);
    // Respond with the start publishing message after the hooks are verified. To stop the reconnecting of
    // OBS when publish failed, we should never send the onStatus(NetStream.Publish.Start)
    // message before failure caused by hooks. See https://github.com/ossrs/srs/issues/4037
    virtual srs_error_t start_publishing(int stream_id);

public:
    // Expect a specified message, drop others until we get the specified one.
    // @remark, only on success can the user use and must free the pmsg and ppacket.
    template <class T>
    srs_error_t expect_message(SrsCommonMessage **pmsg, T **ppacket)
    {
        return protocol->expect_message<T>(pmsg, ppacket);
    }

private:
    virtual srs_error_t identify_create_stream_client(SrsCreateStreamPacket *req, int stream_id, int depth, SrsRtmpConnType &type, std::string &stream_name, srs_utime_t &duration);
    virtual srs_error_t identify_fmle_publish_client(SrsFMLEStartPacket *req, SrsRtmpConnType &type, std::string &stream_name);
    virtual srs_error_t identify_flash_publish_client(SrsPublishPacket *req, SrsRtmpConnType &type, std::string &stream_name);

private:
    virtual srs_error_t identify_play_client(SrsPlayPacket *req, SrsRtmpConnType &type, std::string &stream_name, srs_utime_t &duration);
};

// 4.1.1. connect
// The client sends the connect command to the server to request
// connection to a server application instance.
class SrsConnectAppPacket : public SrsPacket
{
public:
    // Name of the command. Set to "connect".
    std::string command_name;
    // Always set to 1.
    double transaction_id;
    // Command information object which has the name-value pairs.
    // @remark: allocated in the packet constructor, the user can directly use it,
    //       the user should never alloc it again which will cause a memory leak.
    // @remark, never NULL.
    SrsAmf0Object *command_object;
    // Any optional information
    // @remark, optional, initialized to NULL and may stay NULL.
    SrsAmf0Object *args;

public:
    SrsConnectAppPacket();
    virtual ~SrsConnectAppPacket();
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};
// Response for SrsConnectAppPacket.
class SrsConnectAppResPacket : public SrsPacket
{
public:
    // The _result or _error; indicates whether the response is result or error.
    std::string command_name;
    // Transaction ID is 1 for call connect responses
    double transaction_id;
    // Name-value pairs that describe the properties(fmsver etc.) of the connection.
    // @remark, never NULL.
    SrsAmf0Object *props;
    // Name-value pairs that describe the response from the server. 'code',
    // 'level', 'description' are names of few among such information.
    // @remark, never NULL.
    SrsAmf0Object *info;

public:
    SrsConnectAppResPacket();
    virtual ~SrsConnectAppResPacket();
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// 4.1.3. createStream
// The client sends this command to the server to create a logical
// channel for message communication. The publishing of audio, video, and
// metadata is carried out over stream channel created using the
// createStream command.
class SrsCreateStreamPacket : public SrsPacket
{
public:
    // Name of the command. Set to "createStream".
    std::string command_name;
    // Transaction ID of the command.
    double transaction_id;
    // If there exists any command info this is set, else this is set to null type.
    // @remark, never NULL, an AMF0 null instance.
    SrsAmf0Any *command_object; // null
public:
    SrsCreateStreamPacket();
    virtual ~SrsCreateStreamPacket();

public:
    void set_command_object(SrsAmf0Any *v);
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};
// Response for SrsCreateStreamPacket.
class SrsCreateStreamResPacket : public SrsPacket
{
public:
    // The _result or _error; indicates whether the response is result or error.
    std::string command_name;
    // ID of the command that the response belongs to.
    double transaction_id;
    // If there exists any command info this is set, else this is set to null type.
    // @remark, never NULL, an AMF0 null instance.
    SrsAmf0Any *command_object; // null
    // The return value is either a stream ID or an error information object.
    double stream_id;

public:
    SrsCreateStreamResPacket(double _transaction_id, double _stream_id);
    virtual ~SrsCreateStreamResPacket();
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// FMLE start publish: ReleaseStream/PublishStream/FCPublish/FCUnpublish
class SrsFMLEStartPacket : public SrsPacket
{
public:
    // Name of the command
    std::string command_name;
    // The transaction ID to get the response.
    double transaction_id;
    // If there exists any command info this is set, else this is set to null type.
    // @remark, never NULL, an AMF0 null instance.
    SrsAmf0Any *command_object; // null
    // The stream name to start publishing or to release.
    std::string stream_name;

public:
    SrsFMLEStartPacket();
    virtual ~SrsFMLEStartPacket();

public:
    void set_command_object(SrsAmf0Any *v);
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
    // Factory method to create the specified FMLE packet.
public:
    static SrsFMLEStartPacket *create_release_stream(std::string stream);
    static SrsFMLEStartPacket *create_FC_publish(std::string stream);
};
// Response for SrsFMLEStartPacket.
class SrsFMLEStartResPacket : public SrsPacket
{
public:
    // Name of the command
    std::string command_name;
    // The transaction ID to get the response.
    double transaction_id;
    // If there exists any command info this is set, else this is set to null type.
    // @remark, never NULL, an AMF0 null instance.
    SrsAmf0Any *command_object; // null
    // The optional args, set to undefined.
    // @remark, never NULL, an AMF0 undefined instance.
    SrsAmf0Any *args; // undefined
public:
    SrsFMLEStartResPacket(double _transaction_id);
    virtual ~SrsFMLEStartResPacket();

public:
    void set_args(SrsAmf0Any *v);
    void set_command_object(SrsAmf0Any *v);
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// FMLE/flash publish
// 4.2.6. Publish
// The client sends the publish command to publish a named stream to the
// server. Using this name, any client can play this stream and receive
// the published audio, video, and data messages.
class SrsPublishPacket : public SrsPacket
{
public:
    // Name of the command, set to "publish".
    std::string command_name;
    // Transaction ID set to 0.
    double transaction_id;
    // Command information object does not exist. Set to null type.
    // @remark, never NULL, an AMF0 null instance.
    SrsAmf0Any *command_object; // null
    // Name with which the stream is published.
    std::string stream_name;
    // Type of publishing. Set to "live", "record", or "append".
    // @remark, SRS only supports live.
    // @remark, optional, default to live.
    std::string type;

public:
    SrsPublishPacket();
    virtual ~SrsPublishPacket();

public:
    void set_command_object(SrsAmf0Any *v);
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// 4.2.1. play
// The client sends this command to the server to play a stream.
class SrsPlayPacket : public SrsPacket
{
public:
    // Name of the command. Set to "play".
    std::string command_name;
    // Transaction ID set to 0.
    double transaction_id;
    // Command information does not exist. Set to null type.
    // @remark, never NULL, an AMF0 null instance.
    SrsAmf0Any *command_object; // null
    // Name of the stream to play.
    std::string stream_name;
    // An optional parameter that specifies the start time in seconds. Default -2 (live).
    double start;
    // An optional parameter that specifies the duration of playback in seconds. Default -1.
    double duration;
    // An optional Boolean value or number that specifies whether to flush any
    // previous playlist.
    bool reset;

public:
    SrsPlayPacket();
    virtual ~SrsPlayPacket();
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// onStatus command, AMF0 Call
// @remark, the user must set the stream_id by send_and_free_packet(pkt, stream_id).
class SrsOnStatusCallPacket : public SrsPacket
{
public:
    // Name of command. Set to "onStatus"
    std::string command_name;
    // Transaction ID set to 0.
    double transaction_id;
    // Command information does not exist. Set to null type.
    // @remark, never NULL, an AMF0 null instance.
    SrsAmf0Any *args; // null
    // Name-value pairs that describe the response from the server.
    // 'code','level', 'description' are names of few among such information.
    // @remark, never NULL, an AMF0 object instance.
    SrsAmf0Object *data;

public:
    SrsOnStatusCallPacket();
    virtual ~SrsOnStatusCallPacket();

public:
    void set_args(SrsAmf0Any *v);
    void set_data(SrsAmf0Object *v);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// AMF0Data RtmpSampleAccess
// @remark, the user must set the stream_id by send_and_free_packet(pkt, stream_id).
class SrsSampleAccessPacket : public SrsPacket
{
public:
    // Name of command. Set to "|RtmpSampleAccess".
    std::string command_name;
    // Whether to allow access to the video sample.
    bool video_sample_access;
    // Whether to allow access to the audio sample.
    bool audio_sample_access;

public:
    SrsSampleAccessPacket();
    virtual ~SrsSampleAccessPacket();
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// The stream metadata.
// FMLE: @setDataFrame
// others: onMetaData
class SrsOnMetaDataPacket : public SrsPacket
{
public:
    // Name of metadata. Set to "onMetaData"
    std::string name;
    // Metadata of the stream.
    // @remark, never NULL, an AMF0 object instance.
    SrsAmf0Object *metadata;

public:
    SrsOnMetaDataPacket();
    virtual ~SrsOnMetaDataPacket();

public:
    void set_metadata(SrsAmf0Object *v);
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// 5.5. Window Acknowledgement Size (5)
// The client or the server sends this message to inform the peer which
// window size to use when sending acknowledgment.
class SrsSetWindowAckSizePacket : public SrsPacket
{
public:
    int32_t ackowledgement_window_size;

public:
    SrsSetWindowAckSizePacket();
    virtual ~SrsSetWindowAckSizePacket();
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// 5.3. Acknowledgement (3)
// The client or the server sends the acknowledgment to the peer after
// receiving bytes equal to the window size.
class SrsAcknowledgementPacket : public SrsPacket
{
public:
    uint32_t sequence_number;

public:
    SrsAcknowledgementPacket();
    virtual ~SrsAcknowledgementPacket();
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// 7.1. Set Chunk Size
// Protocol control message 1, Set Chunk Size, is used to notify the
// peer about the new maximum chunk size.
class SrsSetChunkSizePacket : public SrsPacket
{
public:
    // The maximum chunk size can be 65536 bytes. The chunk size is
    // maintained independently for each direction.
    int32_t chunk_size;

public:
    SrsSetChunkSizePacket();
    virtual ~SrsSetChunkSizePacket();
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// 5.6. Set Peer Bandwidth (6)
enum SrsPeerBandwidthType
{
    // The sender can mark this message hard (0), soft (1), or dynamic (2)
    // using the Limit type field.
    SrsPeerBandwidthHard = 0,
    SrsPeerBandwidthSoft = 1,
    SrsPeerBandwidthDynamic = 2,
};

// 5.6. Set Peer Bandwidth (6)
// The client or the server sends this message to update the output
// bandwidth of the peer.
class SrsSetPeerBandwidthPacket : public SrsPacket
{
public:
    int32_t bandwidth;
    // @see: SrsPeerBandwidthType
    int8_t type;

public:
    SrsSetPeerBandwidthPacket();
    virtual ~SrsSetPeerBandwidthPacket();
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

// 3.7. User Control message
enum SrcPCUCEventType
{
    // Generally, 4bytes event-data

    // The server sends this event to notify the client
    // that a stream has become functional and can be
    // used for communication. By default, this event
    // is sent on ID 0 after the application connect
    // command is successfully received from the
    // client. The event data is 4-byte and represents
    // The stream ID of the stream that became
    // Functional.
    SrcPCUCStreamBegin = 0x00,

    // The server sends this event to notify the client
    // that the playback of data is over as requested
    // on this stream. No more data is sent without
    // issuing additional commands. The client discards
    // The messages received for the stream. The
    // 4 bytes of event data represent the ID of the
    // stream on which playback has ended.
    SrcPCUCStreamEOF = 0x01,

    // The server sends this event to notify the client
    // that there is no more data on the stream. If the
    // server does not detect any message for a time
    // period, it can notify the subscribed clients
    // that the stream is dry. The 4 bytes of event
    // data represent the stream ID of the dry stream.
    SrcPCUCStreamDry = 0x02,

    // The client sends this event to inform the server
    // of the buffer size (in milliseconds) that is
    // used to buffer any data coming over a stream.
    // This event is sent before the server starts
    // processing the stream. The first 4 bytes of the
    // event data represent the stream ID and the next
    // 4 bytes represent the buffer length, in
    // milliseconds.
    SrcPCUCSetBufferLength = 0x03, // 8bytes event-data

    // The server sends this event to notify the client
    // that the stream is a recorded stream. The
    // 4 bytes event data represent the stream ID of
    // The recorded stream.
    SrcPCUCStreamIsRecorded = 0x04,

    // The server sends this event to test whether the
    // client is reachable. Event data is a 4-byte
    // timestamp, representing the local server time
    // When the server dispatched the command. The
    // client responds with kMsgPingResponse on
    // receiving kMsgPingRequest.
    SrcPCUCPingRequest = 0x06,

    // The client sends this event to the server in
    // Response to the ping request. The event data is
    // a 4-byte timestamp, which was received with the
    // kMsgPingRequest request.
    SrcPCUCPingResponse = 0x07,

    // For PCUC size=3, for example the payload is "00 1A 01",
    // it's a FMS control event, where the event type is 0x001a and event data is 0x01,
    // please notice that the event data is only 1 byte for this event.
    SrsPCUCFmsEvent0 = 0x1a,
};

// 5.4. User Control Message (4)
//
// Here the EventData is 4bytes.
// Stream Begin(=0)              4-bytes stream ID
// Stream EOF(=1)                4-bytes stream ID
// StreamDry(=2)                 4-bytes stream ID
// SetBufferLength(=3)           8-bytes 4bytes stream ID, 4bytes buffer length.
// StreamIsRecorded(=4)          4-bytes stream ID
// PingRequest(=6)               4-bytes timestamp local server time
// PingResponse(=7)              4-bytes timestamp received ping request.
//
// 3.7. User Control message
// +------------------------------+-------------------------
// | Event Type ( 2- bytes ) | Event Data
// +------------------------------+-------------------------
// Figure 5 Pay load for the 'User Control Message'.
class SrsUserControlPacket : public SrsPacket
{
public:
    // Event type is followed by Event data.
    // @see: SrcPCUCEventType
    int16_t event_type;
    // The event data is generally 4bytes.
    // @remark for event type 0x001a, only 1 byte.
    // @see SrsPCUCFmsEvent0
    int32_t event_data;
    // 4bytes if event_type is SetBufferLength; otherwise 0.
    int32_t extra_data;

public:
    SrsUserControlPacket();
    virtual ~SrsUserControlPacket();
    // Decode functions for concrete packet to override.
public:
    virtual srs_error_t decode(SrsBuffer *stream);
    // Encode functions for concrete packet to override.
public:
    virtual int get_prefer_cid();
    virtual int get_message_type();

protected:
    virtual int get_size();
    virtual srs_error_t encode_packet(SrsBuffer *stream);
};

#endif
