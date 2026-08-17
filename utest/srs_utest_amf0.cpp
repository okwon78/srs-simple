// srs_simple — 원본: trunk/src/utest/srs_utest_amf0.cpp (서브셋)
#include <srs_utest.hpp>

#include <srs_kernel_buffer.hpp>
#include <srs_protocol_amf0.hpp>

// 타입별 라운드트립: 인코딩 바이트가 스펙과 일치하고, 다시 읽으면 같은 값이 나온다.
VOID TEST(ProtocolAmf0Test, NumberRoundtrip)
{
    srs_error_t err = srs_success;

    char buf[16];
    HELPER_ARRAY_INIT(buf, sizeof(buf), 0x00);

    // write: marker(0x00) + 8B IEEE754 BE
    if (true) {
        SrsBuffer b(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(srs_amf0_write_number(&b, 1.0));
        EXPECT_EQ(9, b.pos());

        uint8_t expect[] = {0x00, 0x3f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        EXPECT_TRUE(0 == memcmp(buf, expect, sizeof(expect)));
    }

    // read back
    if (true) {
        SrsBuffer b(buf, sizeof(buf));
        double value = 0;
        HELPER_EXPECT_SUCCESS(srs_amf0_read_number(&b, value));
        EXPECT_DOUBLE_EQ(1.0, value);
    }

    // any 인터페이스로도 동일
    if (true) {
        SrsAmf0Any* any = SrsAmf0Any::number(100.1);
        EXPECT_TRUE(any->is_number());
        EXPECT_EQ(9, any->total_size());

        SrsBuffer b(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(any->write(&b));
        srs_freep(any);

        SrsBuffer r(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(srs_amf0_read_any(&r, &any));
        ASSERT_TRUE(any != NULL);
        EXPECT_TRUE(any->is_number());
        EXPECT_DOUBLE_EQ(100.1, any->to_number());
        srs_freep(any);
    }
}

VOID TEST(ProtocolAmf0Test, BooleanRoundtrip)
{
    srs_error_t err = srs_success;

    char buf[4];
    HELPER_ARRAY_INIT(buf, sizeof(buf), 0x00);

    if (true) {
        SrsBuffer b(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(srs_amf0_write_boolean(&b, true));
        EXPECT_EQ(2, b.pos());
        EXPECT_EQ(0x01, buf[0]);
        EXPECT_EQ(0x01, buf[1]);
    }

    if (true) {
        SrsBuffer b(buf, sizeof(buf));
        bool value = false;
        HELPER_EXPECT_SUCCESS(srs_amf0_read_boolean(&b, value));
        EXPECT_TRUE(value);
    }

    // 0이 아닌 값은 모두 true (0x01만이 아님)
    if (true) {
        buf[1] = 0x77;
        SrsBuffer b(buf, sizeof(buf));
        bool value = false;
        HELPER_EXPECT_SUCCESS(srs_amf0_read_boolean(&b, value));
        EXPECT_TRUE(value);
    }
}

VOID TEST(ProtocolAmf0Test, StringRoundtrip)
{
    srs_error_t err = srs_success;

    char buf[32];
    HELPER_ARRAY_INIT(buf, sizeof(buf), 0x00);

    // write: marker(0x02) + u16 len + data
    if (true) {
        SrsBuffer b(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(srs_amf0_write_string(&b, "live"));
        EXPECT_EQ(7, b.pos());

        uint8_t expect[] = {0x02, 0x00, 0x04, 'l', 'i', 'v', 'e'};
        EXPECT_TRUE(0 == memcmp(buf, expect, sizeof(expect)));
    }

    if (true) {
        SrsBuffer b(buf, sizeof(buf));
        string value;
        HELPER_EXPECT_SUCCESS(srs_amf0_read_string(&b, value));
        EXPECT_STREQ("live", value.c_str());
    }

    // 빈 문자열도 허용
    if (true) {
        SrsBuffer b(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(srs_amf0_write_string(&b, ""));
        EXPECT_EQ(3, b.pos());

        SrsBuffer r(buf, sizeof(buf));
        string value = "dirty";
        HELPER_EXPECT_SUCCESS(srs_amf0_read_string(&r, value));
        EXPECT_STREQ("dirty", value.c_str()); // len=0이면 값을 건드리지 않음 (원본과 동일)
    }
}

VOID TEST(ProtocolAmf0Test, NullUndefinedRoundtrip)
{
    srs_error_t err = srs_success;

    char buf[4];
    HELPER_ARRAY_INIT(buf, sizeof(buf), 0x7f);

    if (true) {
        SrsBuffer b(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(srs_amf0_write_null(&b));
        EXPECT_EQ(1, b.pos());
        EXPECT_EQ(0x05, buf[0]);

        SrsBuffer r(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(srs_amf0_read_null(&r));
    }

    if (true) {
        SrsBuffer b(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(srs_amf0_write_undefined(&b));
        EXPECT_EQ(1, b.pos());
        EXPECT_EQ(0x06, buf[0]);

        SrsBuffer r(buf, sizeof(buf));
        HELPER_EXPECT_SUCCESS(srs_amf0_read_undefined(&r));
    }
}

// Object 라운드트립: 삽입 순서 보존이 핵심 (FMLE는 순서가 바뀌면 죽는다 — CLAUDE.md §2.4)
VOID TEST(ProtocolAmf0Test, ObjectRoundtripKeepsOrder)
{
    srs_error_t err = srs_success;

    SrsAmf0Object* obj = SrsAmf0Any::object();
    // 사전순이 아닌 순서로 삽입 → 정렬되면 순서가 바뀌는 키들
    obj->set("width", SrsAmf0Any::number(1024));
    obj->set("app", SrsAmf0Any::str("live"));
    obj->set("secure", SrsAmf0Any::boolean(false));

    int nb = obj->total_size();
    // 1(marker) + [2+5 + 9] + [2+3 + 7] + [2+6 + 2] + 3(eof)
    EXPECT_EQ(1 + 16 + 12 + 10 + 3, nb);

    char* buf = new char[nb];
    if (true) {
        SrsBuffer b(buf, nb);
        HELPER_EXPECT_SUCCESS(obj->write(&b));
        EXPECT_EQ(nb, b.pos());
    }
    srs_freep(obj);

    if (true) {
        SrsBuffer b(buf, nb);
        SrsAmf0Any* any = NULL;
        HELPER_ASSERT_SUCCESS(srs_amf0_read_any(&b, &any));
        ASSERT_TRUE(any->is_object());

        SrsAmf0Object* o = any->to_object();
        ASSERT_EQ(3, o->count());
        // 삽입 순서 그대로 나온다
        EXPECT_STREQ("width", o->key_at(0).c_str());
        EXPECT_STREQ("app", o->key_at(1).c_str());
        EXPECT_STREQ("secure", o->key_at(2).c_str());
        EXPECT_DOUBLE_EQ(1024, o->value_at(0)->to_number());
        EXPECT_STREQ("live", o->value_at(1)->to_str().c_str());
        EXPECT_FALSE(o->value_at(2)->to_boolean());

        // 프로퍼티 조회 헬퍼
        EXPECT_TRUE(o->get_property("app") != NULL);
        EXPECT_TRUE(o->get_property("nonexists") == NULL);
        EXPECT_TRUE(o->ensure_property_string("app") != NULL);
        EXPECT_TRUE(o->ensure_property_string("width") == NULL); // number지 string이 아님
        EXPECT_TRUE(o->ensure_property_number("width") != NULL);

        srs_freep(any);
    }
    srs_freepa(buf);
}

VOID TEST(ProtocolAmf0Test, ObjectSetReplaceRemoveCopy)
{
    SrsAmf0Object* obj = SrsAmf0Any::object();
    obj->set("a", SrsAmf0Any::number(1));
    obj->set("b", SrsAmf0Any::str("x"));

    // 같은 키를 다시 set하면 교체 (개수 불변)
    obj->set("a", SrsAmf0Any::number(2));
    EXPECT_EQ(2, obj->count());
    EXPECT_DOUBLE_EQ(2, obj->get_property("a")->to_number());

    // copy는 깊은 복사
    SrsAmf0Any* cp = obj->copy();
    obj->set("a", SrsAmf0Any::number(3));
    EXPECT_DOUBLE_EQ(2, cp->to_object()->get_property("a")->to_number());
    srs_freep(cp);

    obj->remove("a");
    EXPECT_EQ(1, obj->count());
    EXPECT_TRUE(obj->get_property("a") == NULL);

    srs_freep(obj);
}

// 중첩 객체: connect 커맨드의 object 안에 object가 들어가는 경로
VOID TEST(ProtocolAmf0Test, NestedObject)
{
    srs_error_t err = srs_success;

    SrsAmf0Object* obj = SrsAmf0Any::object();
    SrsAmf0Object* data = SrsAmf0Any::object();
    data->set("version", SrsAmf0Any::str("1.0"));
    obj->set("data", data); // 소유권 이전
    obj->set("code", SrsAmf0Any::number(200));

    int nb = obj->total_size();
    char* buf = new char[nb];
    if (true) {
        SrsBuffer b(buf, nb);
        HELPER_EXPECT_SUCCESS(obj->write(&b));
        EXPECT_EQ(nb, b.pos());
    }
    srs_freep(obj);

    if (true) {
        SrsBuffer b(buf, nb);
        SrsAmf0Any* any = NULL;
        HELPER_ASSERT_SUCCESS(srs_amf0_read_any(&b, &any));

        SrsAmf0Object* o = any->to_object();
        ASSERT_EQ(2, o->count());
        ASSERT_TRUE(o->get_property("data")->is_object());
        SrsAmf0Object* d = o->get_property("data")->to_object();
        EXPECT_STREQ("1.0", d->ensure_property_string("version")->to_str().c_str());
        EXPECT_DOUBLE_EQ(200, o->ensure_property_number("code")->to_number());

        srs_freep(any);
    }
    srs_freepa(buf);
}

// 실제 ffmpeg가 보내는 connect 커맨드의 AMF0 페이로드 (hex 덤프 기반)
// string "connect" + number 1.0 + object{app, type, flashVer, tcUrl}
VOID TEST(ProtocolAmf0Test, FfmpegConnectDecode)
{
    srs_error_t err = srs_success;

    uint8_t data[] = {
        // string "connect"
        0x02, 0x00, 0x07, 'c', 'o', 'n', 'n', 'e', 'c', 't',
        // number 1.0 (transaction id)
        0x00, 0x3f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // command object
        0x03,
        0x00, 0x03, 'a', 'p', 'p',
        0x02, 0x00, 0x04, 'l', 'i', 'v', 'e',
        0x00, 0x04, 't', 'y', 'p', 'e',
        0x02, 0x00, 0x0a, 'n', 'o', 'n', 'p', 'r', 'i', 'v', 'a', 't', 'e',
        0x00, 0x08, 'f', 'l', 'a', 's', 'h', 'V', 'e', 'r',
        0x02, 0x00, 0x24, 'F', 'M', 'L', 'E', '/', '3', '.', '0', ' ', '(',
        'c', 'o', 'm', 'p', 'a', 't', 'i', 'b', 'l', 'e', ';', ' ',
        'L', 'a', 'v', 'f', '5', '8', '.', '2', '9', '.', '1', '0', '0', ')',
        0x00, 0x05, 't', 'c', 'U', 'r', 'l',
        0x02, 0x00, 0x1a, 'r', 't', 'm', 'p', ':', '/', '/',
        'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', ':', '1', '9', '3', '5', '/', 'l', 'i', 'v', 'e',
        // object eof
        0x00, 0x00, 0x09,
    };
    SrsBuffer b((char*)data, sizeof(data));

    // command name
    string command;
    HELPER_ASSERT_SUCCESS(srs_amf0_read_string(&b, command));
    EXPECT_STREQ("connect", command.c_str());

    // transaction id
    double tid = 0;
    HELPER_ASSERT_SUCCESS(srs_amf0_read_number(&b, tid));
    EXPECT_DOUBLE_EQ(1.0, tid);

    // command object
    SrsAmf0Any* any = NULL;
    HELPER_ASSERT_SUCCESS(srs_amf0_read_any(&b, &any));
    ASSERT_TRUE(any->is_object());
    EXPECT_TRUE(b.empty()); // 전체 페이로드 소비

    SrsAmf0Object* obj = any->to_object();
    ASSERT_EQ(4, obj->count());
    EXPECT_STREQ("live", obj->ensure_property_string("app")->to_str().c_str());
    EXPECT_STREQ("nonprivate", obj->ensure_property_string("type")->to_str().c_str());
    EXPECT_STREQ("FMLE/3.0 (compatible; Lavf58.29.100)", obj->ensure_property_string("flashVer")->to_str().c_str());
    EXPECT_STREQ("rtmp://localhost:1935/live", obj->ensure_property_string("tcUrl")->to_str().c_str());

    // 재직렬화하면 원본 바이트와 동일 (total_size 일치 확인 포함)
    int nb = obj->total_size();
    EXPECT_EQ((int)sizeof(data) - 10 - 9, nb);
    char* out = new char[nb];
    SrsBuffer w(out, nb);
    HELPER_ASSERT_SUCCESS(obj->write(&w));
    EXPECT_TRUE(0 == memcmp(out, data + 10 + 9, nb));
    srs_freepa(out);

    srs_freep(any);
}

// ffmpeg의 onMetaData는 EcmaArray로 온다 → S6에서 Object로 변환해 쓰는 경로
VOID TEST(ProtocolAmf0Test, EcmaArrayToObject)
{
    srs_error_t err = srs_success;

    uint8_t data[] = {
        // ecma array, count=3
        0x08, 0x00, 0x00, 0x00, 0x03,
        0x00, 0x08, 'd', 'u', 'r', 'a', 't', 'i', 'o', 'n',
        0x00, 0x40, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 10.0
        0x00, 0x05, 'w', 'i', 'd', 't', 'h',
        0x00, 0x40, 0x9e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 1920.0
        0x00, 0x07, 'e', 'n', 'c', 'o', 'd', 'e', 'r',
        0x02, 0x00, 0x0d, 'L', 'a', 'v', 'f', '5', '8', '.', '2', '9', '.', '1', '0', '0',
        // object eof
        0x00, 0x00, 0x09,
    };
    SrsBuffer b((char*)data, sizeof(data));

    SrsAmf0Any* any = NULL;
    HELPER_ASSERT_SUCCESS(srs_amf0_read_any(&b, &any));
    ASSERT_TRUE(any->is_ecma_array());
    EXPECT_TRUE(b.empty());

    SrsAmf0EcmaArray* arr = any->to_ecma_array();
    ASSERT_EQ(3, arr->count());
    EXPECT_EQ(3, arr->_count); // 유선상의 associative-count

    // EcmaArray → Object 변환 (onMetaData 처리 방식, 원본 rtmp_stack의 이디엄)
    SrsAmf0Object* obj = SrsAmf0Any::object();
    for (int i = 0; i < arr->count(); i++) {
        SrsAmf0Any* value = arr->value_at(i);
        obj->set(arr->key_at(i), value->copy());
    }
    srs_freep(any);

    ASSERT_EQ(3, obj->count());
    // 순서 보존 확인
    EXPECT_STREQ("duration", obj->key_at(0).c_str());
    EXPECT_STREQ("width", obj->key_at(1).c_str());
    EXPECT_STREQ("encoder", obj->key_at(2).c_str());
    EXPECT_DOUBLE_EQ(10.0, obj->ensure_property_number("duration")->to_number());
    EXPECT_DOUBLE_EQ(1920.0, obj->ensure_property_number("width")->to_number());
    EXPECT_STREQ("Lavf58.29.100", obj->ensure_property_string("encoder")->to_str().c_str());

    srs_freep(obj);
}

VOID TEST(ProtocolAmf0Test, ObjectEofDetect)
{
    srs_error_t err = srs_success;

    // 00 00 09가 object-eof
    if (true) {
        uint8_t data[] = {0x00, 0x00, 0x09};
        SrsBuffer b((char*)data, sizeof(data));
        EXPECT_TRUE(srs_internal::srs_amf0_is_object_eof(&b));
        EXPECT_EQ(0, b.pos()); // 감지만 하고 소비하지 않음

        SrsAmf0Any* any = NULL;
        HELPER_ASSERT_SUCCESS(SrsAmf0Any::discovery(&b, &any));
        EXPECT_TRUE(any->is_object_eof());
        HELPER_EXPECT_SUCCESS(any->read(&b));
        EXPECT_EQ(3, b.pos());
        srs_freep(any);
    }

    // 다른 바이트는 eof가 아님
    if (true) {
        uint8_t data[] = {0x00, 0x01, 0x09};
        SrsBuffer b((char*)data, sizeof(data));
        EXPECT_FALSE(srs_internal::srs_amf0_is_object_eof(&b));
    }
}

VOID TEST(ProtocolAmf0Test, DecodeErrors)
{
    srs_error_t err = srs_success;

    // 서브셋에서 제거된 타입(StrictArray 0x0A)은 invalid
    if (true) {
        uint8_t data[] = {0x0a, 0x00, 0x00, 0x00, 0x01, 0x00};
        SrsBuffer b((char*)data, sizeof(data));
        SrsAmf0Any* any = NULL;
        err = SrsAmf0Any::discovery(&b, &any);
        EXPECT_EQ(ERROR_RTMP_AMF0_INVALID, srs_error_code(err));
        srs_freep(err);
    }

    // 마커 불일치
    if (true) {
        uint8_t data[] = {0x02, 0x00, 0x01, 'a'};
        SrsBuffer b((char*)data, sizeof(data));
        double v;
        HELPER_EXPECT_FAILED(srs_amf0_read_number(&b, v));
    }

    // utf8 데이터 잘림 (len=4인데 2바이트만 있음)
    if (true) {
        uint8_t data[] = {0x02, 0x00, 0x04, 'l', 'i'};
        SrsBuffer b((char*)data, sizeof(data));
        string v;
        HELPER_EXPECT_FAILED(srs_amf0_read_string(&b, v));
    }

    // object의 프로퍼티 값이 잘림
    if (true) {
        uint8_t data[] = {0x03, 0x00, 0x03, 'a', 'p', 'p', 0x02, 0x00, 0x04, 'l', 'i'};
        SrsBuffer b((char*)data, sizeof(data));
        SrsAmf0Any* any = NULL;
        HELPER_EXPECT_FAILED(srs_amf0_read_any(&b, &any));
        EXPECT_TRUE(any == NULL); // 실패 시 해제됨
    }
}

VOID TEST(ProtocolAmf0Test, Amf0Size)
{
    EXPECT_EQ(2 + 4, SrsAmf0Size::utf8("live"));
    EXPECT_EQ(1 + 2 + 4, SrsAmf0Size::str("live"));
    EXPECT_EQ(9, SrsAmf0Size::number());
    EXPECT_EQ(1, SrsAmf0Size::null());
    EXPECT_EQ(1, SrsAmf0Size::undefined());
    EXPECT_EQ(2, SrsAmf0Size::boolean());
    EXPECT_EQ(3, SrsAmf0Size::object_eof());

    // 빈 object = marker + eof
    SrsAmf0Object* obj = SrsAmf0Any::object();
    EXPECT_EQ(1 + 3, SrsAmf0Size::object(obj));
    srs_freep(obj);

    // 빈 ecma array = marker + count(4) + eof
    SrsAmf0EcmaArray* arr = SrsAmf0Any::ecma_array();
    EXPECT_EQ(1 + 4 + 3, SrsAmf0Size::ecma_array(arr));
    srs_freep(arr);
}
