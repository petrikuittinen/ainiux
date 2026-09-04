const uniqueStrings = (values) => [...new Set((Array.isArray(values) ? values : [])
  .filter((value) => typeof value === "string" && value))];

export function normalizeImageCatalog(value) {
  const source = value && typeof value === "object" ? value : {};
  const limits = source.limits && typeof source.limits === "object" ? source.limits : {};
  const models = (Array.isArray(source.models) ? source.models : []).filter((model) =>
    model && typeof model === "object" && typeof model.provider === "string" &&
    typeof model.model === "string" && model.model);
  return {
    default_provider: typeof source.default_provider === "string" ? source.default_provider : "",
    providers: uniqueStrings(source.providers),
    models,
    limits: {
      max_input_images: Number(limits.max_input_images) || 16,
      max_image_bytes: Number(limits.max_image_bytes) || 20 * 1024 * 1024,
      max_total_image_bytes: Number(limits.max_total_image_bytes) || 40 * 1024 * 1024,
    },
  };
}

export function modelsForProvider(catalog, provider) {
  const effective = provider || catalog.default_provider;
  return catalog.models.filter((model) => model.provider === effective || model.provider === "any");
}

export function selectImageModel(catalog, provider, requested = "") {
  const models = modelsForProvider(catalog, provider);
  return models.find((model) => model.model === requested) ||
    models.find((model) => model.default === true) || models[0] || null;
}

export function imageFileError(files, limits) {
  const values = Array.from(files || []);
  if (values.length > limits.max_input_images) {
    return `Choose at most ${limits.max_input_images} reference images.`;
  }
  let total = 0;
  for (const file of values) {
    if (!file || !["image/png", "image/jpeg"].includes(file.type)) {
      return "Reference images must be PNG or JPEG files.";
    }
    if (file.size <= 0 || file.size > limits.max_image_bytes) {
      return `Each reference image must be no larger than ${Math.round(limits.max_image_bytes / 1048576)} MiB.`;
    }
    total += file.size;
  }
  if (total > limits.max_total_image_bytes) {
    return `Reference images may total at most ${Math.round(limits.max_total_image_bytes / 1048576)} MiB.`;
  }
  return "";
}

export function customDimensionError(model, width, height) {
  const rules = model && model.custom_size ? model.custom_size : {};
  if (!rules.enabled) return "";
  const w = Number(width);
  const h = Number(height);
  if (!Number.isSafeInteger(w) || !Number.isSafeInteger(h) || w <= 0 || h <= 0) {
    return "Custom width and height must be positive whole numbers.";
  }
  const multiple = Number(rules.multiple) || 1;
  if (w % multiple || h % multiple) return `Width and height must be multiples of ${multiple}.`;
  if (rules.max_edge && (w > rules.max_edge || h > rules.max_edge)) {
    return `Width and height may not exceed ${rules.max_edge}px.`;
  }
  const pixels = w * h;
  if (rules.min_pixels && pixels < rules.min_pixels) return "Custom dimensions are below this model's pixel minimum.";
  if (rules.max_pixels && pixels > rules.max_pixels) return "Custom dimensions exceed this model's pixel maximum.";
  if (rules.max_ratio && Math.max(w / h, h / w) > rules.max_ratio) {
    return `Custom dimensions may not exceed a ${rules.max_ratio}:1 ratio.`;
  }
  return "";
}

export function resetImageFormValues(provider, model) {
  return {
    provider: String(provider || ""),
    model: String(model || ""),
    prompt: "",
    size: "",
    aspect: "",
    quality: "",
    format: "",
    width: "1024",
    height: "1024",
  };
}
