#!/usr/bin/env node
"use strict";

// Optional benchmark adapter. This is not used by the ainiux runtime or tests.
const fs = require("fs");

if (process.argv.length !== 6) {
  console.error("usage: pptxgenjs_worker.cjs PptxGenJS.cjs INPUT.md OUTPUT.pptx COMPRESSION");
  process.exit(2);
}

const PptxGenJS = require(process.argv[2]);
const markdown = fs.readFileSync(process.argv[3], "utf8");
const output = process.argv[4];
const compression = process.argv[5] !== "off";

function splitSlides(text) {
  const slides = [];
  let current = [];
  let fenced = false;
  let marker = "";
  for (const line of text.replace(/\r\n/g, "\n").split("\n")) {
    const match = line.match(/^ {0,3}(`{3,}|~{3,})/);
    if (!fenced && line === "---") {
      slides.push(current.join("\n"));
      current = [];
      continue;
    }
    if (match) {
      const next = match[1][0];
      if (!fenced) {
        fenced = true;
        marker = next;
      } else if (next === marker) {
        fenced = false;
      }
    }
    current.push(line);
  }
  slides.push(current.join("\n"));
  return slides;
}

function plain(text) {
  return text
    .replace(/^<!-- ainiux-pptx[^\n]*-->\s*/m, "")
    .replace(/^#{1,6}\s+/gm, "")
    .replace(/^\s*(?:[-+*]|\d+[.)])\s+/gm, "")
    .replace(/!\[([^\]]*)\]\([^)]*\)/g, "$1")
    .replace(/\[([^\]]+)\]\([^)]*\)/g, "$1")
    .replace(/[*_~+`]/g, "");
}

async function main() {
  const pptx = new PptxGenJS();
  pptx.layout = "LAYOUT_WIDE";
  for (const source of splitSlides(markdown)) {
    const slide = pptx.addSlide();
    const lines = plain(source).trim().split("\n");
    const title = lines.length && lines[0] ? lines.shift() : "";
    if (title) slide.addText(title, { x: 0.6, y: 0.3, w: 12.1, h: 0.7, fontSize: 26, bold: true, breakLine: false });
    const body = lines.join("\n").trim();
    if (body) slide.addText(body, { x: 0.6, y: 1.15, w: 12.1, h: 5.8, fontSize: 16, breakLine: false, valign: "top", fit: "shrink" });
  }
  await pptx.writeFile({ fileName: output, compression });
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
