#include "chat/transcript.hpp"

#include "output/thinking.hpp"

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
    if (!message.text_attachments.empty()) {
        markdown += "\n# Attached Markdown\n\n";
        for (size_t i = 0; i < message.text_attachments.size(); ++i) {
            const provider::TextAttachment& attachment = message.text_attachments[i];
            markdown += "---";
            markdown += attachment.display_name.empty() ? std::string("attachment")
                                                        : attachment.display_name;
            markdown += "---\n";
            markdown += attachment.markdown_content;
            if (!attachment.markdown_content.empty() && attachment.markdown_content.back() != '\n') {
                markdown += '\n';
            }
            if (i + 1 < message.text_attachments.size()) {
                markdown += '\n';
            }
        }
    }
    markdown += '\n';
}

}  // namespace

const char* default_transcript_pdf_path(TranscriptScope scope) {
    return scope == TranscriptScope::LastMessage ? "last.pdf" : "chat.pdf";
}

const char* default_transcript_docx_path(TranscriptScope scope) {
    return scope == TranscriptScope::LastMessage ? "last.docx" : "chat.docx";
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

}  // namespace ainiux::chat
