#include "pdf/stream.hpp"

#include "pdf/limits.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace ainiux::pdf {
namespace {

struct ZInflate {
    z_stream stream{};
    bool ok = false;

    ZInflate() { ok = inflateInit(&stream) == Z_OK; }
    ~ZInflate() {
        if (ok) {
            inflateEnd(&stream);
        }
    }
    ZInflate(const ZInflate&) = delete;
    ZInflate& operator=(const ZInflate&) = delete;
};

struct ZDeflate {
    z_stream stream{};
    bool ok = false;

    ZDeflate() { ok = deflateInit(&stream, Z_BEST_COMPRESSION) == Z_OK; }
    ~ZDeflate() {
        if (ok) {
            deflateEnd(&stream);
        }
    }
    ZDeflate(const ZDeflate&) = delete;
    ZDeflate& operator=(const ZDeflate&) = delete;
};

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

std::vector<std::string> filter_names(const Value& dict) {
    std::vector<std::string> names;
    const Value* filter = dict_get(dict, "Filter");
    if (filter == nullptr) {
        return names;
    }
    if (filter->type == ValueType::Name) {
        names.push_back(filter->text);
        return names;
    }
    if (filter->type == ValueType::Array) {
        for (const Value& item : filter->array) {
            if (item.type == ValueType::Name) {
                names.push_back(item.text);
            }
        }
    }
    return names;
}

Error decode_ascii85(std::string_view in, std::string& out, std::size_t max_out) {
    out.clear();
    out.reserve(in.size());
    std::uint32_t acc = 0;
    int count = 0;
    auto flush = [&](int produced) -> Error {
        if (out.size() + static_cast<std::size_t>(produced) > max_out) {
            return {ErrorCode::FileRead, "decoded PDF stream exceeds size limit"};
        }
        for (int shift = 24; produced > 0; shift -= 8, --produced) {
            out.push_back(static_cast<char>((acc >> shift) & 0xFF));
        }
        return ok_error();
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(in[i]);
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '\0' || ch == '\f') {
            continue;
        }
        if (ch == '~') {
            break;
        }
        if (ch == 'z') {
            if (count != 0) {
                return {ErrorCode::FileRead, "Invalid ASCII85Decode sequence"};
            }
            if (out.size() + 4 > max_out) {
                return {ErrorCode::FileRead, "decoded PDF stream exceeds size limit"};
            }
            out.append(4, '\0');
            continue;
        }
        if (ch < '!' || ch > 'u') {
            return {ErrorCode::FileRead, "Invalid ASCII85Decode character"};
        }
        acc = acc * 85u + static_cast<std::uint32_t>(ch - '!');
        ++count;
        if (count == 5) {
            Error err = flush(4);
            if (!err.ok()) {
                return err;
            }
            acc = 0;
            count = 0;
        }
    }
    if (count > 0) {
        for (int i = count; i < 5; ++i) {
            acc = acc * 85u + 84u;
        }
        Error err = flush(count - 1);
        if (!err.ok()) {
            return err;
        }
    }
    return ok_error();
}

Error apply_named_filter(const std::string& name, std::string& data, std::size_t max_out) {
    if (name == "FlateDecode") {
        std::string decoded;
        Error err = inflate_flate(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), decoded, max_out);
        if (!err.ok()) {
            return err;
        }
        data = std::move(decoded);
        return ok_error();
    }
    if (name == "ASCII85Decode") {
        std::string decoded;
        Error err = decode_ascii85(data, decoded, max_out);
        if (!err.ok()) {
            return err;
        }
        data = std::move(decoded);
        return ok_error();
    }
    return {ErrorCode::UnsupportedFeature, "unsupported stream filter '/" + name + "'"};
}

}  // namespace

Error inflate_flate(const std::uint8_t* data, std::size_t size, std::string& out, std::size_t max_out) {
    out.clear();
    if (data == nullptr || size == 0) {
        return {ErrorCode::FileRead, "empty FlateDecode stream"};
    }
    if (size > std::numeric_limits<uInt>::max()) {
        return {ErrorCode::FileRead, "FlateDecode input exceeds size limit"};
    }
    ZInflate zinflate;
    if (!zinflate.ok) {
        return {ErrorCode::Internal, "unable to start FlateDecode"};
    }
    zinflate.stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zinflate.stream.avail_in = static_cast<uInt>(size);
    out.clear();
    std::array<char, 8192> buffer{};
    int status = Z_OK;
    do {
        zinflate.stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        zinflate.stream.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&zinflate.stream, Z_NO_FLUSH);
        const std::size_t produced = buffer.size() - zinflate.stream.avail_out;
        if (produced > max_out - out.size()) {
            return {ErrorCode::FileRead, "decoded PDF stream exceeds size limit"};
        }
        out.append(buffer.data(), produced);
        if (status == Z_STREAM_END) {
            return ok_error();
        }
        if (status != Z_OK) {
            return {ErrorCode::FileRead, "unable to decompress FlateDecode stream"};
        }
    } while (zinflate.stream.avail_in > 0 || zinflate.stream.avail_out == 0);
    return {ErrorCode::FileRead, "truncated FlateDecode stream"};
}

