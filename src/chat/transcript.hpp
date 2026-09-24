#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "docx/docx.hpp"
#include "pdf/pdf.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"
#include "chat/session.hpp"

namespace ainiux::chat {

enum class TranscriptScope {
    Thread,
    LastMessage,
};

enum class TranscriptFormat { Json, Pdf, Docx, Markdown, Xlsx, Csv };

Error parse_transcript_format(const std::string& name,
                              TranscriptScope scope,
                              TranscriptFormat& format);
const char* transcript_format_name(TranscriptFormat format);
const char* default_transcript_path(TranscriptScope scope, TranscriptFormat format);

Error transcript_markdown(const std::vector<provider::Message>& messages,
                          const std::string& title,
                          TranscriptScope scope,
                          std::string& markdown);

Error transcript_json(Session session, TranscriptScope scope, std::string& json);

Error transcript_pdf(const std::vector<provider::Message>& messages,
                     const std::string& title,
                     TranscriptScope scope,
                     pdf::WriteOptions& options,
                     std::string& pdf);

Error transcript_docx(const std::vector<provider::Message>& messages,
                      const std::string& title,
                      TranscriptScope scope,
                      const docx::WriteOptions& options,
                      std::string& docx);

Error transcript_xlsx(const std::vector<provider::Message>& messages,
                      const std::string& title,
                      TranscriptScope scope,
                      runtime::CancellationToken cancellation,
                      std::string& xlsx);

Error transcript_csv(const std::vector<provider::Message>& messages,
                     const std::string& title,
                     TranscriptScope scope,
                     runtime::CancellationToken cancellation,
                     std::string& csv);

const char* default_transcript_pdf_path(TranscriptScope scope);
const char* default_transcript_docx_path(TranscriptScope scope);

}  // namespace ainiux::chat
