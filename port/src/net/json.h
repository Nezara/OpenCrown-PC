// Minimal JSON value for the lobby protocol (notes/map/lobby_protocol.md): parse + serialize, no exceptions (the HLE
// side is built with -fno-exceptions). Numbers keep an exact int64 when integral; 64-bit ids travel as strings.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dcp::net {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : t_(Type::Bool), b_(b) {}
    Json(int v) : t_(Type::Number), i_(v), d_(v) {}
    Json(int64_t v) : t_(Type::Number), i_(v), d_(double(v)) {}
    Json(uint32_t v) : t_(Type::Number), i_(v), d_(v) {}
    Json(double v) : t_(Type::Number), i_(int64_t(v)), d_(v) {}
    Json(const char* s) : t_(Type::String), s_(s) {}
    Json(std::string s) : t_(Type::String), s_(std::move(s)) {}
    static Json array() { Json j; j.t_ = Type::Array; return j; }
    static Json object() { Json j; j.t_ = Type::Object; return j; }

    Type type() const { return t_; }
    bool is_null() const { return t_ == Type::Null; }
    bool is_object() const { return t_ == Type::Object; }
    bool is_array() const { return t_ == Type::Array; }
    bool is_string() const { return t_ == Type::String; }
    bool is_number() const { return t_ == Type::Number; }

    // Accessors with defaults (wrong type -> default).
    bool as_bool(bool def = false) const { return t_ == Type::Bool ? b_ : def; }
    int64_t as_int(int64_t def = 0) const { return t_ == Type::Number ? i_ : def; }
    double as_double(double def = 0) const { return t_ == Type::Number ? d_ : def; }
    const std::string& as_string() const;
    // u64 carried as a decimal (or 0x-hex) string, or as a number when small.
    uint64_t as_u64(uint64_t def = 0) const;

    // Object access. operator[] on a non-object turns it into an object.
    Json& operator[](const std::string& k);
    const Json& get(const std::string& k) const;  // null if missing
    bool has(const std::string& k) const;
    const std::map<std::string, Json>& items() const { return o_; }

    // Array access.
    void push(Json v);
    size_t size() const { return t_ == Type::Array ? a_.size() : t_ == Type::Object ? o_.size() : 0; }
    const Json& at(size_t i) const;
    const std::vector<Json>& elems() const { return a_; }

    std::string dump() const;
    // Returns false on syntax error (out is then null).
    static bool parse(const std::string& text, Json* out);

private:
    void dump_to(std::string& o) const;
    Type t_ = Type::Null;
    bool b_ = false;
    int64_t i_ = 0;
    double d_ = 0;
    std::string s_;
    std::vector<Json> a_;
    std::map<std::string, Json> o_;
};

std::string u64_str(uint64_t v);                      // decimal
std::string hex_encode(const uint8_t* p, size_t n);   // lower-case
bool hex_decode(const std::string& s, std::vector<uint8_t>* out);

}  // namespace dcp::net
