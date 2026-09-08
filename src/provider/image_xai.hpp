#pragma once

#include "provider/image.hpp"

namespace ainiux::provider {

Error serialize_xai_imagine_request(const ImageGenerateRequest& request, std::string& body);
Error parse_xai_imagine_response(const std::string& body, ImageGenerateResult& result);
Error generate_xai_imagine_image(const RequestContext& context,
                                 const ImageGenerateRequest& request,
                                 ImageGenerateResult& result,
                                 runtime::CancellationToken cancellation);

}  // namespace ainiux::provider
