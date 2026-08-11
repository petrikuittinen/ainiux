#include "mcp/arg_rewrite.hpp"

#include <cctype>
#include <filesystem>
#include <sstream>

#include "input/input.hpp"
#include "json/json.hpp"

namespace ainiux::mcp {
namespace {

namespace fs = std::filesystem;

bool looks_like_path(const std::string& s) {
    if (s.empty() || s.size() > 4096) return false;
    if (s.find('\n') != std::string::npos || s.find('\0') != std::string::npos) return false;
    // Skip obvious base64 blobs.
    if (s.size() > 256) {
        bool b64ish = true;
        for (char c : s) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' ||
                  c == '=' || c == '\n' || c == '\r')) {
                b64ish = false;
                break;
            }
        }
        if (b64ish) return false;
    }
    if (s[0] == '/' || s[0] == '.' || s[0] == '~') return true;
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return true;
    // bare file.ext
    const auto dot = s.rfind('.');
    if (dot != std::string::npos && dot + 1 < s.size() && dot > 0) {
        const std::string ext = s.substr(dot + 1);
        if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "PNG" ||
            ext == "JPG" || ext == "JPEG" || ext == "GIF")
            return true;
    }
    return false;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool name_suggests_base64(const std::string& key) {
    const std::string k = lower_ascii(key);
    return k.find("base64") != std::string::npos || k == "data" || k == "image_data" ||
           k == "content" || k == "image" || k == "bytes" || k == "blob" ||
           k.find("image_b64") != std::string::npos;
}

bool name_suggests_path(const std::string& key) {
    const std::string k = lower_ascii(key);
    return k == "path" || k == "file" || k == "filename" || k == "filepath" ||
           k == "image_path" || k == "file_path" || k == "src" || k == "source";
}

bool name_suggests_mime(const std::string& key) {
    const std::string k = lower_ascii(key);
    return k == "mime" || k == "mimetype" || k == "mime_type" || k == "content_type" ||
           k == "media_type";
}

bool prefer_base64(const ServerConfig& server,
                   const std::string& field_key,
                   const ArgRewriteCaps& caps) {
    if (caps.force_base64) return true;
    if (server.transport == TransportKind::Http) return true;
    if (name_suggests_base64(field_key) && !name_suggests_path(field_key)) return true;
    return false;
}

json::Value jstring(const std::string& s) {
    json::Value v;
    v.type = json::Value::Type::String;
    v.string = s;
    return v;
}

// Resolve a path string to absolute under bag or filesystem (workspace-relative).
bool resolve_existing_file(const std::string& raw, std::string& absolute_out) {
    absolute_out.clear();
    std::error_code ec;
    fs::path p = fs::u8path(raw);
    if (p.is_relative()) {
        p = fs::absolute(p, ec);
        if (ec) return false;
    }
    if (!fs::is_regular_file(p, ec) || ec) return false;
    absolute_out = fs::weakly_canonical(p, ec).generic_u8string();
    if (ec || absolute_out.empty()) absolute_out = p.generic_u8string();
    return true;
}

Error load_image_into_bag(agent::AttachmentBag& bag,
                          const std::string& absolute,
                          const std::string& display,
                          const ArgRewriteCaps& caps,
                          runtime::CancellationToken cancellation,
                          const agent::AttachmentEntry*& out_entry) {
    out_entry = bag.find_by_path(absolute);
    if (out_entry != nullptr && !out_entry->base64_data.empty()) return ok_error();

    input::FileType type;
    Error err = input::classify_file_type(absolute, type);
    if (!err.ok()) return err;
    if (type.kind != input::Kind::Image)
        return {ErrorCode::BadArgs, "path is not a PNG/JPEG/GIF image: " + absolute};

    input::ImageData loaded;
    err = input::load_image_file(absolute, type, caps.max_image_bytes, loaded, cancellation);
    if (!err.ok()) return err;

    err = bag.add_image(absolute, display.empty() ? absolute : display, loaded.mime_type,
                        loaded.base64_data, loaded.byte_size,
                        agent::AttachmentSource::OnDemandPath, false);
    if (!err.ok()) return err;
    out_entry = bag.find_by_path(absolute);
    if (out_entry == nullptr)
        return {ErrorCode::Internal, "attachment bag missing after add"};
    return ok_error();
}

void rewrite_object(json::Value& obj,
                    const ServerConfig& server,
                    const ArgRewriteCaps& caps,
                    agent::AttachmentBag& bag,
                    runtime::CancellationToken cancellation,
                    ArgRewriteResult& result,
                    int depth);

