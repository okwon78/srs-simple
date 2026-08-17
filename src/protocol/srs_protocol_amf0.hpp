// srs_simple — 원본: trunk/src/protocol/srs_protocol_amf0.hpp
// 원본 13타입 중 라이브 경로에 필요한 7타입 서브셋만 유지 (CLAUDE.md §2.4):
// Number/Boolean/String/Object/Null/Undefined/EcmaArray + object-eof.
// StrictArray/Date/LongString/XmlDocument/TypedObject/Reference와
// human_print/to_json은 제거 (CLAUDE.md §5.6).
#ifndef SRS_PROTOCOL_AMF0_HPP
#define SRS_PROTOCOL_AMF0_HPP

#include <srs_core.hpp>

#include <string>
#include <utility>
#include <vector>

class SrsBuffer;
class SrsAmf0Object;
class SrsAmf0EcmaArray;

// internal objects, user should never use it.
namespace srs_internal
{
    class SrsUnSortedHashtable;
    class SrsAmf0ObjectEOF;
}

/*
 Usages:

 1. 스트림에서 임의 타입 읽기:
        SrsAmf0Any* pany = NULL;
        srs_amf0_read_any(&stream, &pany);

 2. 특정 타입 값을 직접 읽기:
        string value;
        srs_amf0_read_string(&stream, value);

 3. 인스턴스에서 값 꺼내기:
        if (any->is_string()) { string str = any->to_str(); }

 4. 복합 객체 만들기:
        SrsAmf0Object* obj = SrsAmf0Any::object();
        obj->set("width", SrsAmf0Any::number(1024));

 5. 직렬화:
        char* bytes = new char[any->total_size()];
        SrsBuffer stream(bytes, any->total_size());
        any->write(&stream);
 */

/**
 * any amf0 value.
 * 2.1 Types Overview
 * value-type = number-type | boolean-type | string-type | object-type
 *         | null-marker | undefined-marker | ecma-array-type
 */
class SrsAmf0Any
{
public:
    char marker;
public:
    SrsAmf0Any();
    virtual ~SrsAmf0Any();
    // type identify, user should identify the type then convert from/to value.
public:
    virtual bool is_string();
    virtual bool is_boolean();
    virtual bool is_number();
    virtual bool is_null();
    virtual bool is_undefined();
    virtual bool is_object();
    virtual bool is_object_eof();
    virtual bool is_ecma_array();
    // whether current instance is an AMF0 object, object-EOF or ecma-array.
    virtual bool is_complex_object();
    // get value of instance
public:
    // get a string copy of instance.
    // @remark assert is_string(), user must ensure the type then convert.
    virtual std::string to_str();
    virtual const char* to_str_raw();
    virtual bool to_boolean();
    virtual double to_number();
    virtual SrsAmf0Object* to_object();
    virtual SrsAmf0EcmaArray* to_ecma_array();
    // set value of instance
public:
    // set the number of any when is_number() indicates true.
    virtual void set_number(double value);
    // serialize/deseriaize instance.
public:
    // get the size of amf0 any, including the marker size.
    virtual int total_size() = 0;
    virtual srs_error_t read(SrsBuffer* stream) = 0;
    virtual srs_error_t write(SrsBuffer* stream) = 0;
    virtual SrsAmf0Any* copy() = 0;
    // create AMF0 instance.
public:
    static SrsAmf0Any* str(const char* value = NULL);
    static SrsAmf0Any* boolean(bool value = false);
    static SrsAmf0Any* number(double value = 0.0);
    static SrsAmf0Any* null();
    static SrsAmf0Any* undefined();
    static SrsAmf0Object* object();
    static SrsAmf0Any* object_eof();
    static SrsAmf0EcmaArray* ecma_array();
    // discovery instance from stream
public:
    /**
     * discovery AMF0 instance from stream
     * @param ppvalue, output the discoveried AMF0 instance. NULL if error.
     * @remark, instance is created without read from stream, user must
     *       use (*ppvalue)->read(stream) to get the instance.
     */
    static srs_error_t discovery(SrsBuffer* stream, SrsAmf0Any** ppvalue);
};

/**
 * 2.5 Object Type
 * anonymous-object-type = object-marker *(object-property)
 * object-property = (UTF-8 value-type) | (UTF-8-empty object-end-marker)
 */
