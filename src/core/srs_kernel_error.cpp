// srs_simple — 원본: trunk/src/kernel/srs_kernel_error.cpp
#include <srs_kernel_error.hpp>

#include <srs_kernel_log.hpp>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include <map>
#include <sstream>
using namespace std;

// 원본은 4MB static 버퍼를 재사용하지만 pthread 모델에서는 데이터 레이스이므로
// 스택 버퍼를 쓴다 (CLAUDE.md §5.6).
const int maxErrorBuf = 4096;

bool srs_is_system_control_error(srs_error_t err)
{
    int error_code = srs_error_code(err);
    return error_code == ERROR_CONTROL_RTMP_CLOSE
        || error_code == ERROR_CONTROL_REPUBLISH;
}

bool srs_is_client_gracefully_close(srs_error_t err)
{
    int error_code = srs_error_code(err);
    return error_code == ERROR_SOCKET_READ
        || error_code == ERROR_SOCKET_READ_FULLY
        || error_code == ERROR_SOCKET_WRITE;
}

SrsCplxError::SrsCplxError()
{
    code = ERROR_SUCCESS;
    wrapped = NULL;
    rerrno = line = 0;
}

SrsCplxError::~SrsCplxError()
{
    srs_freep(wrapped);
}

std::string SrsCplxError::description() {
    if (desc.empty()) {
        stringstream ss;
        ss << "code=" << code;

        string code_str = srs_error_code_str(this);
        if (!code_str.empty()) ss << "(" << code_str << ")";

        SrsCplxError* next = this;
        while (next) {
            ss << " : " << next->msg;
            next = next->wrapped;
        }
        ss << endl;

        next = this;
        while (next) {
            ss << "thread [" << getpid() << "][" << next->cid.c_str() << "]: "
            << next->func << "() [" << next->file << ":" << next->line << "]"
            << "[errno=" << next->rerrno << "]";

            next = next->wrapped;

            if (next) {
                ss << endl;
            }
        }

        desc = ss.str();
    }

    return desc;
}

std::string SrsCplxError::summary() {
    if (_summary.empty()) {
        stringstream ss;

        ss << "code=" << code;

        string code_str = srs_error_code_str(this);
        if (!code_str.empty()) ss << "(" << code_str << ")";

        SrsCplxError* next = this;
        while (next) {
            ss << " : " << next->msg;
            next = next->wrapped;
        }

        _summary = ss.str();
    }

    return _summary;
}

SrsCplxError* SrsCplxError::create(const char* func, const char* file, int line, int code, const char* fmt, ...) {
    int rerrno = (int)errno;

    char buffer[maxErrorBuf];
    va_list ap;
    va_start(ap, fmt);
    int r0 = vsnprintf(buffer, maxErrorBuf, fmt, ap);
    va_end(ap);

    SrsCplxError* err = new SrsCplxError();

    err->func = func;
    err->file = file;
    err->line = line;
    err->code = code;
    err->rerrno = rerrno;
    if (r0 > 0 && r0 < maxErrorBuf) {
        err->msg = string(buffer, r0);
    }
    err->wrapped = NULL;
    if (_srs_context) {
        err->cid = _srs_context->get_id();
    }

    return err;
}

SrsCplxError* SrsCplxError::wrap(const char* func, const char* file, int line, SrsCplxError* v, const char* fmt, ...) {
    int rerrno = (int)errno;

    char buffer[maxErrorBuf];
    va_list ap;
    va_start(ap, fmt);
    int r0 = vsnprintf(buffer, maxErrorBuf, fmt, ap);
    va_end(ap);

    SrsCplxError* err = new SrsCplxError();

    err->func = func;
    err->file = file;
    err->line = line;
    if (v) {
        err->code = v->code;
    }
    err->rerrno = rerrno;
    if (r0 > 0 && r0 < maxErrorBuf) {
        err->msg = string(buffer, r0);
    }
    err->wrapped = v;
    if (_srs_context) {
        err->cid = _srs_context->get_id();
    }

    return err;
}

SrsCplxError* SrsCplxError::success() {
    return NULL;
}

SrsCplxError* SrsCplxError::copy(SrsCplxError* from)
{
    if (from == srs_success) {
        return srs_success;
    }

    SrsCplxError* err = new SrsCplxError();

    err->code = from->code;
    err->wrapped = srs_error_copy(from->wrapped);
    err->msg = from->msg;
    err->func = from->func;
    err->file = from->file;
    err->line = from->line;
    err->cid = from->cid;
    err->rerrno = from->rerrno;
    err->desc = from->desc;

    return err;
}

string SrsCplxError::description(SrsCplxError* err)
{
    return err? err->description() : "Success";
}

string SrsCplxError::summary(SrsCplxError* err)
{
    return err? err->summary() : "Success";
}

int SrsCplxError::error_code(SrsCplxError* err)
{
    return err? err->code : ERROR_SUCCESS;
}

