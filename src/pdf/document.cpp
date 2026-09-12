#include "pdf/document.hpp"

#include "pdf/limits.hpp"
#include "pdf/stream.hpp"
#include "pdf/token.hpp"
#include "platform/filesystem.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ainiux::pdf {
namespace {

constexpr std::uint8_t kParsing = 1;
constexpr std::uint8_t kExpanding = 2;
constexpr std::uint8_t kExpanded = 4;
constexpr std::uint8_t kPageVisited = 8;

// Own the active flag and nesting count on every success/error/cancellation path.
struct LoadGuard {
    std::uint8_t& state;
    std::size_t& depth;
    std::uint8_t flag;
    LoadGuard(Object& object, std::size_t& nesting, std::uint8_t bit)
        : state(object.state), depth(nesting), flag(bit) {
        state |= flag;
        ++depth;
    }
    ~LoadGuard() {
        state &= ~flag;
        --depth;
    }
    LoadGuard(const LoadGuard&) = delete;
    LoadGuard& operator=(const LoadGuard&) = delete;
};

bool bounded_integer(const Value& value, std::int64_t limit, std::int64_t& out) {
    return value.type == ValueType::Number && number_to_integer(value.number, out) && out >= 0 && out <= limit;
}

bool starts_with_header(const std::string& bytes, std::string& version) {
    if (bytes.size() < 8) {
        return false;
    }
    const char* data = bytes.data();
    if (std::memcmp(data, "%PDF-1.", 7) != 0 && std::memcmp(data, "%PDF-2.", 7) != 0) {
        return false;
    }
    if (!pdf_isdigit(static_cast<unsigned char>(data[7]))) {
        return false;
    }
    version.assign(data + 5, 3);
    return true;
}

std::size_t find_startxref(const std::string& bytes) {
    if (bytes.size() < 9) {
        return std::string::npos;
    }
    const std::size_t begin = bytes.size() > kStartxrefWindow ? bytes.size() - kStartxrefWindow : 0;
    std::size_t found = std::string::npos;
    for (std::size_t i = begin; i + 9 <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, "startxref", 9) != 0) {
            continue;
        }
        std::size_t j = i + 9;
        while (j < bytes.size() && pdf_isspace(static_cast<unsigned char>(bytes[j]))) {
            ++j;
        }
        if (j >= bytes.size() || !pdf_isdigit(static_cast<unsigned char>(bytes[j]))) {
            continue;
        }
        std::size_t offset = 0;
        while (j < bytes.size() && pdf_isdigit(static_cast<unsigned char>(bytes[j]))) {
            if (offset > (bytes.size() - static_cast<std::size_t>(bytes[j] - '0')) / 10) {
                offset = bytes.size();
                break;
            }
            offset = offset * 10 + static_cast<std::size_t>(bytes[j] - '0');
            ++j;
        }
        if (offset > 0 && offset < bytes.size()) {
            found = offset;
        }
    }
    return found;
}

void skip_stream_newline(Cursor& cursor) {
    const int ch = cursor.get();
    if (ch == '\r' && cursor.peek() == '\n') {
        cursor.get();
    } else if (ch != '\n' && ch != '\r' && ch != -1) {
        cursor.unget();
    }
}

std::size_t scan_endstream(const std::string& bytes, std::size_t start) {
    const char* marker = "endstream";
    const std::size_t marker_len = 9;
    const std::size_t last = bytes.size() < start ? start : bytes.size();
    for (std::size_t i = start; i + marker_len <= last; ++i) {
        if (std::memcmp(bytes.data() + i, marker, marker_len) == 0) {
            std::size_t end = i;
            if (end > start && bytes[end - 1] == '\n') {
                --end;
            }
            if (end > start && bytes[end - 1] == '\r') {
                --end;
            }
            return end - start;
        }
    }
    return 0;
}

