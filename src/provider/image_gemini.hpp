#pragma once

#include "provider/image.hpp"

namespace ainiux::provider {

Error generate_gemini_image(const RequestContext& context,
                            const ImageGenerateRequest& request,
                            ImageGenerateResult& result,
                            runtime::CancellationToken cancellation);

}  // namespace ainiux::provider