void maybe_rewrite_string_field(json::Value& object,
                                const std::string& key,
                                json::Value& value,
                                const ServerConfig& server,
                                const ArgRewriteCaps& caps,
                                agent::AttachmentBag& bag,
                                runtime::CancellationToken cancellation,
                                ArgRewriteResult& result) {
    if (!value.is_string()) return;
    const std::string& raw = value.string;
    if (!looks_like_path(raw) && bag.find_by_path(raw) == nullptr) return;

    std::string absolute;
    const agent::AttachmentEntry* entry = bag.find_by_path(raw);
    if (entry != nullptr) {
        absolute = entry->absolute_path;
    } else if (!resolve_existing_file(raw, absolute)) {
        return;  // leave argument unchanged
    }

    // Ensure bag has the image if we're going to touch it.
    if (entry == nullptr || (prefer_base64(server, key, caps) && entry->base64_data.empty())) {
        const agent::AttachmentEntry* loaded = nullptr;
        Error err =
            load_image_into_bag(bag, absolute, raw, caps, cancellation, loaded);
        if (!err.ok()) {
            // Don't fail whole call for non-image paths that merely look path-like.
            if (err.code == ErrorCode::BadArgs) return;
            result.notes.push_back("skip " + key + ": " + err.message);
            return;
        }
        entry = loaded;
        absolute = entry->absolute_path;
    }

    const bool use_b64 = prefer_base64(server, key, caps);
    if (use_b64) {
        agent::AttachmentEntry* mut = bag.find_by_path_mut(absolute);
        if (mut == nullptr) return;
        Error err = bag.ensure_base64(*mut, caps.max_image_bytes, cancellation);
        if (!err.ok()) {
            result.notes.push_back("base64 load failed for " + key + ": " + err.message);
            return;
        }
        value = jstring(mut->base64_data);
        result.changed = true;
        result.notes.push_back("field " + key + ": path→base64 (" +
                               std::to_string(mut->byte_size) + " bytes, " + mut->mime_type +
                               ")");
        // Sibling mime field if empty/missing and name suggests image.
        if (object.is_object()) {
            for (auto& field : object.object) {
                if (name_suggests_mime(field.first) && field.second.is_string() &&
                    field.second.string.empty() && !mut->mime_type.empty()) {
                    field.second = jstring(mut->mime_type);
                    result.changed = true;
                }
            }
            // If schema-style object has no mime but has path-like key we converted,
            // optionally add mime_type when a free key is absent — skip inventing keys.
        }
    } else {
        // Normalize to absolute path for local stdio servers.
        if (value.string != absolute) {
            value = jstring(absolute);
            result.changed = true;
            result.notes.push_back("field " + key + ": path→absolute");
        }
    }
}

void rewrite_object(json::Value& obj,
                    const ServerConfig& server,
                    const ArgRewriteCaps& caps,
                    agent::AttachmentBag& bag,
                    runtime::CancellationToken cancellation,
                    ArgRewriteResult& result,
                    int depth) {
    if (!obj.is_object() || depth > 2) return;
    // Copy keys to allow mutation while iterating.
    std::vector<std::string> keys;
    keys.reserve(obj.object.size());
    for (const auto& kv : obj.object) keys.push_back(kv.first);
    for (const std::string& key : keys) {
        json::Value& value = obj.object[key];
        if (value.is_string()) {
            maybe_rewrite_string_field(obj, key, value, server, caps, bag, cancellation, result);
        } else if (value.is_object()) {
            rewrite_object(value, server, caps, bag, cancellation, result, depth + 1);
        } else if (value.is_array()) {
            for (json::Value& item : value.array) {
                if (item.is_object())
                    rewrite_object(item, server, caps, bag, cancellation, result, depth + 1);
                else if (item.is_string() && looks_like_path(item.string)) {
                    // Treat as path-like value without a key name → path for stdio, b64 for HTTP.
                    json::Value tmp_obj;
                    tmp_obj.type = json::Value::Type::Object;
                    tmp_obj.object["path"] = item;
                    maybe_rewrite_string_field(tmp_obj, "path", tmp_obj.object["path"], server,
                                               caps, bag, cancellation, result);
                    item = tmp_obj.object["path"];
                }
            }
        }
    }
}

}  // namespace

std::string redact_base64_in_json_text(const std::string& json_text) {
    // Cheap redaction: replace long base64-ish quoted strings.
    std::string out;
    out.reserve(json_text.size());
    for (std::size_t i = 0; i < json_text.size();) {
        if (json_text[i] != '"') {
            out.push_back(json_text[i++]);
            continue;
        }
        std::size_t j = i + 1;
        while (j < json_text.size()) {
            if (json_text[j] == '\\' && j + 1 < json_text.size()) {
                j += 2;
                continue;
            }
            if (json_text[j] == '"') break;
            ++j;
        }
        if (j >= json_text.size()) {
            out.append(json_text.substr(i));
            break;
        }
        const std::string inner = json_text.substr(i + 1, j - i - 1);
        bool b64 = inner.size() >= 128;
        if (b64) {
            for (char c : inner) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' ||
                      c == '=' || c == '\n' || c == '\r')) {
                    b64 = false;
                    break;
                }
            }
        }
        if (b64) {
            out += "\"[base64 ";
            out += std::to_string(inner.size());
            out += " chars]\"";
        } else {
            out.append(json_text, i, j - i + 1);
        }
        i = j + 1;
    }
    return out;
}

Error rewrite_mcp_arguments(const ServerConfig& server,
                            const std::string& /*tool_name*/,
                            const std::string& /*input_schema_json*/,
                            const std::string& arguments_json,
                            agent::AttachmentBag& bag,
                            const ArgRewriteCaps& caps,
                            runtime::CancellationToken cancellation,
                            ArgRewriteResult& out) {
    out = ArgRewriteResult{};
    out.arguments_json = arguments_json.empty() ? "{}" : arguments_json;
    out.history_arguments_json = out.arguments_json;

    json::ParseResult parsed = json::parse(out.arguments_json);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        // Leave non-objects alone.
        return ok_error();
    }

    rewrite_object(parsed.value, server, caps, bag, cancellation, out, 0);
    if (out.changed) {
        out.arguments_json = json::stringify(parsed.value);
        if (out.arguments_json.size() > caps.max_arguments_json_bytes) {
            return {ErrorCode::BadArgs,
                    "MCP tool arguments after attachment rewrite exceed " +
                        std::to_string(caps.max_arguments_json_bytes) +
                        " bytes (mcp_attachment_too_large)"};
        }
        out.history_arguments_json = redact_base64_in_json_text(out.arguments_json);
    }
    return ok_error();
}

}  // namespace ainiux::mcp
