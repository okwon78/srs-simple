// srs_simple — 원본: trunk/src/protocol/srs_protocol_amf0.cpp (1,779줄 → 서브셋)
#include <srs_protocol_amf0.hpp>

#include <string.h>

#include <utility>
#include <vector>
using namespace std;

#include <srs_kernel_error.hpp>
#include <srs_kernel_buffer.hpp>

using namespace srs_internal;

// AMF0 marker
#define RTMP_AMF0_Number                    0x00
#define RTMP_AMF0_Boolean                   0x01
#define RTMP_AMF0_String                    0x02
#define RTMP_AMF0_Object                    0x03
#define RTMP_AMF0_MovieClip                 0x04 // reserved, not supported
#define RTMP_AMF0_Null                      0x05
#define RTMP_AMF0_Undefined                 0x06
#define RTMP_AMF0_Reference                 0x07
#define RTMP_AMF0_EcmaArray                 0x08
#define RTMP_AMF0_ObjectEnd                 0x09
#define RTMP_AMF0_StrictArray               0x0A
#define RTMP_AMF0_Date                      0x0B
#define RTMP_AMF0_LongString                0x0C
#define RTMP_AMF0_UnSupported               0x0D
#define RTMP_AMF0_RecordSet                 0x0E // reserved, not supported
#define RTMP_AMF0_XmlDocument               0x0F
#define RTMP_AMF0_TypedObject               0x10
// AVM+ object is the AMF3 object.
#define RTMP_AMF0_AVMplusObject             0x11

// User defined
#define RTMP_AMF0_Invalid                   0x3F

SrsAmf0Any::SrsAmf0Any()
{
    marker = RTMP_AMF0_Invalid;
}

SrsAmf0Any::~SrsAmf0Any()
{
}

bool SrsAmf0Any::is_string()
{
    return marker == RTMP_AMF0_String;
}

bool SrsAmf0Any::is_boolean()
{
    return marker == RTMP_AMF0_Boolean;
}

bool SrsAmf0Any::is_number()
{
    return marker == RTMP_AMF0_Number;
}

bool SrsAmf0Any::is_null()
{
    return marker == RTMP_AMF0_Null;
}

bool SrsAmf0Any::is_undefined()
{
    return marker == RTMP_AMF0_Undefined;
}

bool SrsAmf0Any::is_object()
{
    return marker == RTMP_AMF0_Object;
}

bool SrsAmf0Any::is_object_eof()
{
    return marker == RTMP_AMF0_ObjectEnd;
}

bool SrsAmf0Any::is_ecma_array()
{
    return marker == RTMP_AMF0_EcmaArray;
}

bool SrsAmf0Any::is_complex_object()
{
    return is_object() || is_object_eof() || is_ecma_array();
}

string SrsAmf0Any::to_str()
{
    SrsAmf0String* p = dynamic_cast<SrsAmf0String*>(this);
    srs_assert(p != NULL);
    return p->value;
}

const char* SrsAmf0Any::to_str_raw()
{
    SrsAmf0String* p = dynamic_cast<SrsAmf0String*>(this);
    srs_assert(p != NULL);
    return p->value.data();
}

bool SrsAmf0Any::to_boolean()
{
    SrsAmf0Boolean* p = dynamic_cast<SrsAmf0Boolean*>(this);
    srs_assert(p != NULL);
    return p->value;
}

double SrsAmf0Any::to_number()
{
    SrsAmf0Number* p = dynamic_cast<SrsAmf0Number*>(this);
    srs_assert(p != NULL);
    return p->value;
}

SrsAmf0Object* SrsAmf0Any::to_object()
{
    SrsAmf0Object* p = dynamic_cast<SrsAmf0Object*>(this);
    srs_assert(p != NULL);
    return p;
}

SrsAmf0EcmaArray* SrsAmf0Any::to_ecma_array()
{
    SrsAmf0EcmaArray* p = dynamic_cast<SrsAmf0EcmaArray*>(this);
    srs_assert(p != NULL);
    return p;
}

void SrsAmf0Any::set_number(double value)
{
    SrsAmf0Number* p = dynamic_cast<SrsAmf0Number*>(this);
    srs_assert(p != NULL);
    p->value = value;
}

SrsAmf0Any* SrsAmf0Any::str(const char* value)
{
    return new SrsAmf0String(value);
}