class SrsAmf0Object : public SrsAmf0Any
{
private:
    srs_internal::SrsUnSortedHashtable* properties;
    srs_internal::SrsAmf0ObjectEOF* eof;
private:
    friend class SrsAmf0Any;
    // use SrsAmf0Any::object() to create it.
    SrsAmf0Object();
public:
    virtual ~SrsAmf0Object();
    // serialize/deserialize to/from stream.
public:
    virtual int total_size();
    virtual srs_error_t read(SrsBuffer* stream);
    virtual srs_error_t write(SrsBuffer* stream);
    virtual SrsAmf0Any* copy();
    // properties iteration
public:
    virtual void clear();
    virtual int count();
    // @remark: max index is count().
    virtual std::string key_at(int index);
    virtual const char* key_raw_at(int index);
    virtual SrsAmf0Any* value_at(int index);
    // property set/get.
public:
    // @remark user should never free the value, this instance will manage it.
    virtual void set(std::string key, SrsAmf0Any* value);
    // @return the property AMF0 value, NULL if not found.
    // @remark user should never free the returned value, copy it if needed.
    virtual SrsAmf0Any* get_property(std::string name);
    // get the string property, ensure the property is_string().
    virtual SrsAmf0Any* ensure_property_string(std::string name);
    // get the number property, ensure the property is_number().
    virtual SrsAmf0Any* ensure_property_number(std::string name);
    virtual void remove(std::string name);
};

/**
 * 2.10 ECMA Array Type
 * ecma-array-type = associative-count *(object-property)
 * associative-count = U32
 * object-property = (UTF-8 value-type) | (UTF-8-empty object-end-marker)
 * @remark 서버는 읽기(ffmpeg의 onMetaData)만 필요하지만, 원본대로 write도 유지.
 */
class SrsAmf0EcmaArray : public SrsAmf0Any
{
private:
    srs_internal::SrsUnSortedHashtable* properties;
    srs_internal::SrsAmf0ObjectEOF* eof;
    int32_t _count;
private:
    friend class SrsAmf0Any;
    // use SrsAmf0Any::ecma_array() to create it.
    SrsAmf0EcmaArray();
public:
    virtual ~SrsAmf0EcmaArray();
    // serialize/deserialize to/from stream.
public:
    virtual int total_size();
    virtual srs_error_t read(SrsBuffer* stream);
    virtual srs_error_t write(SrsBuffer* stream);
    virtual SrsAmf0Any* copy();
    // properties iteration
public:
    virtual void clear();
    virtual int count();
    virtual std::string key_at(int index);
    virtual const char* key_raw_at(int index);
    virtual SrsAmf0Any* value_at(int index);
    // property set/get.
public:
    virtual void set(std::string key, SrsAmf0Any* value);
    virtual SrsAmf0Any* get_property(std::string name);
    virtual SrsAmf0Any* ensure_property_string(std::string name);
    virtual SrsAmf0Any* ensure_property_number(std::string name);
};

/**
 * the class to get amf0 object size
 */
class SrsAmf0Size
{
public:
    static int utf8(std::string value);
    static int str(std::string value);
    static int number();
    static int null();
    static int undefined();
    static int boolean();
    static int object(SrsAmf0Object* obj);
    static int object_eof();
    static int ecma_array(SrsAmf0EcmaArray* arr);
    static int any(SrsAmf0Any* o);
};

/**
 * read anything from stream.
 * @param ppvalue, the output amf0 any elem.
 *         NULL if error; otherwise, never NULL and user must free it.
 */
extern srs_error_t srs_amf0_read_any(SrsBuffer* stream, SrsAmf0Any** ppvalue);

/**
 * read amf0 string from stream.
 * 2.4 String Type
 * string-type = string-marker UTF-8
 */
extern srs_error_t srs_amf0_read_string(SrsBuffer* stream, std::string& value);
extern srs_error_t srs_amf0_write_string(SrsBuffer* stream, std::string value);

/**
 * read amf0 boolean from stream.
 * boolean-type = boolean-marker U8
 *         0 is false, <> 0 is true
 */
extern srs_error_t srs_amf0_read_boolean(SrsBuffer* stream, bool& value);
extern srs_error_t srs_amf0_write_boolean(SrsBuffer* stream, bool value);

/**
 * read amf0 number from stream.
 * 2.2 Number Type
 * number-type = number-marker DOUBLE (8B IEEE754, BE)
 */
extern srs_error_t srs_amf0_read_number(SrsBuffer* stream, double& value);
extern srs_error_t srs_amf0_write_number(SrsBuffer* stream, double value);

/**
 * read amf0 null from stream.
 * null-type = null-marker
 */
extern srs_error_t srs_amf0_read_null(SrsBuffer* stream);
extern srs_error_t srs_amf0_write_null(SrsBuffer* stream);

/**
 * read amf0 undefined from stream.
 * undefined-type = undefined-marker
 */
extern srs_error_t srs_amf0_read_undefined(SrsBuffer* stream);
extern srs_error_t srs_amf0_write_undefined(SrsBuffer* stream);

// internal objects, user should never use it.
namespace srs_internal
{
    /**
     * read amf0 string from stream.
     * 2.4 String Type
     * string-type = string-marker UTF-8
     * @return default value is empty string.
     * @remark: use SrsAmf0Any::str() to create it.
     */
    class SrsAmf0String : public SrsAmf0Any
    {
    public:
        std::string value;
    private:
        friend class SrsAmf0Any;
        // use SrsAmf0Any::str() to create it.
        SrsAmf0String(const char* _value);
    public:
        virtual ~SrsAmf0String();
    public:
        virtual int total_size();
        virtual srs_error_t read(SrsBuffer* stream);
        virtual srs_error_t write(SrsBuffer* stream);
        virtual SrsAmf0Any* copy();
    };

