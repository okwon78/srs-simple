// srs_simple — 원본: trunk/src/app/srs_app_rtmp_conn.cpp
// do_cycle:173, service_cycle:396, stream_service_cycle:491, publishing:925, playing:702.
#include <srs_app_rtmp_conn.hpp>

#include <inttypes.h>
#include <sys/socket.h>
#include <time.h>

#include <poll.h>

#include <srs_app_config.hpp>
#include <srs_app_server.hpp>
#include <srs_app_source.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_flv.hpp>
#include <srs_kernel_log.hpp>
#include <srs_protocol_rtmp_msg_array.hpp>
#include <srs_protocol_utility.hpp>

using namespace std;

// The timeout in srs_utime_t to wait for the encoder to republish
// if timeout, close the connection. (원본: srs_app_rtmp_conn.cpp:46)
#define SRS_REPUBLISH_SEND_TIMEOUT (3 * SRS_UTIME_MINUTES)
#define SRS_REPUBLISH_RECV_TIMEOUT (3 * SRS_UTIME_MINUTES)

// The timeout in srs_utime_t to wait client data, when client paused. (원본: :52)
#define SRS_PAUSED_SEND_TIMEOUT (3 * SRS_UTIME_MINUTES)
#define SRS_PAUSED_RECV_TIMEOUT (3 * SRS_UTIME_MINUTES)

// The RTMP recv/send timeout (원본: kernel/srs_kernel_consts.hpp:60)
#define SRS_CONSTS_RTMP_TIMEOUT (30 * SRS_UTIME_SECONDS)

// publishing/playing 상태 로그 간격 (원본 SrsPithyPrint의 축소판).
#define SRS_PUBLISH_REPORT_INTERVAL_SECONDS 5

// The merged-write duration to wait in consumer (원본: core/srs_core_performance.hpp:78).
#define SRS_PERF_MW_SLEEP (350 * SRS_UTIME_MILLISECONDS)

SrsClientInfo::SrsClientInfo()
{
    type = SrsRtmpConnUnknown;
    req = new SrsRequest();
    res = new SrsResponse();
}

SrsClientInfo::~SrsClientInfo()
{
    srs_freep(req);
    srs_freep(res);
}

SrsRtmpConn::SrsRtmpConn(SrsServer* svr, srs_netfd_t c, string cip, int cport)
{
    server = svr;
    stfd = c;
    skt = new SrsStSocket(c);
    // 원본과 동일 — server가 ISrsResourceManager를 구현한다.
    manager = svr;
    ip = cip;
    port = cport;

    trd = new SrsSTCoroutine("rtmp", this);
    rtmp = new SrsRtmpServer(skt);
    info = new SrsClientInfo();
}

SrsRtmpConn::~SrsRtmpConn()
{
    trd->interrupt();
    // 블록된 recv/send를 깨운다 — pthread 모델에서는 interrupt가 블록된 IO를
    // 못 깨우므로 shutdown으로 대신한다. join 지연 방지 (CLAUDE.md §5.1).
    if (stfd != SRS_NETFD_INVALID) {
        ::shutdown(stfd, SHUT_RDWR);
    }
    srs_freep(trd);

    srs_freep(info);
    srs_freep(rtmp);
    srs_freep(skt);
    srs_close_stfd(stfd);
}

std::string SrsRtmpConn::desc()
{
    return "RtmpConn";
}

srs_error_t SrsRtmpConn::do_cycle()
{
    srs_error_t err = srs_success;

    srs_trace("RTMP client ip=%s:%d, fd=%d", ip.c_str(), port, srs_netfd_fileno(stfd));

    rtmp->set_recv_timeout(SRS_CONSTS_RTMP_TIMEOUT);
    rtmp->set_send_timeout(SRS_CONSTS_RTMP_TIMEOUT);

    if ((err = rtmp->handshake()) != srs_success) {
        return srs_error_wrap(err, "rtmp handshake");
    }

    SrsRequest* req = info->req;
    if ((err = rtmp->connect_app(req)) != srs_success) {
        return srs_error_wrap(err, "rtmp connect tcUrl");
    }

    // set the client ip to the request.
    req->ip = ip;

    srs_trace("connect app, tcUrl=%s, pageUrl=%s, swfUrl=%s, schema=%s, vhost=%s, port=%d, app=%s, args=%s",
        req->tcUrl.c_str(), req->pageUrl.c_str(), req->swfUrl.c_str(),
        req->schema.c_str(), req->vhost.c_str(), req->port,
        req->app.c_str(), (req->args? "(obj)":"null"));

    if ((err = service_cycle()) != srs_success) {
        err = srs_error_wrap(err, "service cycle");
    }

    return err;
}