SrsAmf0Any* SrsAmf0Any::boolean(bool value)
{
    return new SrsAmf0Boolean(value);
}

SrsAmf0Any* SrsAmf0Any::number(double value)
{
    return new SrsAmf0Number(value);
}

SrsAmf0Any* SrsAmf0Any::null()
{
    return new SrsAmf0Null();
}

SrsAmf0Any* SrsAmf0Any::undefined()
{
    return new SrsAmf0Undefined();
}

SrsAmf0Object* SrsAmf0Any::object()
{
    return new SrsAmf0Object();
}

SrsAmf0Any* SrsAmf0Any::object_eof()
{
    return new SrsAmf0ObjectEOF();
}

SrsAmf0EcmaArray* SrsAmf0Any::ecma_array()
{
    return new SrsAmf0EcmaArray();
}

srs_error_t SrsAmf0Any::discovery(SrsBuffer* stream, SrsAmf0Any** ppvalue)
{
    srs_error_t err = srs_success;

    // detect the object-eof specially
    if (srs_amf0_is_object_eof(stream)) {
        *ppvalue = new SrsAmf0ObjectEOF();
        return err;
    }

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "marker requires 1 only %d bytes", stream->left());
    }

    char marker = stream->read_1bytes();

    // move back over the 1 byte marker.
    stream->skip(-1);

    switch (marker) {
        case RTMP_AMF0_String: {
            *ppvalue = SrsAmf0Any::str();
            return err;
        }
        case RTMP_AMF0_Boolean: {
            *ppvalue = SrsAmf0Any::boolean();
            return err;
        }
        case RTMP_AMF0_Number: {
            *ppvalue = SrsAmf0Any::number();
            return err;
        }
        case RTMP_AMF0_Null: {
            *ppvalue = SrsAmf0Any::null();
            return err;
        }
        case RTMP_AMF0_Undefined: {
            *ppvalue = SrsAmf0Any::undefined();
            return err;
        }
        case RTMP_AMF0_Object: {
            *ppvalue = SrsAmf0Any::object();
            return err;
        }
        case RTMP_AMF0_EcmaArray: {
            *ppvalue = SrsAmf0Any::ecma_array();
            return err;
        }
        // StrictArray/Date 등은 서브셋에서 제거 — invalid로 처리 (원본은 지원).
        case RTMP_AMF0_Invalid:
        default: {
            return srs_error_new(ERROR_RTMP_AMF0_INVALID, "invalid amf0 message, marker=%#x", marker);
        }
    }
}

SrsUnSortedHashtable::SrsUnSortedHashtable()
{
}

SrsUnSortedHashtable::~SrsUnSortedHashtable()
{
    clear();
}

int SrsUnSortedHashtable::count()
{
    return (int)properties.size();
}

void SrsUnSortedHashtable::clear()
{
    std::vector<SrsAmf0ObjectPropertyType>::iterator it;
    for (it = properties.begin(); it != properties.end(); ++it) {
        SrsAmf0ObjectPropertyType& elem = *it;
        SrsAmf0Any* any = elem.second;
        srs_freep(any);
    }
    properties.clear();
}

string SrsUnSortedHashtable::key_at(int index)
{
    srs_assert(index < count());
    SrsAmf0ObjectPropertyType& elem = properties[index];
    return elem.first;
}

const char* SrsUnSortedHashtable::key_raw_at(int index)
{
    srs_assert(index < count());
    SrsAmf0ObjectPropertyType& elem = properties[index];
    return elem.first.data();
}

SrsAmf0Any* SrsUnSortedHashtable::value_at(int index)
{
    srs_assert(index < count());
    SrsAmf0ObjectPropertyType& elem = properties[index];
    return elem.second;
}

void SrsUnSortedHashtable::set(string key, SrsAmf0Any* value)
{
    std::vector<SrsAmf0ObjectPropertyType>::iterator it;

    for (it = properties.begin(); it != properties.end(); ++it) {
        SrsAmf0ObjectPropertyType& elem = *it;
        std::string name = elem.first;
        SrsAmf0Any* any = elem.second;

        if (key == name) {
            srs_freep(any);
            it = properties.erase(it);
            break;
        }
    }

    if (value) {
        properties.push_back(std::make_pair(key, value));
    }
}

