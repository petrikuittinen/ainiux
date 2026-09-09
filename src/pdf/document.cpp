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
    if (objects_.size() <= number) {
        objects_.resize(number + 1);
    }
    Object& object = objects_[number];
    if (object.number == number && generation < object.generation) {
        return true;
    }
    object.number = number;
    object.generation = generation;
    object.offset = compressed ? 0 : offset;
    object.compressed = compressed;
    object.objstm_number = objstm_number;
    object.loaded = false;
    object.value = Value{};
    object.stream_offset = 0;
    object.stream_length = 0;
    return true;
}

Object* Document::find_object(std::uint32_t number) {
    if (number == 0 || number >= objects_.size() || objects_[number].number != number) {
        return nullptr;
    }
    return &objects_[number];
}

Error Document::parse_object_at(std::size_t offset, Object& object) {
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
        std::int64_t length = 0;
        if (object.value.type == ValueType::Dict && dict_int(object.value, "Length", length) && length > 0) {
            object.stream_length = static_cast<std::size_t>(length);
        } else if (object.value.type == ValueType::Dict) {
            Ref length_ref;
            if (dict_ref(object.value, "Length", length_ref)) {
                Error loaded = load_object(length_ref.number);
                if (!loaded.ok()) {
                    return loaded;
                }
                Object* length_obj = find_object(length_ref.number);
                if (length_obj != nullptr && length_obj->value.type == ValueType::Number &&
                    length_obj->value.number > 0) {
                    object.stream_length = static_cast<std::size_t>(length_obj->value.number);
                }
            }
        }
        if (object.stream_length == 0) {
            object.stream_length = scan_endstream(bytes_, object.stream_offset);
        }
        if (object.stream_offset + object.stream_length > bytes_.size()) {
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
        return load_obj_stream(object->objstm_number, Options{});
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
    if (object.stream_length == 0) {
        return {ErrorCode::FileRead, "No stream data"};
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
        widths[i] = static_cast<int>(w->array[i].number);
        if (widths[i] < 0 || widths[i] > 8) {
            return {ErrorCode::FileRead, "Bad W array in cross-reference stream"};
        }
    }
    if (widths[1] == 0) {
        return {ErrorCode::FileRead, "Bad W array in cross-reference stream"};
    }
    const int row = widths[0] + widths[1] + widths[2];
    std::vector<std::pair<std::uint32_t, std::uint32_t>> index;
    const Value* index_arr = dict_array(xref_obj.value, "Index");
    if (index_arr != nullptr && index_arr->array.size() >= 2) {
        for (std::size_t i = 0; i + 1 < index_arr->array.size(); i += 2) {
            if (index_arr->array[i].type != ValueType::Number || index_arr->array[i + 1].type != ValueType::Number) {
                continue;
            }
            index.emplace_back(static_cast<std::uint32_t>(index_arr->array[i].number),
                               static_cast<std::uint32_t>(index_arr->array[i + 1].number));
        }
    }
    if (index.empty()) {
        std::int64_t size = 0;
        dict_int(xref_obj.value, "Size", size);
        index.emplace_back(0, size > 0 ? static_cast<std::uint32_t>(size) : 0);
    }
    std::size_t cursor = 0;
    for (const auto& span : index) {
        if (cancelled(options)) {
            return {ErrorCode::Cancelled, "PDF xref load cancelled"};
        }
        std::uint32_t number = span.first;
        for (std::uint32_t n = 0; n < span.second; ++n, ++number) {
            if (cursor + static_cast<std::size_t>(row) > decoded.size()) {
                break;
            }
            const auto* field = reinterpret_cast<const std::uint8_t*>(decoded.data() + cursor);
            cursor += static_cast<std::size_t>(row);
            int type = 1;
            if (widths[0] > 0) {
                type = static_cast<int>(read_be(field, widths[0]));
            }
            const std::uint64_t field1 = read_be(field + widths[0], widths[1]);
            std::uint64_t field2 = widths[2] > 0 ? read_be(field + widths[0] + widths[1], widths[2]) : 0;
            if (field2 > 65535) {
                field2 = 65535;
            }
            if (type == 0) {
                continue;
            }
            if (type == 2) {
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
            } else {
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
        const std::uint32_t start = static_cast<std::uint32_t>(std::strtoul(token.text.c_str(), nullptr, 10));
        const std::uint32_t count = static_cast<std::uint32_t>(std::strtoul(count_tok.text.c_str(), nullptr, 10));
        Cursor& cursor = tokens.cursor();
        while (pdf_isspace(cursor.peek())) {
            cursor.get();
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (cursor.tell() + 20 > cursor.size) {
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
            const std::size_t offset = static_cast<std::size_t>(std::strtoull(line, nullptr, 10));
            const unsigned generation = static_cast<unsigned>(std::strtoul(line + 11, nullptr, 10));
            if (!add_placeholder(start + i, static_cast<std::uint16_t>(generation), offset, false, 0)) {
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
        double prev_number = 0;
        if (dict_number(this_trailer, "Prev", prev_number) && prev_number > 0) {
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
    dict_int(stream->value, "N", count);
    dict_int(stream->value, "First", first);
    if (count < 0 || static_cast<std::size_t>(count) > kMaxObjStmObjects) {
        return {ErrorCode::FileRead, "Too many compressed objects"};
    }
    Tokenizer header(Cursor{reinterpret_cast<const std::uint8_t*>(decoded.data()), decoded.size(), 0});
    std::vector<std::uint32_t> numbers;
    numbers.reserve(static_cast<std::size_t>(count));
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
        if (n.kind != TokenKind::Number) {
            break;
        }
        numbers.push_back(static_cast<std::uint32_t>(std::strtoul(n.text.c_str(), nullptr, 10)));
    }
    const std::size_t values_at = first > 0 ? static_cast<std::size_t>(first) : header.cursor().tell();
    Tokenizer values(Cursor{reinterpret_cast<const std::uint8_t*>(decoded.data()), decoded.size(), values_at});
    for (std::uint32_t obj_number : numbers) {
        if (!add_placeholder(obj_number, 0, 0, true, number)) {
            return {ErrorCode::FileRead, "Too many compressed objects"};
        }
        Object* object = find_object(obj_number);
        if (object == nullptr) {
            continue;
        }
        err = read_value(values, object->value, 0);
        if (!err.ok()) {
            return err;
        }
        object->loaded = true;
        object->compressed = true;
        object->objstm_number = number;
    }
    return ok_error();
}

Error Document::repair_xref(const Options& options) {
    objects_.clear();
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
    for (const Object& object : objects_) {
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
    const Value* dict = object_dict(number);
    if (dict == nullptr) {
        return {ErrorCode::FileRead, "page object is not a dictionary"};
    }
    const Value* kids = dict_array(*dict, "Kids");
    if (kids != nullptr) {
        for (const Value& kid : kids->array) {
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
        decoded.append(piece);
        decoded.push_back('\n');
    }
    return ok_error();
}

}  // namespace ainiux::pdf
