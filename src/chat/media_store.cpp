#include "chat/media_store.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace ainiux::chat {
namespace {

class Fd {
   public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

   private:
    int fd_ = -1;
};

std::string errno_message(const std::string& action, const std::string& path) {
    return action + ": " + path + ": " + std::strerror(errno);
}

std::string dirname_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

Error ensure_directory(const std::string& path) {
    if (path.empty() || path == "." || path == "/") {
        return ok_error();
    }
    struct stat st {};
    if (stat(path.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            return {ErrorCode::FileWrite, "media path exists but is not a directory: " + path};
        }
        return ok_error();
    }
    if (errno != ENOENT) {
        return {ErrorCode::FileWrite, errno_message("could not inspect media directory", path)};
    }
    Error err = ensure_directory(dirname_of(path));
    if (!err.ok()) {
        return err;
    }
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        return {ErrorCode::FileWrite, errno_message("could not create media directory", path)};
    }
    return ok_error();
}

uint32_t rotate_right(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32U - count));
}

class Sha256 {
   public:
    Sha256()
        : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(const unsigned char* data, size_t size) {
        total_bytes_ += size;
        while (size > 0) {
            const size_t available = block_.size() - block_size_;
            const size_t take = size < available ? size : available;
            std::memcpy(block_.data() + block_size_, data, take);
            block_size_ += take;
            data += take;
            size -= take;
            if (block_size_ == block_.size()) {
                transform(block_.data());
                block_size_ = 0;
            }
        }
    }

    std::array<unsigned char, 32> finish() {
        const uint64_t bit_count = static_cast<uint64_t>(total_bytes_) * 8U;
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56) {
            while (block_size_ < block_.size()) {
                block_[block_size_++] = 0;
            }
            transform(block_.data());
            block_size_ = 0;
        }
        while (block_size_ < 56) {
            block_[block_size_++] = 0;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            block_[block_size_++] = static_cast<unsigned char>((bit_count >> shift) & 0xffU);
        }
        transform(block_.data());

        std::array<unsigned char, 32> digest{};
        for (size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = static_cast<unsigned char>(state_[i] >> 24U);
            digest[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16U);
            digest[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8U);
            digest[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
        }
        return digest;
    }