bool parse_obj_header(const std::string& line, std::uint32_t& number, std::uint16_t& generation) {
    std::size_t i = 0;
    while (i < line.size() && pdf_isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    if (i >= line.size() || !pdf_isdigit(static_cast<unsigned char>(line[i]))) {
        return false;
    }
    std::uint64_t n = 0;
    while (i < line.size() && pdf_isdigit(static_cast<unsigned char>(line[i]))) {
        n = n * 10 + static_cast<std::uint64_t>(line[i] - '0');
        ++i;
        if (n > kMaxObjects) {
            return false;
        }
    }
    while (i < line.size() && pdf_isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    if (i >= line.size() || !pdf_isdigit(static_cast<unsigned char>(line[i]))) {
        return false;
    }
    std::uint64_t g = 0;
    while (i < line.size() && pdf_isdigit(static_cast<unsigned char>(line[i]))) {
        g = g * 10 + static_cast<std::uint64_t>(line[i] - '0');
        ++i;
        if (g > 65535) {
            return false;
        }
    }
    while (i < line.size() && pdf_isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    if (i + 3 > line.size() || line.compare(i, 3, "obj") != 0) {
        return false;
    }
    if (n < 1) {
        return false;
    }
    number = static_cast<std::uint32_t>(n);
    generation = static_cast<std::uint16_t>(g);
    return true;
}

std::uint64_t read_be(const std::uint8_t* data, int width) {
    std::uint64_t value = 0;
    for (int i = 0; i < width; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

}  // namespace

bool Document::cancelled(const Options& options) const {
    return options.cancellation.cancelled();
}

Error Document::open_file(const std::string& path, const Options& options, Document& out) {
    if (path.empty()) {
        return {ErrorCode::BadArgs, "PDF path is empty"};
    }
    const std::size_t limit = options.max_bytes == 0 ? kDefaultMaxBytes : options.max_bytes;
    std::string bytes;
    Error err = platform::read_file_bounded(expand_user_path(path), limit, bytes);
    if (!err.ok()) {
        return err;
    }
    return open_bytes(std::move(bytes), options, out);
}

Error Document::open_bytes(std::string bytes, const Options& options, Document& out) {
    out = Document{};
    if (bytes.size() > (options.max_bytes == 0 ? kDefaultMaxBytes : options.max_bytes)) {
        return {ErrorCode::FileRead, "PDF input exceeds size limit"};
    }
    out.cancellation_ = options.cancellation;
    out.bytes_ = std::move(bytes);
    return out.open_loaded(options);
}

Error Document::open_loaded(const Options& options) {
    if (cancelled(options)) {
        return {ErrorCode::Cancelled, "PDF open cancelled"};
    }
    if (!starts_with_header(bytes_, version_)) {
        return {ErrorCode::FileRead, "Bad PDF header"};
    }
    const std::size_t xref = find_startxref(bytes_);
    Error err;
    if (xref == std::string::npos) {
        err = repair_xref(options);
    } else {
        err = load_xref(xref, options);
        if (!err.ok() && err.code != ErrorCode::Cancelled && err.code != ErrorCode::UnsupportedFeature) {
            err = repair_xref(options);
        }
    }
    if (!err.ok()) {
        return err;
    }
    if (dict_get(trailer_, "Encrypt") != nullptr) {
        return {ErrorCode::UnsupportedFeature, "encrypted PDFs are not supported yet"};
    }
    Ref root;
    if (!dict_ref(trailer_, "Root", root)) {
        return {ErrorCode::FileRead, "Missing Root object"};
    }
    Error load = load_object(root.number);
    if (!load.ok()) {
        return load;
    }
    const Value* catalog = object_dict(root.number);
    if (catalog == nullptr) {
        return {ErrorCode::FileRead, "Root object is not a dictionary"};
    }
    Ref pages;
    if (!dict_ref(*catalog, "Pages", pages)) {
        return {ErrorCode::FileRead, "Missing Pages object"};
    }
    return load_pages(pages.number, 0, options);
}

bool Document::add_placeholder(std::uint32_t number, std::uint16_t generation, std::size_t offset, bool compressed,
                               std::uint32_t objstm_number) {
    if (number == 0 || number > kMaxObjects) {
        return false;
    }
    const auto block = number / kObjectBlockSize;
    while (object_blocks_.size() <= block) {
        object_blocks_.push_back(std::make_unique<std::array<Object, kObjectBlockSize>>());
    }
    object_slots_ = std::max(object_slots_, static_cast<std::size_t>(number) + 1);
    Object& object = (*object_blocks_[block])[number % kObjectBlockSize];
    if (object.number == number && generation < object.generation) {
        return true;
    }
    object.number = number;
    object.generation = generation;
    object.offset = compressed ? 0 : offset;
    object.compressed = compressed;
    object.objstm_number = objstm_number;
    object.loaded = false;
    object.state = 0;
    object.value = Value{};
    object.stream_offset = 0;
    object.stream_length = 0;
    return true;
}

Object* Document::find_object(std::uint32_t number) {
    if (number == 0 || number >= object_slots_) {
        return nullptr;
    }
    Object& object = (*object_blocks_[number / kObjectBlockSize])[number % kObjectBlockSize];
    return object.number == number ? &object : nullptr;
}

Error Document::parse_object_at(std::size_t offset, Object& object) {
    if ((object.state & kParsing) || load_depth_ >= kMaxDepth) {
        return {ErrorCode::FileRead, "Recursive or excessively nested PDF object " + std::to_string(object.number)};
    }
    if (cancellation_.cancelled()) {
        return {ErrorCode::Cancelled, "PDF object load cancelled"};
    }
    LoadGuard guard(object, load_depth_, kParsing);
    if (offset >= bytes_.size()) {
        return {ErrorCode::FileRead, "Unable to seek to object " + std::to_string(object.number)};
    }
    Tokenizer tokens(Cursor{reinterpret_cast<const std::uint8_t*>(bytes_.data()), bytes_.size(), offset});
    Token n;
    Token g;
    Token kw;
    Error err = tokens.next(n);
    if (!err.ok()) {
        return err;
    }
    err = tokens.next(g);
    if (!err.ok()) {
        return err;
    }
    err = tokens.next(kw);
    if (!err.ok()) {
        return err;
    }
    if (n.kind != TokenKind::Number || g.kind != TokenKind::Number || kw.kind != TokenKind::Keyword ||
        kw.text != "obj") {
        return {ErrorCode::FileRead, "Bad header for object " + std::to_string(object.number)};
    }
    err = read_value(tokens, object.value, 0);
    if (!err.ok()) {
        return {ErrorCode::FileRead, "Unable to read value for object " + std::to_string(object.number)};
    }
    Token next;
    err = tokens.next(next);
    if (!err.ok()) {
        return err;
    }
    if (next.kind == TokenKind::Keyword && next.text == "stream") {
        skip_stream_newline(tokens.cursor());
        object.stream_offset = tokens.cursor().tell();
        const Value* length_value = dict_get(object.value, "Length");
        if (length_value != nullptr && length_value->type == ValueType::Ref) {
            const auto length_number = length_value->ref.number;
            Error loaded = load_object(length_number);
            if (!loaded.ok()) {
                return loaded;
            }
            length_value = object_value(length_number);
        }
        if (length_value == nullptr) {
            // Keep repair support for streams with an omitted Length only.
            object.stream_length = scan_endstream(bytes_, object.stream_offset);
        } else {
            std::int64_t length = 0;
            if (length_value->type != ValueType::Number ||
                !number_to_integer(length_value->number, length) || length < 0 ||
                static_cast<std::uint64_t>(length) > bytes_.size() - object.stream_offset) {
                return {ErrorCode::FileRead, "Invalid stream Length for PDF object " + std::to_string(object.number)};
            }
            object.stream_length = static_cast<std::size_t>(length);
        }
        if (object.stream_length > bytes_.size() - object.stream_offset) {
            return {ErrorCode::FileRead, "No stream data for object " + std::to_string(object.number)};
        }
    } else {
        tokens.push(std::move(next));
    }
    object.loaded = true;
    return ok_error();
}

Error Document::load_object(std::uint32_t number) {
    Object* object = find_object(number);
    if (object == nullptr) {
        return {ErrorCode::FileRead, "missing PDF object " + std::to_string(number)};
    }
    if (object->loaded) {
        return ok_error();
    }
    if (object->compressed) {
        Options options;
        options.cancellation = cancellation_;
        Error err = load_obj_stream(object->objstm_number, options);
        if (!err.ok()) {
            return err;
        }
        return object->loaded ? ok_error() : Error{ErrorCode::FileRead, "Missing object in PDF object stream"};
    }
    return parse_object_at(object->offset, *object);
}

const Value* Document::object_dict(std::uint32_t number) {
    const Value* value = object_value(number);
    if (value == nullptr || value->type != ValueType::Dict) {
        return nullptr;
    }
    return value;
}

const Value* Document::object_value(std::uint32_t number) {
    Object* object = find_object(number);
    if (object == nullptr) {
        return nullptr;
    }
    if (!object->loaded) {
        Error err = load_object(number);
        if (!err.ok()) {
            return nullptr;
        }
    }
    return &object->value;
}

const Value* Document::page_dictionary(std::size_t index) {
    if (index >= pages_.size()) {
        return nullptr;
    }
    return object_dict(pages_[index]);
}

const Value* Document::page_inherited(std::size_t index, const char* key) {
    const Value* page = page_dictionary(index);
    int depth = 0;
    while (page != nullptr && depth < static_cast<int>(kMaxDepth)) {
        const Value* found = dict_get(*page, key);
        if (found != nullptr) {
            return found;
        }
        Ref parent;
        if (!dict_ref(*page, "Parent", parent)) {
            break;
        }
        page = object_dict(parent.number);
        ++depth;
    }
    return nullptr;
}

Error Document::decode_stream(std::uint32_t number, std::string& decoded) {
    Error err = load_object(number);
    if (!err.ok()) {
        return err;
    }
    Object* object = find_object(number);
    if (object == nullptr) {
        return {ErrorCode::FileRead, "missing PDF object " + std::to_string(number)};
    }
    return decode_object_stream(*object, decoded);
}

Error Document::decode_object_stream(Object& object, std::string& decoded) {
    if (!object.loaded) {
        Error err = parse_object_at(object.offset, object);
        if (!err.ok()) {
            return err;
        }
    }
    if (object.stream_offset == 0) {
        return {ErrorCode::FileRead, "object " + std::to_string(object.number) + " has no stream"};
    }
    if (object.stream_offset > bytes_.size() || object.stream_length > bytes_.size() - object.stream_offset) {
        return {ErrorCode::FileRead, "PDF stream exceeds input bounds"};
    }
    const std::string_view raw(bytes_.data() + object.stream_offset, object.stream_length);
    return decode_stream_bytes(raw, object.value, decoded, kMaxDecodedStream);
}

Error Document::load_xref_stream(std::size_t object_offset, const Options& options, Value& this_trailer) {
    Object xref_obj;
    Error err = parse_object_at(object_offset, xref_obj);
    if (!err.ok()) {
        return err;
    }
    if (xref_obj.value.type != ValueType::Dict) {
        return {ErrorCode::FileRead, "expected dictionary for cross-reference stream"};
    }
    std::string decoded;
    err = decode_object_stream(xref_obj, decoded);
    if (!err.ok()) {
        return err;
    }
    const Value* w = dict_array(xref_obj.value, "W");
    if (w == nullptr || w->array.size() < 2 || w->array.size() > 3) {
        return {ErrorCode::FileRead, "Missing or bad W array in cross-reference stream"};
    }
    int widths[3] = {0, 0, 0};
    for (std::size_t i = 0; i < w->array.size(); ++i) {
        if (w->array[i].type != ValueType::Number) {
            return {ErrorCode::FileRead, "Bad W array in cross-reference stream"};
        }
        std::int64_t width = 0;
        if (!bounded_integer(w->array[i], 8, width)) {
            return {ErrorCode::FileRead, "Bad W array in cross-reference stream"};
        }
        widths[i] = static_cast<int>(width);
    }
    if (widths[1] == 0) {
        return {ErrorCode::FileRead, "Bad W array in cross-reference stream"};
    }
    const int row = widths[0] + widths[1] + widths[2];
    std::vector<std::pair<std::uint32_t, std::uint32_t>> index;
    const Value* index_arr = dict_array(xref_obj.value, "Index");
    if (index_arr != nullptr) {
        if (index_arr->array.empty() || index_arr->array.size() % 2 != 0) {
            return {ErrorCode::FileRead, "Bad Index array in cross-reference stream"};
        }
        for (std::size_t i = 0; i + 1 < index_arr->array.size(); i += 2) {
            std::int64_t start = 0;
            std::int64_t count = 0;
            if (!bounded_integer(index_arr->array[i], kMaxObjects, start) ||
                !bounded_integer(index_arr->array[i + 1], kMaxObjects + 1 - start, count)) {
                return {ErrorCode::FileRead, "Bad Index range in cross-reference stream"};
            }
            index.emplace_back(static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(count));
        }
    }
    if (index.empty()) {
        std::int64_t size = 0;
        if (!dict_int(xref_obj.value, "Size", size) || size < 1 ||
            static_cast<std::uint64_t>(size) > kMaxObjects + 1) {
            return {ErrorCode::FileRead, "Bad Size in cross-reference stream"};
        }
        index.emplace_back(0, static_cast<std::uint32_t>(size));
    }
    std::size_t cursor = 0;
    for (const auto& span : index) {
        if (cancelled(options)) {
            return {ErrorCode::Cancelled, "PDF xref load cancelled"};
        }
        std::uint32_t number = span.first;
        for (std::uint32_t n = 0; n < span.second; ++n, ++number) {
            if (static_cast<std::size_t>(row) > decoded.size() - cursor) {
                return {ErrorCode::FileRead, "Truncated cross-reference stream"};
            }
            const auto* field = reinterpret_cast<const std::uint8_t*>(decoded.data() + cursor);
            cursor += static_cast<std::size_t>(row);
            std::uint64_t type = 1;
            if (widths[0] > 0) {
                type = read_be(field, widths[0]);
            }
            const std::uint64_t field1 = read_be(field + widths[0], widths[1]);
            const std::uint64_t field2 = widths[2] > 0 ? read_be(field + widths[0] + widths[1], widths[2]) : 0;
            if (type == 0) {
                continue;
            }
            if (type == 2) {
                if (field1 == 0 || field1 > kMaxObjects || field2 >= kMaxObjStmObjects) {
                    return {ErrorCode::FileRead, "Invalid compressed cross-reference entry"};
                }
                if (!add_placeholder(number, 0, 0, true, static_cast<std::uint32_t>(field1))) {
                    return {ErrorCode::FileRead, "Too many compressed objects"};
                }
                if (std::find(obj_streams_.begin(), obj_streams_.end(), static_cast<std::uint32_t>(field1)) ==
                    obj_streams_.end()) {
                    if (obj_streams_.size() >= kMaxObjStreams) {
                        return {ErrorCode::FileRead, "Too many object streams"};
                    }
                    obj_streams_.push_back(static_cast<std::uint32_t>(field1));
                }
            } else if (type == 1) {
                if (field1 >= bytes_.size() || field2 > UINT16_MAX) {
                    return {ErrorCode::FileRead, "Invalid cross-reference offset or generation"};
                }
                if (!add_placeholder(number, static_cast<std::uint16_t>(field2), static_cast<std::size_t>(field1), false,
                                     0)) {
                    return {ErrorCode::FileRead, "Too many PDF objects"};
                }
            }
        }
    }
    this_trailer = xref_obj.value;
    return ok_error();
}

Error Document::load_xref_table(Tokenizer& tokens, const Options& options, Value& this_trailer) {
    for (;;) {
        if (cancelled(options)) {
            return {ErrorCode::Cancelled, "PDF xref load cancelled"};
        }
        Token token;
        Error err = tokens.next(token);
        if (!err.ok()) {
            return err;
        }
        if (token.kind == TokenKind::Keyword && token.text == "trailer") {
            return read_value(tokens, this_trailer, 0);
        }
        if (token.kind != TokenKind::Number) {
            return {ErrorCode::FileRead, "invalid xref table"};
        }
        Token count_tok;
        err = tokens.next(count_tok);
        if (!err.ok()) {
            return err;
        }
        if (count_tok.kind != TokenKind::Number) {
            return {ErrorCode::FileRead, "invalid xref subsection"};
        }
        std::uint64_t start = 0;
        std::uint64_t count = 0;
        if (!parse_unsigned_integer(token.text, kMaxObjects, start) ||
            !parse_unsigned_integer(count_tok.text, kMaxObjects + 1 - start, count)) {
            return {ErrorCode::FileRead, "invalid xref subsection range"};
        }
        Cursor& cursor = tokens.cursor();
        while (pdf_isspace(cursor.peek())) {
            cursor.get();
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (cursor.tell() > cursor.size || 20 > cursor.size - cursor.tell()) {
                return {ErrorCode::FileRead, "truncated xref table"};
            }
            char line[21];
            std::memcpy(line, cursor.data + cursor.tell(), 20);
            line[20] = '\0';
            cursor.pos += 20;
            if (std::memcmp(line + 18, "\r\n", 2) != 0 && std::memcmp(line + 18, "\r\r", 2) != 0 &&
                std::memcmp(line + 18, " \n", 2) != 0 && std::memcmp(line + 18, " \r", 2) != 0) {
                return {ErrorCode::FileRead, "bad xref entry end-of-line"};
            }
            const char type = line[17];
            if (type == 'f') {
                continue;
            }
            if (type != 'n') {
                return {ErrorCode::FileRead, "bad xref entry type"};
            }
            line[10] = '\0';
            line[16] = '\0';
            std::uint64_t offset = 0;
            std::uint64_t generation = 0;
            if (!parse_unsigned_integer(std::string_view(line, 10), bytes_.size() - 1, offset) ||
                !parse_unsigned_integer(std::string_view(line + 11, 5), UINT16_MAX, generation)) {
                return {ErrorCode::FileRead, "invalid xref offset or generation"};
            }
            if (!add_placeholder(static_cast<std::uint32_t>(start + i), static_cast<std::uint16_t>(generation),
                                 static_cast<std::size_t>(offset), false, 0)) {
                return {ErrorCode::FileRead, "Too many PDF objects"};
            }
        }
    }
}

Error Document::load_xref(std::size_t xref_offset, const Options& options) {
    std::size_t current = xref_offset;
    std::vector<std::size_t> seen;
    for (std::size_t chain = 0; chain < kMaxXrefPrev; ++chain) {
        if (cancelled(options)) {
            return {ErrorCode::Cancelled, "PDF xref load cancelled"};
        }
        if (std::find(seen.begin(), seen.end(), current) != seen.end()) {
            return {ErrorCode::FileRead, "Recursive xref table"};
        }
        seen.push_back(current);
        if (current >= bytes_.size()) {
            return {ErrorCode::FileRead, "Unable to seek to xref"};
        }
        Tokenizer tokens(Cursor{reinterpret_cast<const std::uint8_t*>(bytes_.data()), bytes_.size(), current});
        Token first;
        Error err = tokens.next(first);
        if (!err.ok()) {
            return err;
        }
        Value this_trailer;
        if (first.kind == TokenKind::Number) {
            tokens.push(std::move(first));
            err = load_xref_stream(current, options, this_trailer);
        } else if (first.kind == TokenKind::Keyword && first.text == "xref") {
            err = load_xref_table(tokens, options, this_trailer);
        } else {
            return {ErrorCode::FileRead, "invalid xref"};
        }
        if (!err.ok()) {
            return err;
        }
        if (trailer_.type != ValueType::Dict) {
            trailer_ = this_trailer;
        }
        std::int64_t prev_number = 0;
        if (dict_get(this_trailer, "Prev") != nullptr) {
            if (!dict_int(this_trailer, "Prev", prev_number) || prev_number < 0 ||
                static_cast<std::uint64_t>(prev_number) >= bytes_.size()) {
                return {ErrorCode::FileRead, "Invalid previous xref offset"};
            }
        }
        if (prev_number > 0) {
            current = static_cast<std::size_t>(prev_number);
            continue;
        }
        break;
    }
    for (std::uint32_t number : obj_streams_) {
        Error err = load_obj_stream(number, options);
        if (!err.ok()) {
            return err;
        }
    }
    return ok_error();
}

Error Document::load_obj_stream(std::uint32_t number, const Options& options) {
    if (cancelled(options)) {
        return {ErrorCode::Cancelled, "PDF object stream load cancelled"};
    }
    Object* stream = find_object(number);
    if (stream == nullptr) {
        return {ErrorCode::FileRead, "Unable to find compressed object stream " + std::to_string(number)};
    }
    if (stream->state & kExpanded) {
        return ok_error();
    }
    if (stream->compressed || (stream->state & (kParsing | kExpanding)) || load_depth_ >= kMaxDepth) {
        return {ErrorCode::FileRead, "Recursive or excessively nested PDF object stream"};
    }
    LoadGuard guard(*stream, load_depth_, kExpanding);
    if (!stream->loaded) {
        Error err = parse_object_at(stream->offset, *stream);
        if (!err.ok()) {
            return err;
        }
    }
    std::string decoded;
    Error err = decode_object_stream(*stream, decoded);
    if (!err.ok()) {
        return err;
    }
    std::int64_t count = 0;
    std::int64_t first = 0;
    if (!dict_int(stream->value, "N", count) || count < 0 ||
        static_cast<std::uint64_t>(count) > kMaxObjStmObjects ||
        !dict_int(stream->value, "First", first) || first < 0 ||
        static_cast<std::uint64_t>(first) > decoded.size()) {
        return {ErrorCode::FileRead, "Invalid PDF object stream N or First"};
    }
    const auto values_at = static_cast<std::size_t>(first);
    Tokenizer header(Cursor{reinterpret_cast<const std::uint8_t*>(decoded.data()), values_at, 0});
    std::vector<std::pair<std::uint32_t, std::size_t>> entries;
    entries.reserve(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        Token n;
        Token off;
        err = header.next(n);
        if (!err.ok()) {
            return err;
        }
        err = header.next(off);
        if (!err.ok()) {
            return err;
        }
        std::uint64_t obj_number = 0;
        std::uint64_t offset = 0;
        if (n.kind != TokenKind::Number || off.kind != TokenKind::Number ||
            !parse_unsigned_integer(n.text, kMaxObjects, obj_number) || obj_number == 0 || obj_number == number ||
            !parse_unsigned_integer(off.text, decoded.size() - values_at, offset) ||
            (!entries.empty() && offset <= entries.back().second)) {
            return {ErrorCode::FileRead, "Invalid PDF object stream header"};
        }
        entries.emplace_back(static_cast<std::uint32_t>(obj_number), static_cast<std::size_t>(offset));
    }
    Tokenizer values;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (cancelled(options)) {
            return {ErrorCode::Cancelled, "PDF object stream load cancelled"};
        }
        const auto obj_number = entries[i].first;
        Object* object = find_object(obj_number);
        // Never replace a live dictionary (or a newer uncompressed revision).
        if (object != nullptr && (object->loaded || !object->compressed || object->objstm_number != number)) {
            continue;
        }
        if (object == nullptr) {
            if (!add_placeholder(obj_number, 0, 0, true, number)) {
                return {ErrorCode::FileRead, "Too many compressed objects"};
            }
            object = find_object(obj_number);
        }
        const auto end = i + 1 < entries.size() ? values_at + entries[i + 1].second : decoded.size();
        values.reset(Cursor{reinterpret_cast<const std::uint8_t*>(decoded.data()), end,
                            values_at + entries[i].second});
        err = read_value(values, object->value, 0);
        if (!err.ok()) {
            return err;
        }
        object->loaded = true;
        object->compressed = true;
        object->objstm_number = number;
    }
    stream->state |= kExpanded;
    return ok_error();
}

Error Document::repair_xref(const Options& options) {
    object_blocks_.clear();
    object_slots_ = 0;
    obj_streams_.clear();
    trailer_ = Value{};
    std::size_t i = 0;
    while (i < bytes_.size()) {
        if (cancelled(options)) {
            return {ErrorCode::Cancelled, "PDF repair cancelled"};
        }
        if (!pdf_isdigit(static_cast<unsigned char>(bytes_[i])) || bytes_[i] == '0') {
            ++i;
            continue;
        }
        const std::size_t start = i;
        std::size_t end = i;
        while (end < bytes_.size() && end - start < 80 && bytes_[end] != '\n') {
            ++end;
        }
        std::uint32_t number = 0;
        std::uint16_t generation = 0;
        if (parse_obj_header(bytes_.substr(start, end - start), number, generation)) {
            if (!add_placeholder(number, generation, start, false, 0)) {
                return {ErrorCode::FileRead, "Too many PDF objects"};
            }
            Object* object = find_object(number);
            if (object != nullptr) {
                parse_object_at(start, *object);
            }
            i = end;
            continue;
        }
        ++i;
    }
    for (std::size_t number = 1; number < object_slots_; ++number) {
        const Object& object = (*object_blocks_[number / kObjectBlockSize])[number % kObjectBlockSize];
        if (!object.loaded || object.value.type != ValueType::Dict) {
            continue;
        }
        std::string type;
        if (dict_name(object.value, "Type", type) && type == "Catalog") {
            trailer_.type = ValueType::Dict;
            Value root;
            root.type = ValueType::Ref;
            root.ref.number = object.number;
            trailer_.dict.clear();
            trailer_.dict.emplace_back("Root", std::move(root));
        }
        if (dict_name(object.value, "Type", type) && type == "XRef" && trailer_.type != ValueType::Dict) {
            trailer_ = object.value;
        }
    }
    if (trailer_.type != ValueType::Dict) {
        return {ErrorCode::FileRead, "Missing Root object"};
    }
    return ok_error();
}

Error Document::load_pages(std::uint32_t number, int depth, const Options& options) {
    if (options.max_pages > 0 && pages_.size() >= options.max_pages) {
        return ok_error();
    }
    if (depth >= static_cast<int>(kMaxDepth)) {
        return {ErrorCode::FileRead, "Depth of pages objects too great to load"};
    }
    if (cancelled(options)) {
        return {ErrorCode::Cancelled, "PDF page tree load cancelled"};
    }
    Error err = load_object(number);
    if (!err.ok()) {
        return err;
    }
    Object* object = find_object(number);
    if (object->state & kPageVisited) {
        return {ErrorCode::FileRead, "Recursive or repeated PDF page tree"};
    }
    object->state |= kPageVisited;
    const Value* dict = object_dict(number);
    if (dict == nullptr) {
        return {ErrorCode::FileRead, "page object is not a dictionary"};
    }
    const Value* kids = dict_array(*dict, "Kids");
    if (kids != nullptr) {
        for (const Value& kid : kids->array) {
            if (options.max_pages > 0 && pages_.size() >= options.max_pages) {
                break;
            }
            if (kid.type != ValueType::Ref) {
                continue;
            }
            err = load_pages(kid.ref.number, depth + 1, options);
            if (!err.ok()) {
                return err;
            }
        }
        return ok_error();
    }
    if (options.max_pages > 0 && pages_.size() >= options.max_pages) {
        return ok_error();
    }
    pages_.push_back(number);
    return ok_error();
}

Error Document::page_content(std::size_t index, std::string& decoded) {
    decoded.clear();
    if (index >= pages_.size()) {
        return {ErrorCode::BadArgs, "PDF page index out of range"};
    }
    const Value* page = page_dictionary(index);
    if (page == nullptr) {
        return {ErrorCode::FileRead, "unable to load page dictionary"};
    }
    std::vector<Ref> contents;
    const Value* contents_value = page_inherited(index, "Contents");
    Ref single;
    if (contents_value != nullptr && contents_value->type == ValueType::Ref) {
        contents.push_back(contents_value->ref);
    } else if (dict_ref(*page, "Contents", single)) {
        contents.push_back(single);
    } else {
        const Value* arr = contents_value != nullptr && contents_value->type == ValueType::Array
                               ? contents_value
                               : dict_array(*page, "Contents");
        if (arr != nullptr) {
            for (const Value& item : arr->array) {
                if (item.type == ValueType::Ref) {
                    contents.push_back(item.ref);
                }
            }
        }
    }
    if (contents.empty()) {
        return ok_error();
    }
    for (const Ref& ref : contents) {
        Error err = load_object(ref.number);
        if (!err.ok()) {
            return err;
        }
        Object* object = find_object(ref.number);
        if (object == nullptr) {
            continue;
        }
        std::string piece;
        err = decode_object_stream(*object, piece);
        if (!err.ok()) {
            return err;
        }
        if (decoded.size() >= kMaxDecodedStream || piece.size() >= kMaxDecodedStream - decoded.size()) {
            return {ErrorCode::FileRead, "decoded PDF page content exceeds size limit"};
        }
        decoded.append(piece);
        decoded.push_back('\n');
    }
    return ok_error();
}

}  // namespace ainiux::pdf
