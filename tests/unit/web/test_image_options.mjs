import test from "node:test";
import assert from "node:assert/strict";

import {
  normalizeImageCatalog, modelsForProvider, selectImageModel,
  imageFileError, customDimensionError,
  resetImageFormValues,
} from "../../../src/web/js/image-options-v1.js";

const catalog = normalizeImageCatalog({
  default_provider: "openai",
  providers: ["openai", "fal"],
  limits: { max_input_images: 16, max_image_bytes: 20, max_total_image_bytes: 40 },
  models: [
    { provider: "openai", model: "image-a", default: true, edits: true,
      max_input_images: 2, custom_size: { enabled: true, multiple: 16,
        max_edge: 2048, min_pixels: 256, max_pixels: 4194304, max_ratio: 3 } },
    { provider: "fal", model: "image-b", default: true, edits: false,
      max_input_images: 0, custom_size: { enabled: false } },
  ],
});

test("catalog selection follows provider defaults", () => {
  assert.deepEqual(modelsForProvider(catalog, "fal").map((item) => item.model), ["image-b"]);
  assert.equal(selectImageModel(catalog, "", "").model, "image-a");
  assert.equal(selectImageModel(catalog, "fal", "missing").model, "image-b");
});

test("reference file validation enforces MIME, count, and byte totals", () => {
  const png = { type: "image/png", size: 20 };
  assert.equal(imageFileError([png, { type: "image/jpeg", size: 20 }], catalog.limits), "");
  assert.match(imageFileError([{ type: "image/gif", size: 1 }], catalog.limits), /PNG or JPEG/);
  assert.match(imageFileError([{ type: "image/png", size: 21 }], catalog.limits), /Each reference/);
  assert.match(imageFileError([png, png, png], { ...catalog.limits, max_input_images: 2 }), /at most 2/);
});

test("custom dimensions use catalog constraints", () => {
  const model = catalog.models[0];
  assert.equal(customDimensionError(model, 1024, 1024), "");
  assert.match(customDimensionError(model, 1025, 1024), /multiples of 16/);
  assert.match(customDimensionError(model, 2048, 512), /3:1 ratio/);
});

test("image reset clears generation fields while retaining provider and model", () => {
  assert.deepEqual(resetImageFormValues("fal", "fal-ai/flux/dev"), {
    provider: "fal",
    model: "fal-ai/flux/dev",
    prompt: "",
    size: "",
    aspect: "",
    quality: "",
    format: "",
    width: "1024",
    height: "1024",
  });
});