SrsAmf0Any* SrsUnSortedHashtable::get_property(string name)
{
    std::vector<SrsAmf0ObjectPropertyType>::iterator it;

    for (it = properties.begin(); it != properties.end(); ++it) {
        SrsAmf0ObjectPropertyType& elem = *it;
        std::string key = elem.first;
        SrsAmf0Any* any = elem.second;
        if (key == name) {
            return any;
        }
    }

    return NULL;
}

SrsAmf0Any* SrsUnSortedHashtable::ensure_property_string(string name)
{
    SrsAmf0Any* prop = get_property(name);

    if (!prop) {
        return NULL;
    }

    if (!prop->is_string()) {
        return NULL;
    }

    return prop;
}

SrsAmf0Any* SrsUnSortedHashtable::ensure_property_number(string name)
{
    SrsAmf0Any* prop = get_property(name);

    if (!prop) {
        return NULL;
    }

    if (!prop->is_number()) {
        return NULL;
    }

    return prop;
}

void SrsUnSortedHashtable::remove(string name)
{
    std::vector<SrsAmf0ObjectPropertyType>::iterator it;

    for (it = properties.begin(); it != properties.end();) {
        std::string key = it->first;
        SrsAmf0Any* any = it->second;

        if (key == name) {
            srs_freep(any);

            it = properties.erase(it);
        } else {
            ++it;
        }
    }
}

void SrsUnSortedHashtable::copy(SrsUnSortedHashtable* src)
{
    std::vector<SrsAmf0ObjectPropertyType>::iterator it;
    for (it = src->properties.begin(); it != src->properties.end(); ++it) {
        SrsAmf0ObjectPropertyType& elem = *it;
        std::string key = elem.first;
        SrsAmf0Any* any = elem.second;
        set(key, any->copy());
    }
}

SrsAmf0ObjectEOF::SrsAmf0ObjectEOF()
{
    marker = RTMP_AMF0_ObjectEnd;
}

SrsAmf0ObjectEOF::~SrsAmf0ObjectEOF()
{
}

int SrsAmf0ObjectEOF::total_size()
{
    return SrsAmf0Size::object_eof();
}

srs_error_t SrsAmf0ObjectEOF::read(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // value
    if (!stream->require(2)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "EOF requires 2 only %d bytes", stream->left());
    }
    int16_t temp = stream->read_2bytes();
    if (temp != 0x00) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "EOF invalid marker=%#x", temp);
    }

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "EOF requires 1 only %d bytes", stream->left());
    }

    char marker = stream->read_1bytes();
    if (marker != RTMP_AMF0_ObjectEnd) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "EOF invalid marker=%#x", marker);
    }

    return err;
}

srs_error_t SrsAmf0ObjectEOF::write(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // value
    if (!stream->require(2)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "EOF requires 2 only %d bytes", stream->left());
    }
    stream->write_2bytes(0x00);

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "EOF requires 1 only %d bytes", stream->left());
    }

    stream->write_1bytes(RTMP_AMF0_ObjectEnd);

    return err;
}

SrsAmf0Any* SrsAmf0ObjectEOF::copy()
{
    return new SrsAmf0ObjectEOF();
}

SrsAmf0Object::SrsAmf0Object()
{
    properties = new SrsUnSortedHashtable();
    eof = new SrsAmf0ObjectEOF();
    marker = RTMP_AMF0_Object;
}

SrsAmf0Object::~SrsAmf0Object()
{
    srs_freep(properties);
    srs_freep(eof);
}

int SrsAmf0Object::total_size()
{
    int size = 1;

    for (int i = 0; i < properties->count(); i++){
        std::string name = key_at(i);
        SrsAmf0Any* value = value_at(i);

        size += SrsAmf0Size::utf8(name);
        size += SrsAmf0Size::any(value);
    }

    size += SrsAmf0Size::object_eof();

    return size;
}

