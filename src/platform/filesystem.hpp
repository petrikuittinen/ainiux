#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common.hpp"

namespace ainiux::platform {

struct FileIdentity {
    std::uint64_t volume = 0;
    std::uint64_t file_low = 0;
    std::uint64_t file_high = 0;
    bool valid = false;
};

struct PathSafetyOptions {
    bool reject_drive_relative = true;
    bool reject_alternate_streams = true;
    bool reject_reserved_names = true;
    bool reject_reparse_points = false;
};

// Pure lexical Windows checks; available on every platform for unit tests.
Error validate_windows_path_syntax(const std::string& utf8_path,
                                   const PathSafetyOptions& options = {});

// Converts an external UTF-8 path to the native filesystem representation and
// returns project-relative storage using generic '/' separators.
std::string generic_path(const std::string& utf8_path);

Error file_identity(const std::string& utf8_path, FileIdentity& identity,
                    bool follow_links = true);
bool same_file_identity(const FileIdentity& left, const FileIdentity& right);
Error path_is_within(const std::string& root,
                     const std::string& candidate,
                     bool& within);
Error path_contains_link_or_reparse(const std::string& path, bool& contains);
Error path_is_link_or_reparse(const std::string& path, bool& is_link);
Error require_directory_access(const std::string& utf8_path,
                               bool require_read,
                               bool require_write);

Error ensure_private_directory(const std::string& utf8_path,
                               bool reject_reparse_points = false,
                               bool protect_existing = false);
Error create_private_directory_exclusive(const std::string& utf8_path,
                                         bool& created);
Error secure_random_hex(std::size_t byte_count, std::string& output);
Error create_private_file_if_missing(const std::string& utf8_path);
Error create_private_file_exclusive(const std::string& utf8_path, bool& created);
Error read_file_bounded(const std::string& utf8_path,
                        std::size_t limit,
                        std::string& output);
// Private content (credentials, chat, history under .ainiux-pr): mode 0600 / private DACL.
Error atomic_write_private(const std::string& utf8_path,
                           const std::string& data,
                           bool reject_reparse_points = false);
// Atomically publishes a new private file and fails if the target already
// exists, including when another process wins a concurrent create.
Error atomic_write_private_create(const std::string& utf8_path,
                                  const std::string& data,
                                  bool reject_reparse_points = false);
// Ordinary project/workspace content: preserve existing mode when overwriting;
// new files use 0666 so the process umask applies (typically 0644 or 0664).
// On Windows uses default inheritance (not the private user-only DACL).
Error atomic_write_shared(const std::string& utf8_path,
                          const std::string& data,
                          bool reject_reparse_points = false);
// Atomically publishes a new ordinary workspace file and fails if the target
// already exists, including when it appears during the operation.
Error atomic_write_shared_create(const std::string& utf8_path,
                                 const std::string& data,
                                 bool reject_reparse_points = false);
Error atomic_move(const std::string& from_utf8_path,
                  const std::string& to_utf8_path,
                  bool replace_existing = false);

// Best-effort durability flush for an existing file. Unlike atomic_write_private
// it does not change the file contents.
Error flush_file(const std::string& utf8_path);

#if defined(_WIN32)
namespace testing {
enum class AtomicWriteFault {
    None,
    DiskFull,
    ShortWrite,
};
// One-shot native fault injection used by the Windows filesystem suite.
void set_next_atomic_write_fault(AtomicWriteFault fault);
}  // namespace testing
#endif

}  // namespace ainiux::platform
