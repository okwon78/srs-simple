// srs_simple — 원본: trunk/src/protocol/srs_protocol_rtmp_handshake.hpp
// 심플 핸드셰이크만 유지 (복잡 핸드셰이크 HMAC-SHA256/DH 약 1,000줄 제거 — CLAUDE.md §1).
// OBS/ffmpeg는 심플 핸드셰이크로 충분하다. 복잡판은 Flash 플레이어 전용.
// SrsHandshakeBytes는 원본에서 rtmp_stack.{hpp,cpp}에 있으나, S3 시점에 rtmp_stack이
// 없으므로 여기로 이동 (CLAUDE.md §5.6). 서버 역할만 구현 (handshake_with_server/proxy 제거).
#ifndef SRS_PROTOCOL_RTMP_HANDSHAKE_HPP
#define SRS_PROTOCOL_RTMP_HANDSHAKE_HPP

#include <srs_core.hpp>

class ISrsProtocolReader;
class ISrsProtocolReadWriter;

// The handshake data, 지연 할당(lazy alloc)으로 핸드셰이크 동안만 메모리 사용.
//   C0 = 버전 1바이트(0x03 = plain RTMP)
//   C1/S1 = time(4) | version(4) | random(1528) — 총 1536바이트
//   S2 = C1의 복사, C2 = 읽고 폐기 (SRS도 검증하지 않음 — ffmpeg 호환)
class SrsHandshakeBytes
{
public:
    // [1+1536]
    char* c0c1;
    // [1+1536+1536]
    char* s0s1s2;
    // [1536]
    char* c2;
public:
    SrsHandshakeBytes();
    virtual ~SrsHandshakeBytes();
public:
    virtual void dispose();
public:
    virtual srs_error_t read_c0c1(ISrsProtocolReader* io);
    virtual srs_error_t read_c2(ISrsProtocolReader* io);
    virtual srs_error_t create_s0s1s2(const char* c1 = NULL);
};

// Simple handshake.
// @see also: https://blog.csdn.net/win_lin/article/details/13006803
class SrsSimpleHandshake
{
public:
    SrsSimpleHandshake();
    virtual ~SrsSimpleHandshake();
public:
    // Simple handshake.
    virtual srs_error_t handshake_with_client(SrsHandshakeBytes* hs_bytes, ISrsProtocolReadWriter* io);
};

#endif