srs_error_t SrsAmf0Object::read(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "object requires 1 only %d bytes", stream->left());
    }

    char marker = stream->read_1bytes();
    if (marker != RTMP_AMF0_Object) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "object invalid marker=%#x", marker);
    }

    // value
    while (!stream->empty()) {
        // detect whether it is eof.
        if (srs_amf0_is_object_eof(stream)) {
            SrsAmf0ObjectEOF pbj_eof;
            if ((err = pbj_eof.read(stream)) != srs_success) {
                return srs_error_wrap(err, "read EOF");
            }
            break;
        }

        // property-name: utf8 string
        std::string property_name;
        if ((err = srs_amf0_read_utf8(stream, property_name)) != srs_success) {
            return srs_error_wrap(err, "read property name");
        }
        // property-value: any
        SrsAmf0Any* property_value = NULL;
        if ((err = srs_amf0_read_any(stream, &property_value)) != srs_success) {
            srs_freep(property_value);
            return srs_error_wrap(err, "read property value, name=%s", property_name.c_str());
        }

        // add property
        this->set(property_name, property_value);
    }

    return err;
}

srs_error_t SrsAmf0Object::write(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "object requires 1 only %d bytes", stream->left());
    }

    stream->write_1bytes(RTMP_AMF0_Object);

    // value
    for (int i = 0; i < properties->count(); i++) {
        std::string name = this->key_at(i);
        SrsAmf0Any* any = this->value_at(i);

        if ((err = srs_amf0_write_utf8(stream, name)) != srs_success) {
            return srs_error_wrap(err, "write property name=%s", name.c_str());
        }

        if ((err = srs_amf0_write_any(stream, any)) != srs_success) {
            return srs_error_wrap(err, "write property value, name=%s", name.c_str());
        }
    }

    if ((err = eof->write(stream)) != srs_success) {
        return srs_error_wrap(err, "write EOF");
    }

    return err;
}

SrsAmf0Any* SrsAmf0Object::copy()
{
    SrsAmf0Object* copy = new SrsAmf0Object();
    copy->properties->copy(properties);
    return copy;
}

void SrsAmf0Object::clear()
{
    properties->clear();
}

int SrsAmf0Object::count()
{
    return properties->count();
}

string SrsAmf0Object::key_at(int index)
{
    return properties->key_at(index);
}

const char* SrsAmf0Object::key_raw_at(int index)
{
    return properties->key_raw_at(index);
}

SrsAmf0Any* SrsAmf0Object::value_at(int index)
{
    return properties->value_at(index);
}

void SrsAmf0Object::set(string key, SrsAmf0Any* value)
{
    properties->set(key, value);
}

SrsAmf0Any* SrsAmf0Object::get_property(string name)
{
    return properties->get_property(name);
}

SrsAmf0Any* SrsAmf0Object::ensure_property_string(string name)
{
    return properties->ensure_property_string(name);
}

SrsAmf0Any* SrsAmf0Object::ensure_property_number(string name)
{
    return properties->ensure_property_number(name);
}

void SrsAmf0Object::remove(string name)
{
    properties->remove(name);
}

SrsAmf0EcmaArray::SrsAmf0EcmaArray()
{
    _count = 0;
    properties = new SrsUnSortedHashtable();
    eof = new SrsAmf0ObjectEOF();
    marker = RTMP_AMF0_EcmaArray;
}

SrsAmf0EcmaArray::~SrsAmf0EcmaArray()
{
    srs_freep(properties);
    srs_freep(eof);
}

int SrsAmf0EcmaArray::total_size()
{
    int size = 1 + 4;

    for (int i = 0; i < properties->count(); i++){
        std::string name = key_at(i);
        SrsAmf0Any* value = value_at(i);

        size += SrsAmf0Size::utf8(name);
        size += SrsAmf0Size::any(value);
    }

    size += SrsAmf0Size::object_eof();

    return size;
}

srs_error_t SrsAmf0EcmaArray::read(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 1 only %d bytes", stream->left());
    }

    char marker = stream->read_1bytes();
    if (marker != RTMP_AMF0_EcmaArray) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "EcmaArray invalid marker=%#x", marker);
    }

    // count
    if (!stream->require(4)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 4 only %d bytes", stream->left());
    }

    int32_t count = stream->read_4bytes();

    // value
    this->_count = count;

    while (!stream->empty()) {
        // detect whether it is eof.
        if (srs_amf0_is_object_eof(stream)) {
            SrsAmf0ObjectEOF pbj_eof;
            if ((err = pbj_eof.read(stream)) != srs_success) {
                return srs_error_wrap(err, "read EOF");
            }
            break;
        }

        // property-name: utf8 string
        std::string property_name;
        if ((err = srs_amf0_read_utf8(stream, property_name)) != srs_success) {
            return srs_error_wrap(err, "read property name");
        }
        // property-value: any
        SrsAmf0Any* property_value = NULL;
        if ((err = srs_amf0_read_any(stream, &property_value)) != srs_success) {
            return srs_error_wrap(err, "read property value, name=%s", property_name.c_str());
        }

        // add property
        this->set(property_name, property_value);
    }

    return err;
}

