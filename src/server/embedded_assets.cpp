#include "server/embedded_assets.hpp"

#include "embedded_web_assets.hpp"

namespace ainiux::server {

bool is_web_ui_path(std::string_view path) {
    return path == "/ui" || path == "/ui/" || path.rfind("/ui/", 0) == 0;
}

bool find_embedded_asset(std::string_view path, EmbeddedAsset& asset) {
    if (path == "/ui" || path == "/ui/" || path == "/ui/index.html") {
        asset = {"text/html; charset=utf-8", web::kIndexHtml, false};
        return true;
    }
    if (path == web::kStylesheetPath) {
        asset = {"text/css; charset=utf-8", web::kStylesheet, true};
        return true;
    }
    if (path == web::kJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web::kJavascript, true};
        return true;
    }
    if (path == web::kHighlightJavascriptPath) {
        asset = {"text/javascript; charset=utf-8", web::kHighlightJavascript, true};
        return true;
    }
    return false;
}

}  // namespace ainiux::server