    /**
     * read amf0 boolean from stream.
     * boolean-type = boolean-marker U8
     */
    class SrsAmf0Boolean : public SrsAmf0Any
    {
    public:
        bool value;
    private:
        friend class SrsAmf0Any;
        // use SrsAmf0Any::boolean() to create it.
        SrsAmf0Boolean(bool _value);
    public:
        virtual ~SrsAmf0Boolean();
    public:
        virtual int total_size();
        virtual srs_error_t read(SrsBuffer* stream);
        virtual srs_error_t write(SrsBuffer* stream);
        virtual SrsAmf0Any* copy();
    };

    /**
     * read amf0 number from stream.
     * number-type = number-marker DOUBLE
     */
    class SrsAmf0Number : public SrsAmf0Any
    {
    public:
        double value;
    private:
        friend class SrsAmf0Any;
        // use SrsAmf0Any::number() to create it.
        SrsAmf0Number(double _value);
    public:
        virtual ~SrsAmf0Number();
    public:
        virtual int total_size();
        virtual srs_error_t read(SrsBuffer* stream);
        virtual srs_error_t write(SrsBuffer* stream);
        virtual SrsAmf0Any* copy();
    };

    /**
     * read amf0 null from stream.
     * null-type = null-marker
     */
    class SrsAmf0Null : public SrsAmf0Any
    {
    private:
        friend class SrsAmf0Any;
        // use SrsAmf0Any::null() to create it.
        SrsAmf0Null();
    public:
        virtual ~SrsAmf0Null();
    public:
        virtual int total_size();
        virtual srs_error_t read(SrsBuffer* stream);
        virtual srs_error_t write(SrsBuffer* stream);
        virtual SrsAmf0Any* copy();
    };

    /**
     * read amf0 undefined from stream.
     * undefined-type = undefined-marker
     */
    class SrsAmf0Undefined : public SrsAmf0Any
    {
    private:
        friend class SrsAmf0Any;
        // use SrsAmf0Any::undefined() to create it.
        SrsAmf0Undefined();
    public:
        virtual ~SrsAmf0Undefined();
    public:
        virtual int total_size();
        virtual srs_error_t read(SrsBuffer* stream);
        virtual srs_error_t write(SrsBuffer* stream);
        virtual SrsAmf0Any* copy();
    };

    /**
     * to ensure in inserted order.
     * for the FMLE will crash when AMF0Object is not ordered by inserted,
     * if ordered in map, the string compare order, the FMLE will creash when
     * get the response of connect app.
     */
    class SrsUnSortedHashtable
    {
    private:
        typedef std::pair<std::string, SrsAmf0Any*> SrsAmf0ObjectPropertyType;
        std::vector<SrsAmf0ObjectPropertyType> properties;
    public:
        SrsUnSortedHashtable();
        virtual ~SrsUnSortedHashtable();
    public:
        virtual int count();
        virtual void clear();
        virtual std::string key_at(int index);
        virtual const char* key_raw_at(int index);
        virtual SrsAmf0Any* value_at(int index);
        /**
         * set the value of hashtable.
         * @param value, the value to set. NULL to delete the property.
         */
        virtual void set(std::string key, SrsAmf0Any* value);
    public:
        virtual SrsAmf0Any* get_property(std::string name);
        virtual SrsAmf0Any* ensure_property_string(std::string name);
        virtual SrsAmf0Any* ensure_property_number(std::string name);
        virtual void remove(std::string name);
    public:
        virtual void copy(SrsUnSortedHashtable* src);
    };

    /**
     * 2.11 Object End Type
     * object-end-type = UTF-8-empty object-end-marker
     * 0x00 0x00 0x09
     */
    class SrsAmf0ObjectEOF : public SrsAmf0Any
    {
    public:
        SrsAmf0ObjectEOF();
        virtual ~SrsAmf0ObjectEOF();
    public:
        virtual int total_size();
        virtual srs_error_t read(SrsBuffer* stream);
        virtual srs_error_t write(SrsBuffer* stream);
        virtual SrsAmf0Any* copy();
    };

    /**
     * read amf0 utf8 string from stream.
     * 1.3.1 Strings and UTF-8
     * UTF-8 = U16 *(UTF8-char)
     */
    extern srs_error_t srs_amf0_read_utf8(SrsBuffer* stream, std::string& value);
    extern srs_error_t srs_amf0_write_utf8(SrsBuffer* stream, std::string value);

    extern bool srs_amf0_is_object_eof(SrsBuffer* stream);
    extern srs_error_t srs_amf0_write_object_eof(SrsBuffer* stream, SrsAmf0ObjectEOF* value);

    extern srs_error_t srs_amf0_write_any(SrsBuffer* stream, SrsAmf0Any* value);
};

#endif