srs_error_t SrsAmf0EcmaArray::write(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 1 only %d bytes", stream->left());
    }

    stream->write_1bytes(RTMP_AMF0_EcmaArray);

    // count
    if (!stream->require(4)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 4 only %d bytes", stream->left());
    }

    stream->write_4bytes(this->_count);

    // value
    for (int i = 0; i < properties->count(); i++) {
        std::string name = this->key_at(i);
        SrsAmf0Any* any = this->value_at(i);

        if ((err = srs_amf0_write_utf8(stream, name)) != srs_success) {
            return srs_error_wrap(err, "write property name=%s", name.c_str());
        }

        if ((err = srs_amf0_write_any(stream, any)) != srs_success) {
            return srs_error_wrap(err, "write property value, name=%s", name.c_str());
        }
    }

    if ((err = eof->write(stream)) != srs_success) {
        return srs_error_wrap(err, "write EOF");
    }

    return err;
}

SrsAmf0Any* SrsAmf0EcmaArray::copy()
{
    SrsAmf0EcmaArray* copy = new SrsAmf0EcmaArray();
    copy->properties->copy(properties);
    copy->_count = _count;
    return copy;
}

void SrsAmf0EcmaArray::clear()
{
    properties->clear();
}

int SrsAmf0EcmaArray::count()
{
    return properties->count();
}

string SrsAmf0EcmaArray::key_at(int index)
{
    return properties->key_at(index);
}

const char* SrsAmf0EcmaArray::key_raw_at(int index)
{
    return properties->key_raw_at(index);
}

SrsAmf0Any* SrsAmf0EcmaArray::value_at(int index)
{
    return properties->value_at(index);
}

void SrsAmf0EcmaArray::set(string key, SrsAmf0Any* value)
{
    properties->set(key, value);
}

SrsAmf0Any* SrsAmf0EcmaArray::get_property(string name)
{
    return properties->get_property(name);
}

SrsAmf0Any* SrsAmf0EcmaArray::ensure_property_string(string name)
{
    return properties->ensure_property_string(name);
}

SrsAmf0Any* SrsAmf0EcmaArray::ensure_property_number(string name)
{
    return properties->ensure_property_number(name);
}

int SrsAmf0Size::utf8(string value)
{
    return (int)(2 + value.length());
}

int SrsAmf0Size::str(string value)
{
    return 1 + SrsAmf0Size::utf8(value);
}

int SrsAmf0Size::number()
{
    return 1 + 8;
}

int SrsAmf0Size::null()
{
    return 1;
}

int SrsAmf0Size::undefined()
{
    return 1;
}

int SrsAmf0Size::boolean()
{
    return 1 + 1;
}

int SrsAmf0Size::object(SrsAmf0Object* obj)
{
    if (!obj) {
        return 0;
    }

    return obj->total_size();
}

int SrsAmf0Size::object_eof()
{
    return 2 + 1;
}

int SrsAmf0Size::ecma_array(SrsAmf0EcmaArray* arr)
{
    if (!arr) {
        return 0;
    }

    return arr->total_size();
}

int SrsAmf0Size::any(SrsAmf0Any* o)
{
    if (!o) {
        return 0;
    }

    return o->total_size();
}

SrsAmf0String::SrsAmf0String(const char* _value)
{
    marker = RTMP_AMF0_String;
    if (_value) {
        value = _value;
    }
}

SrsAmf0String::~SrsAmf0String()
{
}

int SrsAmf0String::total_size()
{
    return SrsAmf0Size::str(value);
}

srs_error_t SrsAmf0String::read(SrsBuffer* stream)
{
    return srs_amf0_read_string(stream, value);
}

srs_error_t SrsAmf0String::write(SrsBuffer* stream)
{
    return srs_amf0_write_string(stream, value);
}

SrsAmf0Any* SrsAmf0String::copy()
{
    SrsAmf0String* copy = new SrsAmf0String(value.c_str());
    return copy;
}

