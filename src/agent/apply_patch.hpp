#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"

namespace ainiux::agent {

// OpenAI/Codex-style apply_patch envelope (V4A-ish):
//
//   *** Begin Patch
//   *** Add File: path
//   +line
//   *** Update File: path
//   @@ optional header
//    context
//   -old
//   +new
//   *** Delete File: path
//   *** End Patch
//
// Pure parse + in-memory hunk application. Workspace I/O and guards live in tools.

enum class PatchOpKind { AddFile, UpdateFile, DeleteFile };

struct PatchHunk {
    // Optional text after "@@". May include unified-diff line anchors such as
    // "-120,5 +120,8" (used as a match window) plus an optional function label.
    std::string header;
    // Raw body lines including the leading ' ', '-', or '+' (no trailing \n).
    std::vector<std::string> lines;
};

struct PatchFileOp {
    PatchOpKind kind = PatchOpKind::UpdateFile;
    std::string path;
    // For AddFile: full new file text (may be empty).
    std::string add_content;
    // For UpdateFile: one or more hunks applied top-to-bottom.
    std::vector<PatchHunk> hunks;
};

struct ParsedPatch {
    std::vector<PatchFileOp> ops;
};

// Parse a complete patch string. Accepts:
// - full envelope (*** Begin Patch … *** End Patch)
// - bare *** Update/Add/Delete File sections (no Begin/End; common with local models)
// - optional Markdown ``` fences around the body
// Returns BadArgs with a clear message on syntax errors.
Error parse_apply_patch(const std::string& patch_text, ParsedPatch& out);

// Apply update hunks to an existing file body. On success, result is the new
// content. match_modes collects per-hunk modes (exact / normalized_whitespace /
// indent_stripped) when fuzzy matching was used.
Error apply_patch_hunks(const std::string& original,
                        const std::vector<PatchHunk>& hunks,
                        bool allow_fuzzy,
                        std::string& result,
                        std::vector<std::string>& match_modes);

// Build the "old" and "new" text for a single hunk (used by tests and diagnostics).
// old_text is context+deleted lines; new_text is context+added lines. Lines are
// joined with '\n'; trailing_newline mirrors whether the original block should
// end with a newline when the source file does.
Error hunk_old_new_text(const PatchHunk& hunk,
                        std::string& old_text,
                        std::string& new_text);

}  // namespace ainiux::agent