   private:
    void transform(const unsigned char* block) {
        static constexpr std::array<uint32_t, 64> constants = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
            0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
            0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
            0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
            0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
        std::array<uint32_t, 64> words{};
        for (size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<uint32_t>(block[i * 4]) << 24U) |
                       (static_cast<uint32_t>(block[i * 4 + 1]) << 16U) |
                       (static_cast<uint32_t>(block[i * 4 + 2]) << 8U) |
                       static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (size_t i = 16; i < words.size(); ++i) {
            const uint32_t s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^
                                (words[i - 15] >> 3U);
            const uint32_t s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^
                                (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];
        for (size_t i = 0; i < words.size(); ++i) {
            const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const uint32_t choice = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + choice + constants[i] + words[i];
            const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<uint32_t, 8> state_{};
    std::array<unsigned char, 64> block_{};
    size_t block_size_ = 0;
    size_t total_bytes_ = 0;
};

std::string sha256_hex(const std::string& bytes) {
    Sha256 sha;
    sha.update(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    const std::array<unsigned char, 32> digest = sha.finish();
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char byte : digest) {
        out.push_back(hex[byte >> 4U]);
        out.push_back(hex[byte & 0x0fU]);
    }
    return out;
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

Error write_all(int fd, const std::string& bytes, const std::string& path) {
    const char* cursor = bytes.data();
    size_t remaining = bytes.size();
    while (remaining > 0) {
        const ssize_t count = write(fd, cursor, remaining);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {ErrorCode::FileWrite, errno_message("could not write media object", path)};
        }
        if (count == 0) {
            return {ErrorCode::FileWrite, "could not write media object: short write: " + path};
        }
        cursor += count;
        remaining -= static_cast<size_t>(count);
    }
    return ok_error();
}

Error read_file(const std::string& path,
                size_t max_bytes,
                std::string& bytes,
                runtime::CancellationToken cancellation) {
    Fd fd(open(path.c_str(), O_RDONLY));
    if (fd.get() < 0) {
        return {ErrorCode::FileRead, errno_message("could not open managed media", path)};
    }
    std::array<char, 8192> buffer{};
    while (true) {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "managed media read cancelled: " + path};
        }
        const ssize_t count = read(fd.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {ErrorCode::FileRead, errno_message("could not read managed media", path)};
        }
        if (count == 0) {
            break;
        }
        const size_t chunk = static_cast<size_t>(count);
        if (bytes.size() > max_bytes || chunk > max_bytes - bytes.size()) {
            return {ErrorCode::UnsupportedFeature,
                    "managed attachment exceeds its configured size limit: " + path};
        }
        bytes.append(buffer.data(), chunk);
    }
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
    const size_t slash = database_path.find_last_of('/');
    const std::string filename = slash == std::string::npos
                                     ? database_path
                                     : database_path.substr(slash + 1);
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
    const std::string digest = sha256_hex(bytes);
    const std::string destination = path_for_digest(database_path, digest, mime_type);
    Error err = ensure_directory(dirname_of(destination));
    if (!err.ok()) {
        return err;
    }

    struct stat existing {};
    if (stat(destination.c_str(), &existing) == 0) {
        if (!S_ISREG(existing.st_mode) || existing.st_size != static_cast<off_t>(bytes.size())) {
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
    } else if (errno == ENOENT) {
        std::string temporary_pattern = destination + ".tmp.XXXXXX";
        std::vector<char> temporary_buffer(temporary_pattern.begin(), temporary_pattern.end());
        temporary_buffer.push_back('\0');
        Fd fd(mkstemp(temporary_buffer.data()));
        const std::string temporary = temporary_buffer.data();
        if (fd.get() < 0) {
            return {ErrorCode::FileWrite,
                    errno_message("could not create temporary media object", temporary)};
        }
        if (fchmod(fd.get(), 0600) != 0) {
            Error mode_error{ErrorCode::FileWrite,
                             errno_message("could not protect temporary media object", temporary)};
            unlink(temporary.c_str());
            return mode_error;
        }
        err = write_all(fd.get(), bytes, temporary);
        if (!err.ok()) {
            unlink(temporary.c_str());
            return err;
        }
        if (fsync(fd.get()) != 0) {
            err = {ErrorCode::FileWrite, errno_message("could not fsync media object", temporary)};
            unlink(temporary.c_str());
            return err;
        }
        if (close(fd.release()) != 0) {
            err = {ErrorCode::FileWrite, errno_message("could not close media object", temporary)};
            unlink(temporary.c_str());
            return err;
        }
        if (link(temporary.c_str(), destination.c_str()) != 0 && errno != EEXIST) {
            err = {ErrorCode::FileWrite,
                   errno_message("could not install managed media object", destination)};
            unlink(temporary.c_str());
            return err;
        }
        unlink(temporary.c_str());
        Fd directory(open(dirname_of(destination).c_str(), O_RDONLY));
        if (directory.get() >= 0) {
            fsync(directory.get());
        }
    } else {
        return {ErrorCode::FileWrite, errno_message("could not inspect media object", destination)};
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
            if (sha256_hex(bytes) != image.storage_ref) {
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
                if (sha256_hex(markdown) != attachment.storage_ref) {
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
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return ok_error();
        }
        return {ErrorCode::FileRead, errno_message("could not inspect managed media", path)};
    }
    available = S_ISREG(st.st_mode) &&
                (expected_size <= 0 || st.st_size == static_cast<off_t>(expected_size));
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
    if (unlink(path.c_str()) != 0) {
        if (errno == ENOENT) {
            return ok_error();
        }
        return {ErrorCode::FileWrite, errno_message("could not remove managed media", path)};
    }
    removed = true;
    Fd directory(open(dirname_of(path).c_str(), O_RDONLY));
    if (directory.get() >= 0) {
        fsync(directory.get());
    }
    return ok_error();
}

}  // namespace ainiux::chat
