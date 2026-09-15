#include "csv/csv.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "markdown/table_format.hpp"

namespace ainiux::csv {
namespace {

void skip_utf8_bom(std::string_view& bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.remove_prefix(3);
    }
}

Error fail(const std::string& detail, std::size_t offset) {
    return {ErrorCode::UnsupportedFeature,
            "CSV parse error at byte " + std::to_string(offset) + ": " + detail};
}

Error parse_quoted_field(std::string_view bytes,
                         std::size_t& pos,
                         const ReadOptions& options,
                         std::string& field) {
    if (pos >= bytes.size() || bytes[pos] != '"') {
        return fail("expected a quoted field", pos);
    }
    ++pos;
    while (pos < bytes.size()) {
        if (options.cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "CSV conversion cancelled"};
        }
        const char ch = bytes[pos];
        if (ch == '"') {
            if (pos + 1 < bytes.size() && bytes[pos + 1] == '"') {
                field.push_back('"');
                pos += 2;
                continue;
            }
            ++pos;
            return ok_error();
        }
        field.push_back(ch);
        ++pos;
    }
    return fail("unclosed quoted field", pos);
}

Error parse_unquoted_field(std::string_view bytes, std::size_t& pos, std::string& field) {
    while (pos < bytes.size()) {
        const char ch = bytes[pos];
        if (ch == ',' || ch == '\n' || ch == '\r') {
            break;
        }
        field.push_back(ch);
        ++pos;
    }
    return ok_error();
}

Error consume_record_end(std::string_view bytes, std::size_t& pos) {
    if (pos >= bytes.size()) {
        return ok_error();
    }
    if (bytes[pos] == '\r') {
        ++pos;
        if (pos < bytes.size() && bytes[pos] == '\n') {
            ++pos;
        }
        return ok_error();
    }
    if (bytes[pos] == '\n') {
        ++pos;
        return ok_error();
    }
    return fail("expected comma or end of record", pos);
}

bool row_is_single_empty_field(const std::vector<std::string>& row) {
    return row.size() == 1 && row[0].empty();
}

Error parse_rows(std::string_view bytes,
                 const ReadOptions& options,
                 std::vector<std::vector<std::string>>& rows) {
    rows.clear();
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        if (options.cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "CSV conversion cancelled"};
        }
        std::vector<std::string> row;
        while (true) {
            std::string field;
            Error err;
            if (pos < bytes.size() && bytes[pos] == '"') {
                err = parse_quoted_field(bytes, pos, options, field);
            } else {
                err = parse_unquoted_field(bytes, pos, field);
            }
            if (!err.ok()) {
                return err;
            }
            row.push_back(std::move(field));
            if (pos < bytes.size() && bytes[pos] == ',') {
                ++pos;
                continue;
            }
            break;
        }
        const bool at_end = pos >= bytes.size();
        Error err = consume_record_end(bytes, pos);
        if (!err.ok()) {
            return err;
        }
        // A terminating newline after the last record is not another row.
        if (at_end && row_is_single_empty_field(row) && !rows.empty()) {
            break;
        }
        rows.push_back(std::move(row));
    }
    return ok_error();
}

void escape_gfm_cell(std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '|') {
            out += "\\|";
        } else if (ch == '\n' || ch == '\r') {
            out += "<br>";
        } else {
            out.push_back(ch);
        }
    }
    text = std::move(out);
}

void escape_row(std::vector<std::string>& row) {
    for (std::string& cell : row) {
        escape_gfm_cell(cell);
    }
}

}  // namespace

Error to_markdown_bytes(std::string_view bytes,
                        const ReadOptions& options,
                        std::string& markdown,
                        Diagnostics* diagnostics) {
    markdown.clear();
    if (diagnostics != nullptr) {
        *diagnostics = {};
    }
    if (options.max_bytes == 0) {
        return {ErrorCode::BadArgs, "CSV conversion max_bytes must be greater than zero"};
    }
    if (bytes.size() > options.max_bytes) {
        return {ErrorCode::UnsupportedFeature,
                "CSV exceeds max_bytes limit of " + std::to_string(options.max_bytes)};
    }
    skip_utf8_bom(bytes);
    if (bytes.empty()) {
        return {ErrorCode::UnsupportedFeature, "CSV input is empty"};
    }
    if (options.cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "CSV conversion cancelled"};
    }

    std::vector<std::vector<std::string>> rows;
    Error err = parse_rows(bytes, options, rows);
    if (!err.ok()) {
        return err;
    }
    if (rows.empty() || (rows.size() == 1 && rows[0].size() == 1 && rows[0][0].empty())) {
        return {ErrorCode::UnsupportedFeature, "CSV input has no fields"};
    }

    std::size_t columns = 0;
    for (const auto& row : rows) {
        columns = std::max(columns, row.size());
    }
    if (columns == 0) {
        return {ErrorCode::UnsupportedFeature, "CSV input has no fields"};
    }

    std::size_t ragged = 0;
    const std::size_t header_width = rows.front().size();
    for (const auto& row : rows) {
        if (row.size() != header_width) {
            ++ragged;
        }
    }
    if (ragged > 0 && diagnostics != nullptr) {
        diagnostics->ragged_rows = ragged;
        diagnostics->messages.push_back(
            "CSV rows have mixed column counts; shorter rows were padded");
    }

    escape_row(rows.front());
    std::vector<std::string> headers = std::move(rows.front());
    std::vector<std::vector<std::string>> body;
    body.reserve(rows.size() > 0 ? rows.size() - 1 : 0);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        escape_row(rows[i]);
        body.push_back(std::move(rows[i]));
    }

    try {
        markdown = markdown::format_table(headers, {}, body, markdown::TableStyle::PaddedGfm);
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to format CSV as Markdown"};
    } catch (const std::length_error&) {
        return {ErrorCode::UnsupportedFeature, "converted CSV Markdown is too large"};
    }
    if (markdown.empty()) {
        return {ErrorCode::UnsupportedFeature, "CSV input has no fields"};
    }
    return ok_error();
}

}  // namespace ainiux::csv
