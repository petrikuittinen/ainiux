#include "chat/transcript.hpp"

#include <algorithm>
#include <cctype>

#include "markdown/blocks.hpp"
#include "output/thinking.hpp"
#include "xlsx/xlsx.hpp"

namespace ainiux::chat {
namespace {

std::string export_message_text(const provider::Message& message) {
    if (message.role == "assistant") {
        return output::split_thinking_traces(message.content).visible;
    }
    return message.content;
}

bool message_has_export_payload(const provider::Message& message) {
    return !export_message_text(message).empty() || !message.images.empty() ||
           !message.text_attachments.empty();
}

const provider::Message* last_exportable_message(const std::vector<provider::Message>& messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "thinking") {
            continue;
        }
        return &*it;
    }
    return nullptr;
}

const char* role_heading(const std::string& role) {
    if (role == "assistant") {
        return "Assistant";
    }
    if (role == "system") {
        return "System";
    }
    return "User";
}

void append_message_markdown(std::string& markdown, const provider::Message& message) {
    if (message.role == "thinking") {
        return;
    }
    const std::string body = export_message_text(message);
    markdown += "## ";
    markdown += role_heading(message.role);
    markdown += "\n\n";
    if (!body.empty()) {
        markdown += body;
        if (body.back() != '\n') {
            markdown += '\n';
        }
    }
    for (const provider::ImageInput& image : message.images) {
        markdown += "\n[image: ";
        markdown += image.display_name.empty() ? std::string("attached image") : image.display_name;
        markdown += "]\n";
    }
    for (const provider::TextAttachment& attachment : message.text_attachments) {
        markdown += "\nAttached: ";
        markdown += attachment.display_name.empty() ? std::string("attachment")
                                                    : attachment.display_name;
        markdown += '\n';
    }
    markdown += '\n';
}

std::string runs_text(const std::vector<markdown::Run>& runs) {
    std::string text;
    for (const markdown::Run& run : runs) {
        if (run.image_placeholder) {
            text += "[image: ";
            text += run.image_alt.empty() ? "image" : run.image_alt;
            text += ']';
        } else {
            text += run.text;
        }
        if (run.hard_break_after) text += '\n';
    }
    return text;
}

void append_csv_field(std::string& csv, const std::string& field) {
    const bool quote = field.find_first_of(",\"\r\n") != std::string::npos;
    if (!quote) {
        csv += field;
        return;
    }
    csv += '"';
    for (char ch : field) {
        if (ch == '"') csv += "\"\"";
        else csv += ch;
    }
    csv += '"';
}

}  // namespace

const char* transcript_format_name(TranscriptFormat format) {
    switch (format) {
        case TranscriptFormat::Json: return "json";
        case TranscriptFormat::Pdf: return "pdf";
        case TranscriptFormat::Docx: return "docx";
        case TranscriptFormat::Markdown: return "md";
        case TranscriptFormat::Xlsx: return "xlsx";
        case TranscriptFormat::Csv: return "csv";
    }
    return "";
}

Error parse_transcript_format(const std::string& name,
                              TranscriptScope scope,
                              TranscriptFormat& format) {
    std::string normalized = name;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "json") format = TranscriptFormat::Json;
    else if (normalized == "pdf") format = TranscriptFormat::Pdf;
    else if (normalized == "docx") format = TranscriptFormat::Docx;
    else if (normalized == "md") format = TranscriptFormat::Markdown;
    else if (normalized == "xlsx") format = TranscriptFormat::Xlsx;
    else if (normalized == "csv") format = TranscriptFormat::Csv;
    else return {ErrorCode::BadArgs, "unknown chat export format: " + name};
    if (scope == TranscriptScope::Thread &&
        (format == TranscriptFormat::Xlsx || format == TranscriptFormat::Csv)) {
        return {ErrorCode::BadArgs,
                std::string(transcript_format_name(format)) +
                    " export is available only with /export-last"};
    }
    return ok_error();
}

const char* default_transcript_path(TranscriptScope scope, TranscriptFormat format) {
    const bool last = scope == TranscriptScope::LastMessage;
    switch (format) {
        case TranscriptFormat::Json: return last ? "last.json" : "chat.json";
        case TranscriptFormat::Pdf: return last ? "last.pdf" : "chat.pdf";
        case TranscriptFormat::Docx: return last ? "last.docx" : "chat.docx";
        case TranscriptFormat::Markdown: return last ? "last.md" : "chat.md";
        case TranscriptFormat::Xlsx: return "last.xlsx";
        case TranscriptFormat::Csv: return "last.csv";
    }
    return "";
}

const char* default_transcript_pdf_path(TranscriptScope scope) {
    return default_transcript_path(scope, TranscriptFormat::Pdf);
}

