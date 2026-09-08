import test from "node:test";
import assert from "node:assert/strict";

import {
  normalizeVideoCatalog, modelsForVideoProvider, selectVideoModel, videoFileError,
} from "../../../src/web/js/video-options-v1.js";

const catalog = normalizeVideoCatalog({
  default_provider: "fal",
  providers: ["fal", "fal"],
  limits: { max_inputs: 4, max_input_bytes: 20, max_image_bytes: 10,
    max_audio_bytes: 5, max_total_bytes: 30 },
  models: [
    { provider: "fal", model: "text", default: true, input_mode: "text",
      max_input_images: 0, max_input_videos: 0, max_input_audios: 0, settings: [] },
    { provider: "fal", model: "reference", input_mode: "reference",
      max_input_images: 3, max_input_videos: 2, max_input_audios: 1,
      max_input_total: 3, settings: [] },
  ],
});

test("video catalog normalization and default selection are deterministic", () => {
  assert.deepEqual(catalog.providers, ["fal"]);
  assert.deepEqual(modelsForVideoProvider(catalog, "fal").map((item) => item.model),
    ["text", "reference"]);
  assert.equal(selectVideoModel(catalog, "", "missing").model, "text");
});

test("video media validation enforces mode, per-type, aggregate, and model limits", () => {
  const reference = catalog.models[1];
  const image = { name: "in.png", type: "image/png", size: 10 };
  const video = { name: "in.mp4", type: "video/mp4", size: 15 };
  const audio = { name: "in.wav", type: "audio/wav", size: 5 };
  assert.equal(videoFileError(reference, [image, video], catalog.limits), "");
  assert.match(videoFileError(reference, [audio], catalog.limits), /image or video/);
  assert.match(videoFileError(reference, [image, video, audio, image],
    { ...catalog.limits, max_total_bytes: 100 }), /model's input limit/);
  assert.match(videoFileError(reference, [{ ...image, size: 11 }], catalog.limits), /image input size/);
  assert.match(videoFileError({ ...reference, max_input_image_bytes: 9 }, [image], catalog.limits),
    /image input size/);
  assert.match(videoFileError(catalog.models[0], [image], catalog.limits), /does not accept/);
});
