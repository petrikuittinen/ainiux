#include "chat/media_store.hpp"
#include "platform/filesystem.hpp"
#include "security/hash.hpp"

#include <filesystem>
#include <limits>
#include <sstream>
#include <vector>

namespace ainiux::chat {
namespace {

std::string dirname_of(const std::string& path) {
    const std::filesystem::path parent = std::filesystem::u8path(path).parent_path();
    return parent.empty() ? "." : parent.u8string();
}

Error ensure_directory(const std::string& path) {
    return platform::ensure_private_directory(path, true, true);
}

bool valid_digest(const std::string& digest) {
    if (digest.size() != 64) {
        return false;
    }
    for (char ch : digest) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string extension_for_mime_type(const std::string& mime_type) {
    return mime_type == "text/markdown" ? ".md" : std::string();
}

std::string relative_path_for_digest(const std::string& digest, const std::string& mime_type) {
    return "sha256/" + digest.substr(0, 2) + "/" + digest +
           extension_for_mime_type(mime_type);
}

std::string path_for_digest(const std::string& database_path,
                            const std::string& digest,
                            const std::string& mime_type) {
    return media_root_for_database(database_path) + "/" +
           relative_path_for_digest(digest, mime_type);
}

Error read_file(const std::string& path,
                size_t max_bytes,
                std::string& bytes,
                runtime::CancellationToken cancellation) {
    if (cancellation.cancelled())
        return {ErrorCode::Cancelled, "managed media read cancelled: " + path};
    Error error = platform::read_file_bounded(path, max_bytes, bytes);
    if (!error.ok() && error.message.find("exceeds size limit") != std::string::npos)
        return {ErrorCode::UnsupportedFeature,
                "managed attachment exceeds its configured size limit: " + path};
    if (!error.ok()) return error;
    if (cancellation.cancelled())
        return {ErrorCode::Cancelled, "managed media read cancelled: " + path};
    return ok_error();
}

std::string base64_encode(const std::string& data) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        const unsigned int a = static_cast<unsigned char>(data[i++]);
        const unsigned int b = static_cast<unsigned char>(data[i++]);
        const unsigned int c = static_cast<unsigned char>(data[i++]);
        out.push_back(alphabet[a >> 2]);
        out.push_back(alphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
        out.push_back(alphabet[((b & 0x0fU) << 2U) | (c >> 6U)]);
        out.push_back(alphabet[c & 0x3fU]);
    }
    const size_t remaining = data.size() - i;
    if (remaining == 1) {
        const unsigned int a = static_cast<unsigned char>(data[i]);
        out.push_back(alphabet[a >> 2]);
        out.push_back(alphabet[(a & 0x03U) << 4U]);
        out += "==";
    } else if (remaining == 2) {
        const unsigned int a = static_cast<unsigned char>(data[i]);
        const unsigned int b = static_cast<unsigned char>(data[i + 1]);
        out.push_back(alphabet[a >> 2]);
        out.push_back(alphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
        out.push_back(alphabet[(b & 0x0fU) << 2U]);
        out.push_back('=');
    }
    return out;
}

int base64_value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

Error base64_decode(const std::string& encoded, std::string& decoded) {
    if (encoded.empty() || encoded.size() % 4 != 0) {
        return {ErrorCode::ProviderSchema, "stored image base64 has an invalid length"};
    }
    decoded.clear();
    decoded.reserve((encoded.size() / 4) * 3);
    for (size_t i = 0; i < encoded.size(); i += 4) {
        const bool last = i + 4 == encoded.size();
        const int a = base64_value(static_cast<unsigned char>(encoded[i]));
        const int b = base64_value(static_cast<unsigned char>(encoded[i + 1]));
        const bool pad_c = encoded[i + 2] == '=';
        const bool pad_d = encoded[i + 3] == '=';
        const int c = pad_c ? 0 : base64_value(static_cast<unsigned char>(encoded[i + 2]));
        const int d = pad_d ? 0 : base64_value(static_cast<unsigned char>(encoded[i + 3]));
        if (a < 0 || b < 0 || c < 0 || d < 0 || (!last && (pad_c || pad_d)) ||
            (pad_c && !pad_d)) {
            return {ErrorCode::ProviderSchema, "stored image base64 is malformed"};
        }
        decoded.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (!pad_c) {
            decoded.push_back(static_cast<char>(((b & 0x0f) << 4) | (c >> 2)));
        }
        if (!pad_d) {
            decoded.push_back(static_cast<char>(((c & 0x03) << 6) | d));
        }
    }
    return ok_error();
}

}  // namespace

std::string media_root_for_database(const std::string& database_path) {
    const std::filesystem::path database = std::filesystem::u8path(database_path);
    const std::string filename = database.filename().u8string();
    if (filename == "ainiux.db") {
        return dirname_of(database_path) + "/media";
    }
    return database_path + ".media";
}

Error store_media_bytes(const std::string& database_path,
                        const std::string& bytes,
                        const std::string& mime_type,
                        StoredMedia& stored) {
    if (bytes.empty()) {
        return {ErrorCode::UnsupportedFeature, "cannot persist an empty media attachment"};
    }
    const std::string digest = security::sha256_hex(bytes);
    const std::string destination = path_for_digest(database_path, digest, mime_type);
    Error err = ensure_directory(dirname_of(destination));
    if (!err.ok()) {
        return err;
    }

    std::error_code filesystem_error;
    const std::filesystem::file_status existing =
        std::filesystem::status(std::filesystem::u8path(destination), filesystem_error);
    if (!filesystem_error && std::filesystem::exists(existing)) {
        const std::uintmax_t existing_size =
            std::filesystem::file_size(std::filesystem::u8path(destination), filesystem_error);
        if (filesystem_error || !std::filesystem::is_regular_file(existing) ||
            existing_size != bytes.size()) {
            return {ErrorCode::FileRead, "managed media object is corrupt: " + destination};
        }
        std::string existing_bytes;
        err = read_file(destination, bytes.size(), existing_bytes,
                        runtime::CancellationToken());
        if (!err.ok()) {
            return err;
        }
        if (existing_bytes != bytes) {
            return {ErrorCode::FileRead,
                    "managed media object content does not match its SHA-256 path: " +
                        destination};
        }
    } else if (!filesystem_error ||
               filesystem_error == std::errc::no_such_file_or_directory) {
        err = platform::atomic_write_private(destination, bytes, true);
        if (!err.ok()) return err;
    } else {
        return {ErrorCode::FileWrite,
                "could not inspect media object: " + destination + ": " +
                    filesystem_error.message()};
    }

    stored.sha256 = digest;
    stored.storage_ref = relative_path_for_digest(digest, mime_type);
    stored.mime_type = mime_type;
    stored.byte_size = static_cast<long long>(bytes.size());
    return ok_error();
}

Error store_media_base64(const std::string& database_path,
                         const std::string& base64_data,
                         const std::string& mime_type,
                         StoredMedia& stored) {
    std::string bytes;
    Error err = base64_decode(base64_data, bytes);
    if (!err.ok()) {
        return err;
    }
    return store_media_bytes(database_path, bytes, mime_type, stored);
}

Error hydrate_message_images(const std::string& database_path,
                             std::vector<provider::Message>& messages,
                             size_t max_image_bytes,
                             runtime::CancellationToken cancellation) {
    for (provider::Message& message : messages) {
        for (provider::ImageInput& image : message.images) {
            if (!image.base64_data.empty()) {
                continue;
            }
            if (!valid_digest(image.storage_ref)) {
                return {ErrorCode::ProviderSchema,
                        "message attachment has an invalid managed-media reference"};
            }
            std::string bytes;
            Error err = read_file(path_for_digest(database_path, image.storage_ref,
                                                  image.mime_type),
                                  max_image_bytes, bytes, cancellation);
            if (!err.ok()) {
                const std::string label = image.display_name.empty() ? image.storage_ref : image.display_name;
                return {err.code, err.message + "\nAttachment: " + label};
            }
            if (image.byte_size > 0 && image.byte_size != static_cast<long long>(bytes.size())) {
                return {ErrorCode::FileRead,
                        "managed media size does not match its database record: " + image.storage_ref};
            }
            if (security::sha256_hex(bytes) != image.storage_ref) {
                return {ErrorCode::FileRead,
                        "managed media hash does not match its database record: " + image.storage_ref};
            }
            image.base64_data = base64_encode(bytes);
            if (cancellation.cancelled()) {
                return {ErrorCode::Cancelled, "managed media encoding cancelled"};
            }
        }
    }
    return ok_error();
}

Error hydrate_message_text_attachments(const std::string& database_path,
                                       std::vector<provider::Message>& messages,
                                       size_t max_attachment_bytes,
                                       runtime::CancellationToken cancellation) {
    for (provider::Message& message : messages) {
        if (message.text_attachments.empty()) {
            continue;
        }
        std::string expanded = message.content;
        if (!expanded.empty() && expanded.back() != '\n') {
            expanded.push_back('\n');
        }
        expanded += "\n# Attached Markdown\n\n";
        for (size_t index = 0; index < message.text_attachments.size(); ++index) {
            provider::TextAttachment& attachment = message.text_attachments[index];
            std::string markdown = attachment.markdown_content;
            if (markdown.empty() && !attachment.storage_ref.empty()) {
                if (!valid_digest(attachment.storage_ref)) {
                    return {ErrorCode::ProviderSchema,
                            "text attachment has an invalid managed-media reference"};
                }
                Error err = read_file(path_for_digest(database_path, attachment.storage_ref,
                                                      "text/markdown"),
                                      max_attachment_bytes, markdown, cancellation);
                if (!err.ok()) {
                    const std::string label = attachment.display_name.empty()
                                                  ? attachment.storage_ref
                                                  : attachment.display_name;
                    return {err.code, err.message + "\nAttachment: " + label};
                }
                if (security::sha256_hex(markdown) != attachment.storage_ref) {
                    return {ErrorCode::FileRead,
                            "managed Markdown hash does not match its database record: " +
                                attachment.storage_ref};
                }
            } else if (markdown.empty() && attachment.byte_size != 0) {
                return {ErrorCode::ProviderSchema,
                        "inline Markdown attachment content is missing: " +
                            attachment.display_name};
            }
            if (attachment.byte_size > 0 &&
                attachment.byte_size != static_cast<long long>(markdown.size())) {
                return {ErrorCode::FileRead,
                        "Markdown attachment size does not match its database record: " +
                            attachment.display_name};
            }
            expanded += "---" + attachment.display_name + "---\n";
            expanded += markdown;
            if (!markdown.empty() && markdown.back() != '\n') {
                expanded.push_back('\n');
            }
            if (index + 1 < message.text_attachments.size()) {
                expanded.push_back('\n');
            }
            if (cancellation.cancelled()) {
                return {ErrorCode::Cancelled, "Markdown attachment hydration cancelled"};
            }
        }
        message.content = std::move(expanded);
        message.text_attachments.clear();
    }
    return ok_error();
}

Error media_file_available(const std::string& database_path,
                           const std::string& sha256,
                           const std::string& mime_type,
                           long long expected_size,
                           bool& available) {
    available = false;
    if (!valid_digest(sha256)) {
        return {ErrorCode::ProviderSchema, "invalid managed-media SHA-256 reference"};
    }
    const std::string path = path_for_digest(database_path, sha256, mime_type);
    std::error_code filesystem_error;
    const std::filesystem::file_status status =
        std::filesystem::status(std::filesystem::u8path(path), filesystem_error);
    if (filesystem_error == std::errc::no_such_file_or_directory) return ok_error();
    if (filesystem_error)
        return {ErrorCode::FileRead,
                "could not inspect managed media: " + path + ": " +
                    filesystem_error.message()};
    const std::uintmax_t size = std::filesystem::is_regular_file(status)
                                    ? std::filesystem::file_size(
                                          std::filesystem::u8path(path), filesystem_error)
                                    : 0;
    available = !filesystem_error && std::filesystem::is_regular_file(status) &&
                (expected_size <= 0 || size == static_cast<std::uintmax_t>(expected_size));
    return ok_error();
}

Error remove_media_file(const std::string& database_path,
                        const std::string& sha256,
                        const std::string& mime_type,
                        bool& removed) {
    removed = false;
    if (!valid_digest(sha256)) {
        return {ErrorCode::ProviderSchema, "invalid managed-media SHA-256 reference"};
    }
    const std::string path = path_for_digest(database_path, sha256, mime_type);
    std::error_code filesystem_error;
    removed = std::filesystem::remove(std::filesystem::u8path(path), filesystem_error);
    if (filesystem_error)
        return {ErrorCode::FileWrite,
                "could not remove managed media: " + path + ": " +
                    filesystem_error.message()};
    return ok_error();
}

}  // namespace ainiux::chat