const char* default_transcript_docx_path(TranscriptScope scope) {
    return default_transcript_path(scope, TranscriptFormat::Docx);
}

Error transcript_markdown(const std::vector<provider::Message>& messages,
                          const std::string& title,
                          TranscriptScope scope,
                          std::string& markdown) {
    markdown.clear();
    if (messages.empty()) {
        return {ErrorCode::BadArgs, "chat export needs at least one message"};
    }
    const provider::Message* last = nullptr;
    if (scope == TranscriptScope::LastMessage) {
        last = last_exportable_message(messages);
        if (last == nullptr || !message_has_export_payload(*last)) {
            return {ErrorCode::BadArgs, "the last chat message is empty"};
        }
    }

    markdown = "# ";
    markdown += title.empty() ? std::string("Chat") : title;
    markdown += "\n\n";
    if (scope == TranscriptScope::LastMessage) {
        append_message_markdown(markdown, *last);
    } else {
        for (const provider::Message& message : messages) {
            append_message_markdown(markdown, message);
        }
    }
    return ok_error();
}

Error transcript_json(Session session, TranscriptScope scope, std::string& json) {
    json.clear();
    if (session.messages.empty()) {
        return {ErrorCode::BadArgs, "chat export needs at least one message"};
    }
    if (scope == TranscriptScope::LastMessage) {
        const provider::Message* last = last_exportable_message(session.messages);
        if (last == nullptr || !message_has_export_payload(*last)) {
            return {ErrorCode::BadArgs, "the last chat message is empty"};
        }
        provider::Message kept = *last;
        session.messages.assign(1, std::move(kept));
        session.compaction_events.clear();
    }
    json = session_to_json(session);
    return ok_error();
}

Error transcript_pdf(const std::vector<provider::Message>& messages,
                     const std::string& title,
                     TranscriptScope scope,
                     pdf::WriteOptions& options,
                     std::string& pdf) {
    std::string markdown;
    Error err = transcript_markdown(messages, title, scope, markdown);
    if (!err.ok()) {
        return err;
    }
    return pdf::from_markdown(markdown, options, pdf);
}

Error transcript_docx(const std::vector<provider::Message>& messages,
                      const std::string& title,
                      TranscriptScope scope,
                      const docx::WriteOptions& options,
                      std::string& docx) {
    std::string markdown;
    Error err = transcript_markdown(messages, title, scope, markdown);
    if (!err.ok()) {
        return err;
    }
    return docx::from_markdown(markdown, options, docx);
}

Error transcript_xlsx(const std::vector<provider::Message>& messages,
                      const std::string& title,
                      TranscriptScope scope,
                      runtime::CancellationToken cancellation,
                      std::string& bytes) {
    if (scope != TranscriptScope::LastMessage) {
        return {ErrorCode::BadArgs, "XLSX export is available only for the last message"};
    }
    std::string markdown;
    Error err = transcript_markdown(messages, title, scope, markdown);
    if (!err.ok()) return err;
    xlsx::WriteOptions options;
    options.cancellation = cancellation;
    return xlsx::from_markdown(markdown, options, bytes);
}

Error transcript_csv(const std::vector<provider::Message>& messages,
                     const std::string& title,
                     TranscriptScope scope,
                     runtime::CancellationToken cancellation,
                     std::string& csv) {
    csv.clear();
    if (scope != TranscriptScope::LastMessage) {
        return {ErrorCode::BadArgs, "CSV export is available only for the last message"};
    }
    std::string transcript;
    Error err = transcript_markdown(messages, title, scope, transcript);
    if (!err.ok()) return err;
    const std::vector<markdown::Block> blocks = markdown::parse_blocks(transcript);
    const markdown::Block* table = nullptr;
    std::size_t count = 0;
    for (const markdown::Block& block : blocks) {
        if (cancellation.cancelled()) return {ErrorCode::Cancelled, "CSV export cancelled"};
        if (block.kind != markdown::BlockKind::Table || block.table_cells.empty()) continue;
        table = &block;
        ++count;
    }
    if (count == 0) {
        return {ErrorCode::BadArgs,
                "CSV export requires exactly one Markdown table in the last message; found none"};
    }
    if (count != 1) {
        return {ErrorCode::BadArgs,
                "CSV export requires exactly one Markdown table in the last message; found " +
                    std::to_string(count)};
    }
    for (const auto& row : table->table_cells) {
        for (std::size_t column = 0; column < row.size(); ++column) {
            if (column != 0) csv += ',';
            append_csv_field(csv, runs_text(row[column]));
        }
        csv += "\r\n";
    }
    return ok_error();
}

}  // namespace ainiux::chat
