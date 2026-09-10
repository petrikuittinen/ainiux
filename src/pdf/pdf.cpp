#include "pdf/pdf.hpp"

#include "pdf/document.hpp"
#include "pdf/extract.hpp"
#include "pdf/layout.hpp"

namespace ainiux::pdf {

Error to_markdown_file(const std::string& path, const Options& options, std::string& markdown) {
    markdown.clear();
    Document document;
    Error err = Document::open_file(path, options, document);
    if (!err.ok()) {
        return err;
    }
    return extract_markdown(document, options, markdown);
}

Error to_markdown_bytes(std::string_view pdf, const Options& options, std::string& markdown) {
    markdown.clear();
    Document document;
    Error err = Document::open_bytes(std::string(pdf), options, document);
    if (!err.ok()) {
        return err;
    }
    return extract_markdown(document, options, markdown);
}

Error from_markdown(std::string_view markdown, WriteOptions& options, std::string& pdf) {
    return layout_markdown(markdown, options, pdf);
}

}  // namespace ainiux::pdf
