#include "value.h"

#include <cstdlib>
#include <cstring>

#include "../../core/fs.h"

static const Value g_null;

const Value& Value::null_value() { return g_null; }

Value::Type Value::type() const {
    if (v_ & 1) return Int;
    if (!v_) return Null;
    if (v_ == 2 || v_ == 6) return Bool;
    switch (kind()) {
        case STR: return Str;
        case ARR:
        case CMD: return Arr;
        case OBJ: return Obj;
        case NUM: return Num;
        case BIG: return Int;
        case BYTES: return Bytes;
        default: return Null;
    }
}

size_t Value::size() const {
    switch (kind()) {
        case STR:
        case ARR:
        case CMD:
        case OBJ:
        case BYTES: return count();
        default: return 0;
    }
}

const Value& Value::operator[](size_t idx) const {
    uint32_t k = kind();
    if ((k != ARR && k != CMD) || idx >= count()) return g_null;
    return reinterpret_cast<const Value*>(node() + (k == CMD ? 2 : 1))[idx];
}

const Value* Value::find(const char* key) const {
    for (const Member* m = begin_members(), *e = end_members(); m != e; ++m) {
        if (std::strcmp(m->name(), key) == 0) return &m->v;
    }
    return nullptr;
}

const Value& Value::operator[](const char* key) const {
    const Value* v = find(key);
    return v ? *v : g_null;
}

static double read_double(const uint32_t* p) {
    double d;
    std::memcpy(&d, p, sizeof(d));
    return d;
}

int Value::as_int(int def) const {
    if (v_ & 1) return v_ >> 1;
    if (v_ == 2 || v_ == 6) return v_ == 6;
    switch (kind()) {
        case BIG: return (int32_t)node()[1];
        case NUM: return (int)read_double(node() + 1);
        case STR: return std::atoi(str());
        default: return def;
    }
}

double Value::num(double def) const {
    if (v_ & 1) return v_ >> 1;
    if (v_ == 2 || v_ == 6) return v_ == 6;
    switch (kind()) {
        case BIG: return (int32_t)node()[1];
        case NUM: return read_double(node() + 1);
        case STR: return std::atof(str());
        default: return def;
    }
}

bool Value::truthy() const {
    if (v_ & 1) return v_ != 1;
    if (!v_ || v_ == 2) return false;
    if (v_ == 6) return true;
    switch (kind()) {
        case BIG: return node()[1] != 0;
        case NUM: return read_double(node() + 1) != 0.0;
        case STR: return count() > 0;
        default: return true;
    }
}

const char* Value::str(const char* def) const { return kind() == STR ? reinterpret_cast<const char*>(node() + 1) : def; }

const Value& Document::root() const { return data_.size() >= 12 ? *reinterpret_cast<const Value*>(data_.data() + 8) : g_null; }

bool Document::load_memory(std::vector<uint8_t>&& data) {
    data_ = std::move(data);
    if (data_.size() < 12 || std::memcmp(data_.data(), "VBN2", 4) != 0) {
        data_.clear();
        return false;
    }
    uint32_t size = 0;
    std::memcpy(&size, data_.data() + 4, 4);
    if (size > data_.size() || size % 4) {
        data_.clear();
        return false;
    }
    data_.resize(size);
    return true;
}

bool Document::load_data(const std::string& rel) {
    std::vector<uint8_t> d;
    if (!fs::read_data(rel, d)) return false;
    return load_memory(std::move(d));
}