Error deflate_flate(std::string_view raw, std::string& out) {
    out.clear();
    ZDeflate zdeflate;
    if (!zdeflate.ok) {
        return {ErrorCode::Internal, "unable to start FlateEncode"};
    }
    zdeflate.stream.next_in =
        const_cast<Bytef*>(reinterpret_cast<const Bytef*>(raw.data()));
    zdeflate.stream.avail_in = static_cast<uInt>(raw.size());
    std::array<char, 4096> buffer{};
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        zdeflate.stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        zdeflate.stream.avail_out = static_cast<uInt>(buffer.size());
        status = deflate(&zdeflate.stream, Z_FINISH);
        const std::size_t produced = buffer.size() - zdeflate.stream.avail_out;
        out.append(buffer.data(), produced);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
            return {ErrorCode::Internal, "unable to compress FlateDecode stream"};
        }
    }
    return ok_error();
}

Error apply_png_predictor(std::string& data, std::size_t columns, int colors, int bits_per_component) {
    if (columns == 0 || colors <= 0 || bits_per_component <= 0) {
        return {ErrorCode::FileRead, "invalid PNG predictor parameters"};
    }
    const auto max_size = std::numeric_limits<std::size_t>::max();
    const auto bits = static_cast<std::size_t>(bits_per_component);
    if (static_cast<std::size_t>(colors) > (max_size - 7) / bits) {
        return {ErrorCode::FileRead, "invalid PNG predictor pixel size"};
    }
    const auto pixel_bits = bits * static_cast<std::size_t>(colors);
    if (columns > (max_size - 7) / pixel_bits) {
        return {ErrorCode::FileRead, "invalid PNG predictor row size"};
    }
    const std::size_t row = (pixel_bits * columns + 7) / 8;
    if (row == 0 || row > kMaxDecodedStream) {
        return {ErrorCode::FileRead, "invalid PNG predictor row size"};
    }
    const std::size_t stride = row + 1;
    const std::size_t bpp = (pixel_bits + 7) / 8;
    if (data.size() % stride != 0) {
        return {ErrorCode::FileRead, "truncated PNG predictor row"};
    }
    if (data.empty()) {
        return ok_error();
    }
    const std::size_t rows = data.size() / stride;
    std::string out(rows * row, '\0');
    std::vector<std::uint8_t> prev(row, 0);
    for (std::size_t r = 0; r < rows; ++r) {
        const auto* raw = reinterpret_cast<const std::uint8_t*>(data.data() + r * stride);
        const int filter = raw[0];
        auto* recon = reinterpret_cast<std::uint8_t*>(&out[r * row]);
        const std::uint8_t* src = raw + 1;
        for (std::size_t i = 0; i < row; ++i) {
            const std::uint8_t a = i >= bpp ? recon[i - bpp] : 0;
            const std::uint8_t b = prev[i];
            const std::uint8_t c = i >= bpp ? prev[i - bpp] : 0;
            const std::uint8_t x = src[i];
            switch (filter) {
                case 0:
                case 10:
                    recon[i] = x;
                    break;
                case 1:
                case 11:
                    recon[i] = static_cast<std::uint8_t>(x + a);
                    break;
                case 2:
                case 12:
                    recon[i] = static_cast<std::uint8_t>(x + b);
                    break;
                case 3:
                case 13:
                    recon[i] = static_cast<std::uint8_t>(x + ((a + b) / 2));
                    break;
                case 4:
                case 14:
                    recon[i] = static_cast<std::uint8_t>(x + paeth(a, b, c));
                    break;
                default:
                    return {ErrorCode::FileRead, "bad PNG filter " + std::to_string(filter)};
            }
        }
        std::memcpy(prev.data(), recon, row);
    }
    data = std::move(out);
    return ok_error();
}

Error decode_stream_bytes(std::string_view raw, const Value& dict, std::string& out, std::size_t max_out) {
    if (raw.size() > max_out) {
        return {ErrorCode::FileRead, "PDF stream exceeds size limit"};
    }
    out.assign(raw.data(), raw.size());
    const std::vector<std::string> names = filter_names(dict);
    for (const std::string& name : names) {
        Error err = apply_named_filter(name, out, max_out);
        if (!err.ok()) {
            return err;
        }
    }
    const Value* parms = dict_dict(dict, "DecodeParms");
    if (parms == nullptr) {
        const Value* arr = dict_array(dict, "DecodeParms");
        if (arr != nullptr && !arr->array.empty() && arr->array[0].type == ValueType::Dict) {
            parms = &arr->array[0];
        }
    }
    if (parms == nullptr) {
        return ok_error();
    }
    std::int64_t predictor = 1;
    if (dict_get(*parms, "Predictor") != nullptr && !dict_int(*parms, "Predictor", predictor)) {
        return {ErrorCode::FileRead, "invalid PDF Predictor"};
    }
    if (predictor <= 1) {
        return ok_error();
    }
    std::int64_t columns = 1;
    std::int64_t colors = 1;
    std::int64_t bpc = 8;
    if ((dict_get(*parms, "Columns") != nullptr && !dict_int(*parms, "Columns", columns)) ||
        (dict_get(*parms, "Colors") != nullptr && !dict_int(*parms, "Colors", colors)) ||
        (dict_get(*parms, "BitsPerComponent") != nullptr && !dict_int(*parms, "BitsPerComponent", bpc)) ||
        columns <= 0 || static_cast<std::uint64_t>(columns) > std::numeric_limits<std::size_t>::max() ||
        colors <= 0 || colors > std::numeric_limits<int>::max() || bpc <= 0 || bpc > 16) {
        return {ErrorCode::FileRead, "invalid PNG predictor parameters"};
    }
    if (predictor >= 10) {
        return apply_png_predictor(out, static_cast<std::size_t>(columns), static_cast<int>(colors),
                                   static_cast<int>(bpc));
    }
    return {ErrorCode::UnsupportedFeature, "unsupported Predictor " + std::to_string(predictor)};
}

}  // namespace ainiux::pdf
