#pragma once

#include <cstddef>
#include <cstdint>

namespace ainiux::pdf {

constexpr std::size_t kMaxTokenBytes = 131072;
constexpr std::size_t kMaxWhitespaceRun = 2048;
constexpr std::size_t kMaxDepth = 32;
constexpr std::size_t kMaxXrefPrev = 100;
constexpr std::size_t kMaxObjects = 1000000;
constexpr std::size_t kMaxObjStmObjects = 16384;
constexpr std::size_t kMaxObjStreams = 16384;
constexpr std::size_t kMaxDecodedStream = 64 * 1024 * 1024;
constexpr std::size_t kDefaultMaxBytes = 64 * 1024 * 1024;
constexpr std::size_t kStartxrefWindow = 1024;

}  // namespace ainiux::pdf
