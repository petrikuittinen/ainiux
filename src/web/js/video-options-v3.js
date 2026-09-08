const uniqueStrings = (values) => [...new Set((Array.isArray(values) ? values : [])
  .filter((value) => typeof value === "string" && value))];

export function normalizeVideoCatalog(value) {
  const source = value && typeof value === "object" ? value : {};
  const limits = source.limits && typeof source.limits === "object" ? source.limits : {};
  const models = (Array.isArray(source.models) ? source.models : []).filter((model) =>
    model && typeof model === "object" && typeof model.provider === "string" &&
    typeof model.model === "string" && model.model && Array.isArray(model.settings));
  return {
    default_provider: typeof source.default_provider === "string" ? source.default_provider : "fal",
    providers: uniqueStrings(source.providers),
    models,
    limits: {
      max_inputs: Number(limits.max_inputs) || 50,
      max_input_bytes: Number(limits.max_input_bytes) || 200 * 1024 * 1024,
      max_image_bytes: Number(limits.max_image_bytes) || 30 * 1024 * 1024,
      max_audio_bytes: Number(limits.max_audio_bytes) || 15 * 1024 * 1024,
      max_total_bytes: Number(limits.max_total_bytes) || 1024 * 1024 * 1024,
    },
  };
}

export function modelsForVideoProvider(catalog, provider) {
  const effective = provider || catalog.default_provider;
  return catalog.models.filter((model) => model.provider === effective || model.provider === "any");
}

export function selectVideoModel(catalog, provider, requested = "") {
  const models = modelsForVideoProvider(catalog, provider);
  return models.find((model) => model.model === requested) ||
    models.find((model) => model.default === true) || models[0] || null;
}

export function cropVideoFileName(name, max = 16) {
  const value = typeof name === "string" ? name : "";
  if (value.length <= max) return value;
  return `${value.slice(0, max)}…`;
}

function fileFromVideoInput(input) {
  return input && input.file ? input.file : input;
}

export function videoInputCounts(values) {
  const counts = { images: 0, videos: 0, audios: 0, total: 0 };
  for (const input of Array.from(values || [])) {
    const file = fileFromVideoInput(input);
    if (!file || typeof file.type !== "string") continue;
    counts.total += 1;
    if (file.type.startsWith("image/")) counts.images += 1;
    else if (file.type.startsWith("video/")) counts.videos += 1;
    else if (file.type.startsWith("audio/")) counts.audios += 1;
  }
  return counts;
}

function listedMediaCounts(images, videos, audios) {
  const parts = [];
  if (images) parts.push(`${images} ${images === 1 ? "image" : "images"}`);
  if (videos) parts.push(`${videos} ${videos === 1 ? "video" : "videos"}`);
  if (audios) parts.push(`${audios} audio`);
  return parts.join(", ");
}

export function videoInputStatus(model, inputs = []) {
  if (!model) return "No configured video model is available.";
  if (model.input_mode === "text") return "Text-to-video only.";
  const selected = videoInputCounts(inputs);
  const selectedText = selected.total
    ? `${listedMediaCounts(selected.images, selected.videos, selected.audios)} selected. `
    : "";
  const limitText = listedMediaCounts(
    Number(model.max_input_images || 0),
    Number(model.max_input_videos || 0),
    Number(model.max_input_audios || 0),
  );
  const mode = model.input_mode === "reference" ? "Reference-to-video"
    : model.input_mode === "mixed"
      ? (Number(model.max_input_videos || 0) || Number(model.max_input_audios || 0)
        ? "Text, image, or reference-to-video" : "Text or image-to-video")
      : "Image-to-video";
  if (!limitText) return `${selectedText}${mode}.`;
  return `${selectedText}${mode}. Up to ${limitText}.`;
}

export function videoFileError(model, values, limits) {
  if (!model) return "No configured video model is available.";
  const inputs = Array.from(values || []);
  let images = 0;
  let videos = 0;
  let audios = 0;
  let total = 0;
  for (const input of inputs) {
    const file = input && input.file ? input.file : input;
    if (!file || typeof file.type !== "string" || !Number.isFinite(file.size) || file.size <= 0) {
      return "Every reference must be a non-empty image, video, or audio file.";
    }
    total += file.size;
    if (file.type.startsWith("image/")) {
      images += 1;
      const maximum = Number(model.max_input_image_bytes || 0) || limits.max_image_bytes;
      if (file.size > maximum) return `${file.name || "Image"} exceeds the image input size limit.`;
    } else if (file.type.startsWith("video/")) {
      videos += 1;
      const maximum = Number(model.max_input_video_bytes || 0) || limits.max_input_bytes;
      if (file.size > maximum) return `${file.name || "Video"} exceeds the video input size limit.`;
    } else if (file.type.startsWith("audio/")) {
      audios += 1;
      const maximum = Number(model.max_input_audio_bytes || 0) || limits.max_audio_bytes;
      if (file.size > maximum) return `${file.name || "Audio"} exceeds the audio input size limit.`;
    } else return `${file.name || "File"} has an unsupported media type.`;
  }
  if (inputs.length > limits.max_inputs) return "Too many input files.";
  if (total > limits.max_total_bytes) return "Selected media exceeds the combined input size limit.";
  if (model.input_mode === "text" && inputs.length) return "This text-to-video model does not accept input media.";
  if (images > Number(model.max_input_images || 0) ||
      videos > Number(model.max_input_videos || 0) ||
      audios > Number(model.max_input_audios || 0) ||
      (Number(model.max_input_total || 0) > 0 && inputs.length > Number(model.max_input_total))) {
    return "Selected media exceeds this model's input limit.";
  }
  if (model.input_mode === "image" && images < 1) return "This image-to-video model requires a start image.";
  if (model.input_mode === "reference" && images + videos < 1) return "Reference-to-video requires an image or video (audio cannot be used alone).";
  return "";
}
