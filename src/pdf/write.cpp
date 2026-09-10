#include "pdf/write.hpp"

#include "pdf/stream.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace ainiux::pdf {
namespace {

std::string xref_line(std::size_t offset, unsigned generation, char type) {
    char line[32];
    const int n = std::snprintf(line, sizeof(line), "%010zu %05u %c \n", offset, generation, type);
    if (n != 20) {
        return std::string("0000000000 65535 f \n");
    }
    return std::string(line, 20);
}

}  // namespace

DocumentWriter::DocumentWriter() {
    bodies_.emplace_back();
}

std::uint32_t DocumentWriter::add_object(std::string body) {
    bodies_.push_back(std::move(body));
    return static_cast<std::uint32_t>(bodies_.size() - 1);
}

std::uint32_t DocumentWriter::reserve_object() {
    return add_object("%");
}

void DocumentWriter::set_object(std::uint32_t number, std::string body) {
    if (number > 0 && number < bodies_.size()) {
        bodies_[number] = std::move(body);
    }
}

Error DocumentWriter::add_stream(std::string extra_keys, std::string_view raw, bool flate,
                                 std::uint32_t& number) {
    std::string payload;
    if (flate) {
        Error err = deflate_flate(raw, payload);
        if (!err.ok()) {
            return err;
        }
    } else {
        payload.assign(raw.data(), raw.size());
    }
    std::string body = "<< /Length " + std::to_string(payload.size());
    if (flate) {
        body += " /Filter /FlateDecode";
    }
    body += extra_keys;
    body += " >>\nstream\n";
    body.append(payload);
    body += "\nendstream";
    number = add_object(std::move(body));
    return ok_error();
}

std::uint32_t DocumentWriter::object_count() const {
    return bodies_.empty() ? 0 : static_cast<std::uint32_t>(bodies_.size() - 1);
}

std::string DocumentWriter::finish(std::uint32_t root, std::uint32_t info) const {
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets(bodies_.size(), 0);
    for (std::size_t i = 1; i < bodies_.size(); ++i) {
        offsets[i] = out.size();
        out += std::to_string(i);
        out += " 0 obj\n";
        out += bodies_[i];
        if (out.empty() || out.back() != '\n') {
            out.push_back('\n');
        }
        out += "endobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 ";
    out += std::to_string(bodies_.size());
    out += "\n";
    out += xref_line(0, 65535, 'f');
    for (std::size_t i = 1; i < bodies_.size(); ++i) {
        out += xref_line(offsets[i], 0, 'n');
    }
    out += "trailer\n<< /Size ";
    out += std::to_string(bodies_.size());
    out += " /Root ";
    out += std::to_string(root);
    out += " 0 R /Info ";
    out += std::to_string(info);
    out += " 0 R >>\nstartxref\n";
    out += std::to_string(xref);
    out += "\n%%EOF\n";
    return out;
}

std::string pdf_escape_string(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char ch : text) {
        if (ch == '\\' || ch == '(' || ch == ')') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch < 32 || ch == 127) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\%03o", ch);
            out += buf;
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::string pdf_number(double value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    if (std::fabs(value) < 0.0000005) {
        return "0";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    std::string text(buf);
    for (char& ch : text) {
        if (ch == ',') {
            ch = '.';
        }
    }
    while (text.size() > 1 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text.empty() ? std::string("0") : text;
}

std::string pdf_creation_date() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "D:%04d%02d%02d%02d%02d%02dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace ainiux::pdf
