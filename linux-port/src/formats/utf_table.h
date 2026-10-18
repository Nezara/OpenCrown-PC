// CRI @UTF table reader (the column store used by CPK headers/TOCs and ACB/AWB).
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dcp::formats {

struct UtfData { uint32_t offset, size; };  // relative to the table's data area
using UtfValue = std::variant<std::monostate, uint64_t, int64_t, double, std::string, UtfData>;

class UtfTable {
public:
    // Parses a table starting at "@UTF" (or an XOR-obfuscated equivalent). Throws on malformed input.
    explicit UtfTable(std::span<const uint8_t> bytes);

    const std::string& name() const { return name_; }
    uint32_t rows() const { return rows_; }
    size_t columns() const { return cols_.size(); }
    const std::string& column_name(size_t i) const { return cols_[i].name; }
    std::optional<size_t> find_column(std::string_view name) const;

    UtfValue get(uint32_t row, size_t col) const;
    UtfValue get(uint32_t row, std::string_view col) const;

    // Convenience accessors; return fallback when the column is missing or zero-storage.
    uint64_t u64(uint32_t row, std::string_view col, uint64_t fallback = 0) const;
    std::string str(uint32_t row, std::string_view col) const;
    std::span<const uint8_t> data(uint32_t row, std::string_view col) const;

private:
    struct Column {
        std::string name;
        uint8_t flags;
        uint32_t const_pos;  // position of the constant value in the schema (constant storage)
        uint32_t row_pos;    // offset within a row (per-row storage)
    };
    UtfValue decode(uint8_t type, uint32_t pos) const;

    std::vector<uint8_t> t_;  // table body (after the 8-byte "@UTF"+size header), de-obfuscated
    std::string name_;
    uint32_t rows_off_ = 0, strings_off_ = 0, data_off_ = 0, row_len_ = 0, rows_ = 0;
    std::vector<Column> cols_;
};

}  // namespace dcp::formats