srs_error_t SrsRtmpConn::service_cycle()
{
    srs_error_t err = srs_success;

    SrsRequest* req = info->req;

    if ((err = rtmp->set_window_ack_size(_srs_config->window_ack_size)) != srs_success) {
        return srs_error_wrap(err, "rtmp: set out window ack size");
    }

    if ((err = rtmp->set_peer_bandwidth(_srs_config->peer_bandwidth, SrsPeerBandwidthDynamic)) != srs_success) {
        return srs_error_wrap(err, "rtmp: set peer bandwidth");
    }

    // get the ip which the client connected to.
    std::string local_ip = srs_get_local_ip(srs_netfd_fileno(stfd));

    // set the chunk size to a larger value.
    // set the chunk size before any larger response greater than 128,
    // to make OBS happy, @see https://github.com/ossrs/srs/issues/454
    if ((err = rtmp->set_chunk_size(_srs_config->chunk_size)) != srs_success) {
        return srs_error_wrap(err, "rtmp: set chunk size %d", _srs_config->chunk_size);
    }

    // respond to the client that connect is ok.
    if ((err = rtmp->response_connect_app(req, local_ip.c_str())) != srs_success) {
        return srs_error_wrap(err, "rtmp: response connect app");
    }

    while (true) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "rtmp: thread quit");
        }

        err = stream_service_cycle();

        // the stream service must terminate with an error, never with success.
        // when it terminates with success, it means the user asked to stop.
        if (err == srs_success) {
            continue;
        }

        // when it is not a system control error, it's a fatal error, return.
        if (!srs_is_system_control_error(err)) {
            return srs_error_wrap(err, "rtmp: stream service");
        }

        // for republish, continue service
        if (srs_error_code(err) == ERROR_CONTROL_REPUBLISH) {
            // set the timeout to a larger value, wait for the encoder to republish.
            rtmp->set_send_timeout(SRS_REPUBLISH_RECV_TIMEOUT);
            rtmp->set_recv_timeout(SRS_REPUBLISH_SEND_TIMEOUT);

            srs_trace("rtmp: retry for republish");
            srs_freep(err);
            continue;
        }

        // for "some" system control errors,
        // logically accept and retry the stream service.
        if (srs_error_code(err) == ERROR_CONTROL_RTMP_CLOSE) {
            // set the timeout to a larger value, for the user paused.
            rtmp->set_recv_timeout(SRS_PAUSED_RECV_TIMEOUT);
            rtmp->set_send_timeout(SRS_PAUSED_SEND_TIMEOUT);

            srs_trace("rtmp: retry for close");
            srs_freep(err);
            continue;
        }

        // for other system control messages, it's a fatal error.
        return srs_error_wrap(err, "rtmp: reject");
    }

    return err;
}

