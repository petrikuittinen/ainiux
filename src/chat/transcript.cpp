#include "chat/transcript.hpp"

namespace ainiux::chat {
namespace {

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
    markdown += "## ";
    markdown += role_heading(message.role);
    markdown += "\n\n";
    if (!message.content.empty()) {
        markdown += message.content;
        if (message.content.back() != '\n') {
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

Error transcript_markdown(const std::vector<provider::Message>& messages,
                          const std::string& title,
                          TranscriptScope scope,
                          std::string& markdown) {
    markdown.clear();
    if (messages.empty()) {
        return {ErrorCode::BadArgs, "chat PDF export needs at least one message"};
    }
    if (scope == TranscriptScope::LastMessage && messages.back().content.empty() &&
        messages.back().images.empty() && messages.back().text_attachments.empty()) {
        return {ErrorCode::BadArgs, "the last chat message is empty"};
    }

    markdown = "# ";
    markdown += title.empty() ? std::string("Chat") : title;
    markdown += "\n\n";
    if (scope == TranscriptScope::LastMessage) {
        append_message_markdown(markdown, messages.back());
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

}  // namespace ainiux::chat