SrsAmf0Boolean::SrsAmf0Boolean(bool _value)
{
    marker = RTMP_AMF0_Boolean;
    value = _value;
}

SrsAmf0Boolean::~SrsAmf0Boolean()
{
}

int SrsAmf0Boolean::total_size()
{
    return SrsAmf0Size::boolean();
}

srs_error_t SrsAmf0Boolean::read(SrsBuffer* stream)
{
    return srs_amf0_read_boolean(stream, value);
}

srs_error_t SrsAmf0Boolean::write(SrsBuffer* stream)
{
    return srs_amf0_write_boolean(stream, value);
}

SrsAmf0Any* SrsAmf0Boolean::copy()
{
    SrsAmf0Boolean* copy = new SrsAmf0Boolean(value);
    return copy;
}

SrsAmf0Number::SrsAmf0Number(double _value)
{
    marker = RTMP_AMF0_Number;
    value = _value;
}

SrsAmf0Number::~SrsAmf0Number()
{
}

int SrsAmf0Number::total_size()
{
    return SrsAmf0Size::number();
}

srs_error_t SrsAmf0Number::read(SrsBuffer* stream)
{
    return srs_amf0_read_number(stream, value);
}

srs_error_t SrsAmf0Number::write(SrsBuffer* stream)
{
    return srs_amf0_write_number(stream, value);
}

SrsAmf0Any* SrsAmf0Number::copy()
{
    SrsAmf0Number* copy = new SrsAmf0Number(value);
    return copy;
}

SrsAmf0Null::SrsAmf0Null()
{
    marker = RTMP_AMF0_Null;
}

SrsAmf0Null::~SrsAmf0Null()
{
}

int SrsAmf0Null::total_size()
{
    return SrsAmf0Size::null();
}

srs_error_t SrsAmf0Null::read(SrsBuffer* stream)
{
    return srs_amf0_read_null(stream);
}

srs_error_t SrsAmf0Null::write(SrsBuffer* stream)
{
    return srs_amf0_write_null(stream);
}

SrsAmf0Any* SrsAmf0Null::copy()
{
    SrsAmf0Null* copy = new SrsAmf0Null();
    return copy;
}

SrsAmf0Undefined::SrsAmf0Undefined()
{
    marker = RTMP_AMF0_Undefined;
}

SrsAmf0Undefined::~SrsAmf0Undefined()
{
}

int SrsAmf0Undefined::total_size()
{
    return SrsAmf0Size::undefined();
}

srs_error_t SrsAmf0Undefined::read(SrsBuffer* stream)
{
    return srs_amf0_read_undefined(stream);
}

srs_error_t SrsAmf0Undefined::write(SrsBuffer* stream)
{
    return srs_amf0_write_undefined(stream);
}

SrsAmf0Any* SrsAmf0Undefined::copy()
{
    SrsAmf0Undefined* copy = new SrsAmf0Undefined();
    return copy;
}

srs_error_t srs_amf0_read_any(SrsBuffer* stream, SrsAmf0Any** ppvalue)
{
    srs_error_t err = srs_success;

    if ((err = SrsAmf0Any::discovery(stream, ppvalue)) != srs_success) {
        return srs_error_wrap(err, "discovery");
    }

    srs_assert(*ppvalue);

    if ((err = (*ppvalue)->read(stream)) != srs_success) {
        srs_freep(*ppvalue);
        return srs_error_wrap(err, "parse elem");
    }

    return err;
}

srs_error_t srs_amf0_read_string(SrsBuffer* stream, string& value)
{
    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 1 only %d bytes", stream->left());
    }

    char marker = stream->read_1bytes();
    if (marker != RTMP_AMF0_String) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "String invalid marker=%#x", marker);
    }

    return srs_amf0_read_utf8(stream, value);
}

srs_error_t srs_amf0_write_string(SrsBuffer* stream, string value)
{
    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 1 only %d bytes", stream->left());
    }

    stream->write_1bytes(RTMP_AMF0_String);

    return srs_amf0_write_utf8(stream, value);
}

srs_error_t srs_amf0_read_boolean(SrsBuffer* stream, bool& value)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 1 only %d bytes", stream->left());
    }

    char marker = stream->read_1bytes();
    if (marker != RTMP_AMF0_Boolean) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "Boolean invalid marker=%#x", marker);
    }

    // value
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 1 only %d bytes", stream->left());
    }

    value = (stream->read_1bytes() != 0);

    return err;
}