srs_error_t SrsRtmpConn::stream_service_cycle()
{
    srs_error_t err = srs_success;

    SrsRequest* req = info->req;
    if ((err = rtmp->identify_client(info->res->stream_id, info->type, req->stream, req->duration)) != srs_success) {
        return srs_error_wrap(err, "rtmp: identify client");
    }

    // 재파싱으로 stream에 붙은 ?param을 분리한다 (S6 utest DiscoveryTcUrl 케이스 3).
    srs_discovery_tc_url(req->tcUrl, req->schema, req->host, req->vhost, req->app, req->stream, req->port, req->param);
    req->strip();

    srs_trace("client identified, type=%s, vhost=%s, app=%s, stream=%s, param=%s, duration=%dms",
        srs_client_type_string(info->type).c_str(), req->vhost.c_str(), req->app.c_str(),
        req->stream.c_str(), req->param.c_str(), srsu2msi(req->duration));

    if (req->schema.empty() || req->vhost.empty() || req->port == 0 || req->app.empty()) {
        return srs_error_new(ERROR_RTMP_REQ_TCURL, "discovery tcUrl failed, tcUrl=%s, schema=%s, vhost=%s, port=%d, app=%s",
            req->tcUrl.c_str(), req->schema.c_str(), req->vhost.c_str(), req->port, req->app.c_str());
    }

    // Never allow the empty stream name.
    if (req->stream.empty()) {
        return srs_error_new(ERROR_RTMP_STREAM_NAME_EMPTY, "rtmp: empty stream");
    }

    // the client is identified, set the timeout to the service timeout.
    rtmp->set_recv_timeout(SRS_CONSTS_RTMP_TIMEOUT);
    rtmp->set_send_timeout(SRS_CONSTS_RTMP_TIMEOUT);

    // find a source to serve.
    SrsLiveSource* source = NULL;
    if ((err = _srs_sources->fetch_or_create(req, &source)) != srs_success) {
        return srs_error_wrap(err, "rtmp: fetch source");
    }
    srs_assert(source != NULL);

    srs_trace("source url=%s, ip=%s, cache=%d/%d, source_id=%s/%s",
        req->get_stream_url().c_str(), ip.c_str(), _srs_config->gop_cache,
        _srs_config->gop_cache_max_frames, source->source_id().c_str(), source->pre_source_id().c_str());
    source->set_cache(_srs_config->gop_cache);
    source->set_gop_cache_max_frames(_srs_config->gop_cache_max_frames);

    switch (info->type) {
        case SrsRtmpConnPlay: {
            // respond to the client that play starts
            if ((err = rtmp->start_play(info->res->stream_id)) != srs_success) {
                return srs_error_wrap(err, "rtmp: start play");
            }

            return playing(source);
        }
        case SrsRtmpConnFMLEPublish: {
            if ((err = rtmp->start_fmle_publish(info->res->stream_id)) != srs_success) {
                return srs_error_wrap(err, "rtmp: start FMLE publish");
            }

            return publishing(source);
        }
        case SrsRtmpConnFlashPublish: {
            // 원본은 start_flash_publish로 onStatus만 먼저 보낸다 — 우리는 publishing 안의
            // start_publishing이 같은 onStatus(Publish.Start)를 보낸다 (S6 축소, CLAUDE.md §5.6).
            return publishing(source);
        }
        default: {
            return srs_error_new(ERROR_SYSTEM_CLIENT_INVALID, "rtmp: unknown client type=%d", info->type);
        }
    }

    return err;
}

srs_error_t SrsRtmpConn::playing(SrsLiveSource* source)
{
    srs_error_t err = srs_success;

    // Create a consumer of the source.
    SrsLiveConsumer* consumer = NULL;
    if ((err = source->create_consumer(consumer)) != srs_success) {
        return srs_error_wrap(err, "rtmp: create consumer");
    }

    // ★ 송신 루프 시작 전에 반드시 실행 — onMetaData→AAC sh→AVC sh(SPS/PPS)→GOP 순서로
    // 재생 큐를 프리필한다. 순서가 바뀌면 첫 바이트가 SPS/PPS 없는 GOP 중간이 됨 (CLAUDE.md §5.3).
    if ((err = source->consumer_dumps(consumer)) != srs_success) {
        srs_freep(consumer);
        return srs_error_wrap(err, "rtmp: dumps consumer");
    }

    // Deliver packets to the peer.
    // 원본은 수신 전용 코루틴(SrsQueueRecvThread)을 붙이지만, 연결당 1스레드로
    // do_playing이 idle에 소켓을 직접 확인한다 (CLAUDE.md §5.1).
    err = do_playing(source, consumer);

    // ~SrsLiveConsumer가 source->on_consumer_destroy로 팬아웃 대상에서 빠진다.
    srs_freep(consumer);

    return err;
}

srs_error_t SrsRtmpConn::do_playing(SrsLiveSource* source, SrsLiveConsumer* consumer)
{
    srs_error_t err = srs_success;

    SrsRequest* req = info->req;
    srs_assert(req);
    srs_assert(consumer);

    SrsMessageArray msgs(_srs_config->mw_msgs);

    uint64_t nb_msgs_sent = 0;
    time_t last_report = ::time(NULL);

    while (true) {
        // when the source is set to expired, disconnect it.
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "rtmp: thread quit");
        }

        // 원본은 수신 전용 코루틴이 컨트롤 메시지/연결 종료를 감지한다(#217).
        // 여기서는 0ms poll로 소켓이 읽을 수 있을 때만 recv — 플레이어 종료(FIN)도
        // 이 경로의 recv가 감지한다 (gracefully close).
        struct pollfd pfd;
        pfd.fd = stfd;
        pfd.events = POLLIN;
        while (::poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            SrsCommonMessage* msg = NULL;
            if ((err = rtmp->recv_message(&msg)) != srs_success) {
                return srs_error_wrap(err, "rtmp: recv message");
            }

            err = process_play_control_msg(msg);
            srs_freep(msg);
            if (err != srs_success) {
                return srs_error_wrap(err, "rtmp: play control message");
            }
            pfd.revents = 0;
        }

        // wait for messages to come in.
        // @see https://github.com/ossrs/srs/issues/257
        consumer->wait(_srs_config->mw_msgs, SRS_PERF_MW_SLEEP);

        // get messages from the consumer.
        // each msg in msgs.msgs must be freed, for the SrsMessageArray never frees them.
        int count = 0;
        if ((err = consumer->dump_packets(&msgs, count)) != srs_success) {
            return srs_error_wrap(err, "rtmp: consumer dump packets");
        }

        // ignore when we got nothing.
        if (count <= 0) {
            continue;
        }
        nb_msgs_sent += count;

        // send out the messages, all messages are freed by send_and_free_messages().
        // no need to assert msg, for the rtmp stack will assert it.
        if ((err = rtmp->send_and_free_messages(msgs.msgs, count, info->res->stream_id)) != srs_success) {
            return srs_error_wrap(err, "rtmp: send %d messages", count);
        }

        // 원본 SrsPithyPrint의 축소판 — 주기적으로 송신 카운트를 로그.
        time_t now = ::time(NULL);
        if (now - last_report >= SRS_PUBLISH_REPORT_INTERVAL_SECONDS) {
            last_report = now;
            srs_trace("-> play url=%s, msgs=%" PRIu64 ", send=%" PRId64 "KB",
                req->get_stream_url().c_str(), nb_msgs_sent, rtmp->get_send_bytes() / 1024);
        }
    }

    return err;
}

