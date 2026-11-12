#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Member;

class Value {
public:
    enum Type : uint8_t { Null, Bool, Int, Num, Str, Arr, Obj, Bytes };

    constexpr Value() = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;

    Type type() const;
    bool is_null() const { return v_ == 0; }
    bool is_arr() const { return kind() == ARR || kind() == CMD; }
    bool is_obj() const { return kind() == OBJ; }
    bool is_str() const { return kind() == STR; }
    bool is_num() const { return (v_ & 1) || kind() == NUM || kind() == BIG; }
    size_t size() const;
    const Value& operator[](size_t idx) const;
    const Value& operator[](int idx) const { return (*this)[(size_t)(idx < 0 ? 0 : idx)]; }
    const Value& operator[](const char* key) const;
    const Value* find(const char* key) const;
    int as_int(int def = 0) const;
    double num(double def = 0) const;
    bool truthy() const;
    const char* str(const char* def = "") const;
    std::string string() const { return std::string(str()); }
    const uint8_t* blob() const { return kind() == BYTES ? reinterpret_cast<const uint8_t*>(node() + 1) : nullptr; }
    int code() const { return kind() == CMD ? (int)(node()[1] & 0xFFFF) : 0; }
    int indent() const { return kind() == CMD ? (int)(node()[1] >> 16) : 0; }
    const Value& params() const { return *this; }
    const Member* begin_members() const;
    const Member* end_members() const;
    static const Value& null_value();

private:
    enum : uint32_t { NONE = 0, STR = 1, ARR = 2, OBJ = 3, NUM = 4, BIG = 5, BYTES = 6, CMD = 7 };
    int32_t v_ = 0;
    bool is_node() const { return v_ != 0 && (v_ & 3) == 0; }
    const uint32_t* node() const { return reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(this) + v_); }
    uint32_t kind() const { return is_node() ? node()[0] & 7 : NONE; }
    uint32_t count() const { return node()[0] >> 3; }
};

struct Member {
    Value key;
    Value v;
    const char* name() const { return key.str(); }
};

inline const Member* Value::begin_members() const { return kind() == OBJ ? reinterpret_cast<const Member*>(node() + 1) : nullptr; }
inline const Member* Value::end_members() const { return kind() == OBJ ? reinterpret_cast<const Member*>(node() + 1) + count() : nullptr; }

class Document {
public:
    bool load_data(const std::string& rel);
    bool load_memory(std::vector<uint8_t>&& data);
    const Value& root() const;
    size_t memory() const { return data_.size(); }

private:
    std::vector<uint8_t> data_;
};