static struct
{
    SrsErrorCode code;
    const char* name;
} _srs_strerror_tab[] = {
    {ERROR_SUCCESS, "Success"},
    {ERROR_SOCKET_CREATE, "SocketCreate"},
    {ERROR_SOCKET_SETREUSE, "SocketReuse"},
    {ERROR_SOCKET_BIND, "SocketBind"},
    {ERROR_SOCKET_LISTEN, "SocketListen"},
    {ERROR_SOCKET_CLOSED, "SocketClosed"},
    {ERROR_SOCKET_READ, "SocketRead"},
    {ERROR_SOCKET_READ_FULLY, "SocketReadFully"},
    {ERROR_SOCKET_WRITE, "SocketWrite"},
    {ERROR_SOCKET_TIMEOUT, "SocketTimeout"},
    {ERROR_SYSTEM_PACKET_INVALID, "RtmpInvalidPacket"},
    {ERROR_SYSTEM_CLIENT_INVALID, "RtmpInvalidClient"},
    {ERROR_SYSTEM_ASSERT_FAILED, "SystemAssert"},
    {ERROR_READER_BUFFER_OVERFLOW, "FastStreamGrow"},
    {ERROR_SYSTEM_STREAM_BUSY, "StreamBusy"},
    {ERROR_THREAD_DISPOSED, "ThreadDispose"},
    {ERROR_THREAD_INTERRUPED, "ThreadInterrupt"},
    {ERROR_THREAD_TERMINATED, "ThreadTerminate"},
    {ERROR_THREAD_STARTED, "ThreadStarted"},
    {ERROR_SOCKET_ACCEPT, "SocketAccept"},
    {ERROR_THREAD_CREATE, "ThreadCreate"},
    {ERROR_THREAD_FINISHED, "ThreadFinished"},
    {ERROR_RTMP_PLAIN_REQUIRED, "RtmpPlainRequired"},
    {ERROR_RTMP_CHUNK_START, "RtmpChunkStart"},
    {ERROR_RTMP_MSG_INVALID_SIZE, "RtmpMsgSize"},
    {ERROR_RTMP_AMF0_DECODE, "Amf0Decode"},
    {ERROR_RTMP_AMF0_INVALID, "Amf0Invalid"},
    {ERROR_RTMP_REQ_CONNECT, "RtmpConnect"},
    {ERROR_RTMP_REQ_TCURL, "RtmpTcUrl"},
    {ERROR_RTMP_MESSAGE_DECODE, "RtmpDecode"},
    {ERROR_RTMP_MESSAGE_ENCODE, "RtmpEncode"},
    {ERROR_RTMP_AMF0_ENCODE, "Amf0Encode"},
    {ERROR_RTMP_CHUNK_SIZE, "RtmpChunkSize"},
    {ERROR_RTMP_PACKET_SIZE, "RtmpPacketSize"},
    {ERROR_RTMP_HANDSHAKE, "RtmpHandshake"},
    {ERROR_RTMP_NO_REQUEST, "RtmpNoRequest"},
    {ERROR_RTMP_STREAM_NOT_FOUND, "StreamNotFound"},
    {ERROR_RTMP_STREAM_NAME_EMPTY, "StreamNameEmpty"},
    {ERROR_RTMP_MESSAGE_CREATE, "MessageCreate"},
    {ERROR_RTMP_CREATE_STREAM_DEPTH, "RtmpIdentify"},
    {ERROR_SYSTEM_FILE_OPENE, "FileOpen"},
    {ERROR_SYSTEM_FILE_WRITE, "FileWrite"},
    {ERROR_SYSTEM_FILE_RENAME, "FileRename"},
    {ERROR_SYSTEM_FILE_NOT_EXISTS, "FileNotFound"},
    {ERROR_HLS_DECODE_ERROR, "HlsDecode"},
    {ERROR_HLS_CREATE_DIR, "HlsCreateDir"},
    {ERROR_HLS_AVC_SAMPLE_SIZE, "HlsAvcFrame"},
    {ERROR_HTTP_PARSE_HEADER, "HttpParseHeader"},
    {ERROR_MP4_ILLEGAL_MOOF, "Mp4BoxNoMoof"},
    {ERROR_AVC_NALU_UEV, "AvcNaluUev"},
    {ERROR_CONTROL_RTMP_CLOSE, "RtmpClose"},
    {ERROR_CONTROL_REPUBLISH, "RtmpRepublish"},
    {ERROR_USER_DISCONNECT, "UserDisconnect"},
    {ERROR_SOURCE_NOT_FOUND, "UserNoSource"},
};

static std::map<SrsErrorCode, string> srs_build_error_map()
{
    std::map<SrsErrorCode, string> error_map;
    for (int i = 0; i < (int)(sizeof(_srs_strerror_tab) / sizeof(_srs_strerror_tab[0])); i++) {
        SrsErrorCode code = _srs_strerror_tab[i].code;
        error_map[code] = _srs_strerror_tab[i].name;
    }
    return error_map;
}

std::string SrsCplxError::error_code_str(SrsCplxError* err)
{
    static string not_found = "";
    // 원본은 호출 시 lazy 채움 — ST 단일 스레드라 안전. pthread 모델에서는
    // magic static 초기화로 대체한다 (CLAUDE.md §5.6).
    static std::map<SrsErrorCode, string> error_map = srs_build_error_map();

    std::map<SrsErrorCode, string>::iterator it = error_map.find((SrsErrorCode)srs_error_code(err));
    if (it == error_map.end()) {
        return not_found;
    }

    return it->second;
}

void SrsCplxError::srs_assert(bool expression)
{
    assert(expression);
}
