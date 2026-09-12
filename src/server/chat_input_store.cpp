#include "server/chat_input_store.hpp"

#include <atomic>
#include <new>
#include <stdexcept>
#include <utility>

#include "html/html.hpp"
#include "docx/docx.hpp"
#include "input/input.hpp"
#include "pdf/pdf.hpp"
#include "platform/filesystem.hpp"
#include "server/limits.hpp"

namespace ainiux::server {
namespace {

std::string basename_of(std::string name) {
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    return name;
}

std::string media_type_of(std::string mime) {
    mime = ascii_lower(std::move(mime));
    const std::size_t semi = mime.find(';');
    if (semi != std::string::npos) mime = mime.substr(0, semi);
    return ascii_trim(std::move(mime));
}

}  // namespace

struct ChatInputStore::Budget {
    explicit Budget(std::size_t maximum) : capacity(maximum) {}
    std::atomic<std::size_t> resident{0};
    const std::size_t capacity;
};

ChatInputStore::ChatInputStore(std::size_t byte_capacity, std::chrono::seconds lifetime)
    : budget_(std::make_shared<Budget>(byte_capacity)), lifetime_(lifetime) {}

void ChatInputStore::erase_expired_locked(std::chrono::system_clock::time_point now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires_at <= now) it = entries_.erase(it);
        else ++it;
    }
}

Error ChatInputStore::add(std::string mime_type,
                          std::string filename,
                          std::string bytes,
                          StoredChatInput& output) {
    if (bytes.size() > Limits::upload_body_bytes) {
        return {ErrorCode::BadArgs, "chat upload exceeds the 20 MiB per-file limit"};
    }
    filename = basename_of(std::move(filename));
    const std::string media = media_type_of(mime_type);
    ChatInputKind kind = ChatInputKind::Text;
    bool converted = false;
    std::string stored_mime = media;
    std::string payload = std::move(bytes);
    std::vector<std::string> warnings;

    input::FileType type;
    const bool named = !filename.empty() && input::classify_file_type(filename, type).ok();
    if (media == "image/png" || media == "image/jpeg" || media == "image/gif" ||
        (named && type.kind == input::Kind::Image)) {
        kind = ChatInputKind::Image;
        if (stored_mime.empty() && named) stored_mime = type.mime_type;
        Error error = input::validate_image_bytes(payload, stored_mime);
        if (!error.ok()) return error;
    } else if (media == "application/pdf" || media == "application/x-pdf" ||
               (named && type.kind == input::Kind::Pdf) ||
               (payload.size() >= 5 && payload.compare(0, 5, "%PDF-") == 0)) {
        pdf::Options pdf_options;
        pdf_options.max_bytes = payload.size();
        std::string markdown;
        Error error = pdf::to_markdown_bytes(payload, pdf_options, markdown);
        if (!error.ok()) return error;
        payload = std::move(markdown);
        stored_mime = "text/markdown";
        converted = true;
    } else if (media == docx::kMimeType || (named && type.kind == input::Kind::Docx)) {
        docx::ReadOptions docx_options;
        docx_options.max_bytes = Limits::upload_body_bytes;
        docx::Diagnostics diagnostics;
        std::string markdown;
        Error error = docx::to_markdown_bytes(payload, docx_options, markdown, &diagnostics);
        if (!error.ok()) return error;
        payload = std::move(markdown);
        stored_mime = "text/markdown";
        converted = true;
        warnings = std::move(diagnostics.messages);
    } else if (media == "text/html" || (named && type.kind == input::Kind::Html)) {
        try {
            payload = html::convert(payload, html::OutputFormat::Markdown);
        } catch (const std::bad_alloc&) {
            return {ErrorCode::Internal, "not enough memory to convert uploaded HTML"};
        } catch (const std::length_error&) {
            return {ErrorCode::UnsupportedFeature, "converted HTML is too large to attach"};
        }
        stored_mime = "text/markdown";
        converted = true;
    } else if (media == "text/plain" || media == "text/markdown" ||
               (named && (type.kind == input::Kind::Plaintext || type.kind == input::Kind::Markdown))) {
        stored_mime = named && type.kind == input::Kind::Markdown ? "text/markdown" : "text/plain";
        if (media == "text/markdown") stored_mime = "text/markdown";
    } else {
        return {ErrorCode::UnsupportedFeature,
                "chat uploads accept PNG, JPEG, GIF, PDF, DOCX, Markdown, plaintext, or HTML"};
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        erase_expired_locked(std::chrono::system_clock::now());
    }

    std::size_t observed = budget_->resident.load(std::memory_order_acquire);
    do {
        if (observed > budget_->capacity || payload.size() > budget_->capacity - observed) {
            return {ErrorCode::RateLimit,
                    "temporary chat upload storage is full; remove inputs or retry later"};
        }
    } while (!budget_->resident.compare_exchange_weak(
        observed, observed + payload.size(), std::memory_order_acq_rel));

    const std::shared_ptr<Budget> budget = budget_;
    const std::size_t byte_count = payload.size();
    std::shared_ptr<const std::string> body(
        new std::string(std::move(payload)),
        [budget, byte_count](const std::string* value) {
            delete value;
            budget->resident.fetch_sub(byte_count, std::memory_order_acq_rel);
        });
    const auto expires = std::chrono::system_clock::now() + lifetime_;
    std::string id;
    Error error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::string random;
            error = platform::secure_random_hex(16U, random);
            if (!error.ok()) return error;
            id = "chat_" + random;
            if (entries_.find(id) == entries_.end()) break;
            id.clear();
        }
        if (id.empty()) {
            return {ErrorCode::Internal, "could not allocate a unique chat upload identifier"};
        }
        entries_.emplace(id, Entry{kind, stored_mime, filename, converted, warnings, body, expires});
    }
    output = {id, kind, stored_mime, filename, converted, std::move(warnings), std::move(body), expires};
    return ok_error();
}

Error ChatInputStore::resolve(const std::vector<std::string>& ids,
                              std::vector<StoredChatInput>& output) {
    output.clear();
    if (ids.size() > Limits::chat_input_count) {
        return {ErrorCode::BadArgs, "chat input_ids accepts at most 16 files"};
    }
    const auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    erase_expired_locked(now);
    for (const std::string& id : ids) {
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            return {ErrorCode::FileRead, "uploaded chat input is missing or expired: " + id};
        }
        output.push_back({id, found->second.kind, found->second.mime_type, found->second.display_name,
                          found->second.converted, found->second.warnings, found->second.bytes,
                          found->second.expires_at});
    }
    return ok_error();
}

bool ChatInputStore::erase(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    erase_expired_locked(std::chrono::system_clock::now());
    return entries_.erase(id) != 0;
}

}  // namespace ainiux::server
