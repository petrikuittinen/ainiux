#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "pdf/pdf.hpp"
#include "provider/provider.hpp"

namespace ainiux::chat {

enum class TranscriptScope {
    Thread,
    LastMessage,
};

Error transcript_markdown(const std::vector<provider::Message>& messages,
                          const std::string& title,
                          TranscriptScope scope,
                          std::string& markdown);

Error transcript_pdf(const std::vector<provider::Message>& messages,
                     const std::string& title,
                     TranscriptScope scope,
                     pdf::WriteOptions& options,
                     std::string& pdf);

const char* default_transcript_pdf_path(TranscriptScope scope);

}  // namespace ainiux::chat