srs_error_t SrsRtmpConn::process_play_control_msg(SrsCommonMessage* msg)
{
    srs_error_t err = srs_success;

    if (!msg) {
        return err;
    }

    if (!msg->header.is_amf0_command()) {
        return err;
    }

    SrsPacket* pkt = NULL;
    if ((err = rtmp->decode_message(msg, &pkt)) != srs_success) {
        return srs_error_wrap(err, "rtmp: decode message");
    }

    // 원본은 closeStream→ERROR_CONTROL_RTMP_CLOSE, Call→null 응답, pause를 처리한다.
    // 해당 패킷들은 S6에서 제거 — 미지의 커맨드는 기본 SrsPacket으로 드롭된다 (CLAUDE.md §5.6).
    srs_info("play: ignore AMF0 command message");
    srs_freep(pkt);

    return err;
}

srs_error_t SrsRtmpConn::publishing(SrsLiveSource* source)
{
    srs_error_t err = srs_success;

    // 인가(acquire) 성공 후에만 do_publishing을 돈다.
    srs_error_t acquire_err = acquire_publish(source);
    if ((err = acquire_err) == srs_success) {
        err = do_publishing(source);
    }

    // Release when acquiring publishing succeeded, if not, we should ignore it,
    // because the source is not published by this session.
    if (acquire_err == srs_success) {
        release_publish(source);
    }

    return err;
}

srs_error_t SrsRtmpConn::acquire_publish(SrsLiveSource* source)
{
    srs_error_t err = srs_success;

    SrsRequest* req = info->req;

    // Check whether the RTMP stream is busy.
    // (검사+점유의 원자성은 on_publish 내부에서 보장한다 — CLAUDE.md §5.6)
    if (!source->can_publish()) {
        return srs_error_new(ERROR_SYSTEM_STREAM_BUSY, "rtmp: stream %s is busy", req->get_stream_url().c_str());
    }

    // Notify the source publish event.
    if ((err = source->on_publish()) != srs_success) {
        return srs_error_wrap(err, "rtmp: source publish");
    }

    return err;
}

void SrsRtmpConn::release_publish(SrsLiveSource* source)
{
    // when edge, notify the edge to change state.
    // when origin, notify all services to unpublish.
    source->on_unpublish();
}

srs_error_t SrsRtmpConn::do_publishing(SrsLiveSource* source)
{
    srs_error_t err = srs_success;

    // Respond with the start publishing message, let the client start to publish messages.
    // 인가(acquire) 통과 후에만 보낸다 — OBS 재연결 루프 방지 (#4037, CLAUDE.md §5.3).
    if ((err = rtmp->start_publishing(info->res->stream_id)) != srs_success) {
        return srs_error_wrap(err, "start publishing");
    }

    // 원본은 수신 전용 코루틴(SrsPublishRecvThread)과 cond wait로 타임아웃을 재지만,
    // 연결당 1스레드에서는 소켓 타임아웃으로 직접 recv한다 (CLAUDE.md §5.1).
    time_t last_report = ::time(NULL);
    while (true) {
        if ((err = trd->pull()) != srs_success) {
            return srs_error_wrap(err, "rtmp: thread quit");
        }

        SrsCommonMessage* msg = NULL;
        if ((err = rtmp->recv_message(&msg)) != srs_success) {
            return srs_error_wrap(err, "rtmp: recv message");
        }

        err = handle_publish_message(source, msg);
        srs_freep(msg);
        if (err != srs_success) {
            return srs_error_wrap(err, "rtmp: publish message");
        }

        // 원본 SrsPithyPrint의 축소판 — 주기적으로 수신 카운트를 로그.
        time_t now = ::time(NULL);
        if (now - last_report >= SRS_PUBLISH_REPORT_INTERVAL_SECONDS) {
            last_report = now;
            srs_trace("<- publish url=%s, recv=%" PRId64 "KB",
                info->req->get_stream_url().c_str(), rtmp->get_recv_bytes() / 1024);
        }
    }

    return err;
}

