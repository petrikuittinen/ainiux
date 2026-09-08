import test from "node:test";
import assert from "node:assert/strict";

import {
  normalizeVideoCatalog, modelsForVideoProvider, selectVideoModel, videoFileError,
  cropVideoFileName, videoInputStatus,
} from "../../../src/web/js/video-options-v3.js";

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
    { provider: "replicate", model: "p-video", input_mode: "mixed",
      max_input_images: 2, max_input_videos: 0, max_input_audios: 1, settings: [] },
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
  assert.equal(videoFileError(catalog.models[2], [], catalog.limits), "");
  assert.equal(videoFileError(catalog.models[2], [image], catalog.limits), "");
  assert.match(videoFileError(catalog.models[2], [image, image, image],
    { ...catalog.limits, max_total_bytes: 100 }), /model's input limit/);
});

test("video file names crop to the first 16 characters", () => {
  assert.equal(cropVideoFileName("short.png"), "short.png");
  assert.equal(cropVideoFileName("petrikuittinen_beautiful_Japanese.png"), "petrikuittinen_b…");
  assert.equal(cropVideoFileName(""), "");
});

test("video status reports selected media separately from model capacity", () => {
  const imageModel = {
    input_mode: "image", max_input_images: 2, max_input_videos: 0, max_input_audios: 0,
  };
  assert.equal(videoInputStatus(imageModel, []), "Image-to-video. Up to 2 images.");
  assert.equal(videoInputStatus(imageModel, [{ name: "a.png", type: "image/png", size: 1 }]),
    "1 image selected. Image-to-video. Up to 2 images.");
  assert.equal(videoInputStatus(catalog.models[0]), "Text-to-video only.");
  assert.equal(videoInputStatus(catalog.models[1], [
    { name: "in.png", type: "image/png", size: 10 },
    { file: { name: "in.mp4", type: "video/mp4", size: 15 } },
    { name: "in.wav", type: "audio/wav", size: 5 },
  ]), "1 image, 1 video, 1 audio selected. Reference-to-video. Up to 3 images, 2 videos, 1 audio.");
  assert.equal(videoInputStatus(catalog.models[2], []),
    "Text, image, or reference-to-video. Up to 2 images, 1 audio.");
  assert.equal(videoInputStatus(catalog.models[2], [{ name: "a.png", type: "image/png", size: 1 }]),
    "1 image selected. Text, image, or reference-to-video. Up to 2 images, 1 audio.");
});