srs_error_t srs_amf0_write_boolean(SrsBuffer* stream, bool value)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 1 only %d bytes", stream->left());
    }
    stream->write_1bytes(RTMP_AMF0_Boolean);

    // value
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 1 only %d bytes", stream->left());
    }

    if (value) {
        stream->write_1bytes(0x01);
    } else {
        stream->write_1bytes(0x00);
    }

    return err;
}

srs_error_t srs_amf0_read_number(SrsBuffer* stream, double& value)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 1 only %d bytes", stream->left());
    }

    char marker = stream->read_1bytes();
    if (marker != RTMP_AMF0_Number) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "Number invalid marker=%#x", marker);
    }

    // value
    if (!stream->require(8)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 8 only %d bytes", stream->left());
    }

    int64_t temp = stream->read_8bytes();
    memcpy(&value, &temp, 8);

    return err;
}

srs_error_t srs_amf0_write_number(SrsBuffer* stream, double value)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 1 only %d bytes", stream->left());
    }

    stream->write_1bytes(RTMP_AMF0_Number);

    // value
    if (!stream->require(8)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 8 only %d bytes", stream->left());
    }

    int64_t temp = 0x00;
    memcpy(&temp, &value, 8);
    stream->write_8bytes(temp);

    return err;
}

srs_error_t srs_amf0_read_null(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 1 only %d bytes", stream->left());
    }

    char marker = stream->read_1bytes();
    if (marker != RTMP_AMF0_Null) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "Null invalid marker=%#x", marker);
    }

    return err;
}

srs_error_t srs_amf0_write_null(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 1 only %d bytes", stream->left());
    }

    stream->write_1bytes(RTMP_AMF0_Null);

    return err;
}

srs_error_t srs_amf0_read_undefined(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 1 only %d bytes", stream->left());
    }

    char marker = stream->read_1bytes();
    if (marker != RTMP_AMF0_Undefined) {
        return srs_error_new(ERROR_RTMP_AMF0_DECODE, "Undefined invalid marker=%#x", marker);
    }

    return err;
}

srs_error_t srs_amf0_write_undefined(SrsBuffer* stream)
{
    srs_error_t err = srs_success;

    // marker
    if (!stream->require(1)) {
        return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 1 only %d bytes", stream->left());
    }

    stream->write_1bytes(RTMP_AMF0_Undefined);

    return err;
}

namespace srs_internal
{
    srs_error_t srs_amf0_read_utf8(SrsBuffer* stream, string& value)
    {
        srs_error_t err = srs_success;

        // len
        if (!stream->require(2)) {
            return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires 2 only %d bytes", stream->left());
        }
        int16_t len = stream->read_2bytes();

        // empty string
        if (len <= 0) {
            return err;
        }

        // data
        if (!stream->require(len)) {
            return srs_error_new(ERROR_RTMP_AMF0_DECODE, "requires %d only %d bytes", len, stream->left());
        }
        std::string str = stream->read_string(len);

        value = str;

        return err;
    }

    srs_error_t srs_amf0_write_utf8(SrsBuffer* stream, string value)
    {
        srs_error_t err = srs_success;

        // len
        if (!stream->require(2)) {
            return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires 2 only %d bytes", stream->left());
        }
        stream->write_2bytes((int16_t)value.length());

        // empty string
        if (value.length() <= 0) {
            return err;
        }

        // data
        if (!stream->require((int)value.length())) {
            return srs_error_new(ERROR_RTMP_AMF0_ENCODE, "requires %d only %d bytes", (int)value.length(), stream->left());
        }
        stream->write_string(value);

        return err;
    }

    bool srs_amf0_is_object_eof(SrsBuffer* stream)
    {
        // detect the object-eof specially
        if (stream->require(3)) {
            int32_t flag = stream->read_3bytes();
            stream->skip(-3);

            return 0x09 == flag;
        }

        return false;
    }

    srs_error_t srs_amf0_write_object_eof(SrsBuffer* stream, SrsAmf0ObjectEOF* value)
    {
        srs_assert(value != NULL);
        return value->write(stream);
    }

    srs_error_t srs_amf0_write_any(SrsBuffer* stream, SrsAmf0Any* value)
    {
        srs_assert(value != NULL);
        return value->write(stream);
    }
}