srs_error_t SrsRtmpConn::handle_publish_message(SrsLiveSource* source, SrsCommonMessage* msg)
{
    srs_error_t err = srs_success;

    // process publish event.
    if (msg->header.is_amf0_command()) {
        SrsPacket* pkt = NULL;
        if ((err = rtmp->decode_message(msg, &pkt)) != srs_success) {
            return srs_error_wrap(err, "rtmp: decode message");
        }

        // for fmle, drop others except the fmle start packet.
        // FCUnpublish 수신 → 응답 3종 송신 후 재-publish 루프로 (service_cycle이 받는다).
        SrsFMLEStartPacket* unpublish = dynamic_cast<SrsFMLEStartPacket*>(pkt);
        if (unpublish) {
            double tid = unpublish->transaction_id;
            srs_freep(pkt);
            if ((err = rtmp->fmle_unpublish(info->res->stream_id, tid)) != srs_success) {
                return srs_error_wrap(err, "rtmp: republish");
            }
            return srs_error_new(ERROR_CONTROL_REPUBLISH, "rtmp: republish");
        }

        srs_trace("fmle ignore AMF0 command message.");
        srs_freep(pkt);
        return err;
    }

    // video, audio, data message
    if ((err = process_publish_message(source, msg)) != srs_success) {
        return srs_error_wrap(err, "rtmp: consume message");
    }

    return err;
}

srs_error_t SrsRtmpConn::process_publish_message(SrsLiveSource* source, SrsCommonMessage* msg)
{
    srs_error_t err = srs_success;

    // process audio packet
    if (msg->header.is_audio()) {
        if ((err = source->on_audio(msg)) != srs_success) {
            return srs_error_wrap(err, "rtmp: consume audio");
        }
        return err;
    }
    // process video packet
    if (msg->header.is_video()) {
        if ((err = source->on_video(msg)) != srs_success) {
            return srs_error_wrap(err, "rtmp: consume video");
        }
        return err;
    }

    // process onMetaData
    if (msg->header.is_amf0_data()) {
        SrsPacket* pkt = NULL;
        if ((err = rtmp->decode_message(msg, &pkt)) != srs_success) {
            return srs_error_wrap(err, "rtmp: decode message");
        }

        SrsOnMetaDataPacket* metadata = dynamic_cast<SrsOnMetaDataPacket*>(pkt);
        if (metadata) {
            err = source->on_meta_data(msg, metadata);
            srs_freep(pkt);
            if (err != srs_success) {
                return srs_error_wrap(err, "rtmp: consume metadata");
            }
            return err;
        }
        srs_freep(pkt);
        return err;
    }

    return err;
}

srs_error_t SrsRtmpConn::start()
{
    srs_error_t err = srs_success;

    if ((err = trd->start()) != srs_success) {
        return srs_error_wrap(err, "coroutine");
    }

    return err;
}

srs_error_t SrsRtmpConn::cycle()
{
    srs_error_t err = srs_success;

    // Serve the client.
    err = do_cycle();

    // Notify the manager to remove it.
    // 자기 스레드에서 delete this 금지 — 매니저가 다른 스레드에서 해제한다 (CLAUDE.md §5.3).
    // Note that we create this object, so we use the manager to remove it.
    manager->remove(this);

    // success.
    if (err == srs_success) {
        srs_trace("client finished.");
        return err;
    }

    // the client closed the connection.
    if (srs_is_client_gracefully_close(err)) {
        srs_warn("client disconnect peer. ret=%d", srs_error_code(err));
    } else {
        srs_error("serve error %s", srs_error_desc(err).c_str());
    }

    srs_freep(err);
    return srs_success;
}

string SrsRtmpConn::remote_ip()
{
    return ip;
}

const SrsContextId& SrsRtmpConn::get_id()
{
    return trd->cid();
}
