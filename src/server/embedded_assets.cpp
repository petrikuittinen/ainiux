#include "server/embedded_assets.hpp"

#include "embedded_web_assets.hpp"
#include "embedded_web_editor_history.hpp"

namespace ainiux::server {

bool is_web_ui_path(std::string_view path) {
    return path == "/ui" || path == "/ui/" || path.rfind("/ui/", 0) == 0;
}

bool find_embedded_asset(std::string_view path, EmbeddedAsset& asset) {
    if (path == "/ui" || path == "/ui/" || path == "/ui/index.html") {
        asset = {"text/html; charset=utf-8", web::kIndexHtml};
        return true;
    }
    if (path == web::kStylesheetPath) {
        asset = {"text/css; charset=utf-8", web::kStylesheet};
        return true;
    }
    if (path == web::kJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web::kJavascript};
        return true;
    }
    if (path == web::kHighlightJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web::kHighlightJavascript};
        return true;
    }
    if (path == web::kSyntaxJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web::kSyntaxJavascript};
        return true;
    }
    if (path == web::kImageOptionsJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web::kImageOptionsJavascript};
        return true;
    }
    if (path == web::kVideoOptionsJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web::kVideoOptionsJavascript};
        return true;
    }
    if (path == web::kEditorHistoryJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web_history::kJavascript};
        return true;
    }
    if (path == web::kEditorIndentationJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web::kEditorIndentationJavascript};
        return true;
    }
    if (path == web::kSelectorJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web::kSelectorJavascript};
        return true;
    }
    return false;
}

}  // namespace ainiux::server
