#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common.hpp"

namespace ainiux::editor {

struct FileFingerprint {
    bool exists = false;
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t size = 0;
    std::int64_t modified_seconds = 0;
    std::int64_t modified_nanoseconds = 0;
};

bool operator==(const FileFingerprint& left, const FileFingerprint& right);
bool operator!=(const FileFingerprint& left, const FileFingerprint& right);
Error fingerprint_file(const std::string& canonical_path, FileFingerprint& out);
Error canonicalize_editor_target(const std::string& path, std::string& out);

struct EditorLockOwner {
    unsigned schema_version = 0;
    std::string hostname;
    long long pid = 0;
    long long start_time = 0;
    std::string canonical_target;
    std::string token;
};

class EditorFileLock {
   public:
    // Instances are created by acquire_editor_file_lock. The constructor is
    // public only so std::shared_ptr can own the RAII object without a custom
    // allocation wrapper; callers must not construct locks directly.
    EditorFileLock(std::string canonical_target,
                   std::string lock_directory,
                   EditorLockOwner owner);
    ~EditorFileLock();
    EditorFileLock(const EditorFileLock&) = delete;
    EditorFileLock& operator=(const EditorFileLock&) = delete;

    const std::string& canonical_target() const { return canonical_target_; }
    const std::string& lock_directory() const { return lock_directory_; }
    const EditorLockOwner& owner() const { return owner_; }
    void release();

   private:
    std::string canonical_target_;
    std::string lock_directory_;
    EditorLockOwner owner_;
    bool owns_lock_ = true;
};

struct EditorLockAttempt {
    Error error;
    std::shared_ptr<EditorFileLock> lock;
    EditorLockOwner conflicting_owner;
    bool owner_metadata_valid = false;
    bool stale_lock_recovered = false;
};

EditorLockAttempt acquire_editor_file_lock(const std::string& path);
Error read_editor_lock_owner(const std::string& lock_directory, EditorLockOwner& out);
std::string editor_lock_owner_summary(const EditorLockAttempt& attempt);

}  // namespace ainiux::editor
