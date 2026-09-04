import assert from "node:assert/strict";
import test from "node:test";

import { renderMarkdown } from "../../../src/web/js/highlight-v2.js";

class FakeNode {
  constructor(type, name = "", value = "") {
    this.nodeType = type;
    this.tagName = name.toUpperCase();
    this.nodeValue = value;
    this.childNodes = [];
    this.attributes = new Map();
    this.className = "";
  }

  append(...nodes) {
    for (const node of nodes) {
      if (node.nodeType === 11) this.childNodes.push(...node.childNodes);
      else this.childNodes.push(node);
    }
  }

  setAttribute(name, value) {
    this.attributes.set(name, String(value));
  }

  get textContent() {
    if (this.nodeType === 3) return this.nodeValue;
    return this.childNodes.map((child) => child.textContent).join("");
  }
}

const fakeDocument = {
  createDocumentFragment: () => new FakeNode(11),
  createElement: (tag) => new FakeNode(1, tag),
  createTextNode: (text) => new FakeNode(3, "", String(text)),
};

function descendants(node, tag = "") {
  const wanted = tag.toUpperCase();
  const found = [];
  for (const child of node.childNodes) {
    if (child.nodeType === 1 && (!wanted || child.tagName === wanted)) found.push(child);
    found.push(...descendants(child, tag));
  }
  return found;
}

function render(text) {
  return renderMarkdown(text, fakeDocument);
}

function syntaxText(root, role) {
  return descendants(root)
    .filter((node) => node.className === `syntax-${role}`)
    .map((node) => node.textContent);
}

test("renders ATX and Setext headings as semantic heading elements", () => {
  const root = render("# One\n\n## Two\n\n### C#\n\nSetext one\n===\n\nSetext two\n---");
  assert.deepEqual(descendants(root).filter((node) => /^H[1-6]$/.test(node.tagName))
    .map((node) => [node.tagName, node.textContent]), [
    ["H1", "One"], ["H2", "Two"], ["H3", "C#"],
    ["H1", "Setext one"], ["H2", "Setext two"],
  ]);
});

test("renders the safe core Markdown elements", () => {
  const root = render("Paragraph with **bold**, *em*, ~~gone~~, and `code`.\n\n" +
    "> quoted\n\n- parent\n  - child\n\n1. first\n2. second\n\n---\n\n" +
    "```python\nprint('hello')\n```");
  for (const tag of ["P", "STRONG", "EM", "DEL", "CODE", "BLOCKQUOTE", "UL", "OL", "HR", "PRE"]) {
    assert.ok(descendants(root, tag).length, `expected ${tag}`);
  }
  const fenced = descendants(root, "PRE")[0].childNodes[0];
  assert.equal(fenced.attributes.get("data-language"), "python");
  assert.equal(fenced.textContent, "print('hello')");
});

test("renders aligned GFM tables with inline formatting", () => {
  const root = render("| Name | Value | Note |\n| :--- | ---: | :---: |\n| A | **2** | `x|y` |");
  assert.equal(descendants(root, "TABLE").length, 1);
  assert.equal(descendants(root, "THEAD").length, 1);
  assert.equal(descendants(root, "TBODY").length, 1);
  assert.deepEqual(descendants(root, "TH").map((node) => node.className), [
    "md-align-left", "md-align-right", "md-align-center",
  ]);
  assert.deepEqual(descendants(root, "TD").map((node) => node.textContent), ["A", "2", "x|y"]);
  const wrapper = descendants(root).find((node) => node.className === "markdown-table-scroll");
  assert.equal(wrapper.attributes.get("role"), "region");
  assert.equal(wrapper.attributes.get("tabindex"), "0");
});

test("creates only safe absolute HTTP links", () => {
  const root = render("[Docs](https://example.test/a?q=1 \"Title\") and " +
    "http://example.test/b. [bad](javascript:alert(1)) [relative](/local) " +
    "[credentials](https://user:pass@example.test/) <https://example.test/c>");
  const anchors = descendants(root, "A");
  assert.equal(anchors.length, 3);
  assert.deepEqual(anchors.map((node) => node.textContent), [
    "Docs", "http://example.test/b", "https://example.test/c",
  ]);
  for (const anchor of anchors) {
    assert.equal(anchor.attributes.get("target"), "_blank");
    assert.equal(anchor.attributes.get("rel"), "noopener noreferrer");
    assert.equal(anchor.attributes.get("referrerpolicy"), "no-referrer");
  }
  assert.match(root.textContent, /\[bad\]\(javascript:alert\(1\)\)/);
  assert.match(root.textContent, /\[relative\]\(\/local\)/);
  assert.match(root.textContent, /\[credentials\]/);
});

test("keeps raw HTML and image Markdown inert", () => {
  const root = render("<script>alert('x')</script> <img src=https://example.test/x onerror=alert(1)> " +
    "![remote](https://example.test/image.png)");
  assert.equal(descendants(root, "SCRIPT").length, 0);
  assert.equal(descendants(root, "IMG").length, 0);
  assert.equal(descendants(root, "A").length, 0);
  assert.match(root.textContent, /<script>/);
  assert.match(root.textContent, /!\[remote\]\(https:\/\/example\.test\/image\.png\)/);
});

test("preserves incomplete streaming input and Unicode", () => {
  const openFence = render("```js\nconst emoji = '👨‍👩‍👧‍👦';");
  assert.equal(descendants(openFence, "PRE")[0].textContent, "const emoji = '👨‍👩‍👧‍👦';");
  const incomplete = render("Before **unfinished and [link](https://example.test");
  assert.equal(incomplete.textContent, "Before **unfinished and [link](https://example.test");
  const identifiers = render("window_width_max and snake_case");
  assert.equal(descendants(identifiers, "EM").length, 0);
  assert.equal(identifiers.textContent, "window_width_max and snake_case");
});

test("handles long literal input without recursion or data loss", () => {
  const source = `${"a".repeat(100000)} 世界`;
  assert.equal(render(source).textContent, source);
});

test("bounds adversarial delimiter scans and block nesting", () => {
  const brackets = "[".repeat(50000);
  assert.equal(render(brackets).textContent, brackets);
  const nestedQuote = `${"> ".repeat(1000)}deep`;
  assert.match(render(nestedQuote).textContent, /deep$/);
});

test("highlights JavaScript and TypeScript fences with TUI semantic roles", () => {
  const javascript = render("```js\nconst answer = greet(\"hi\", 42); // note\nconst ready = true;\n```");
  assert.ok(syntaxText(javascript, "keyword").includes("const"));
  assert.ok(syntaxText(javascript, "function").includes("greet"));
  assert.ok(syntaxText(javascript, "string").includes("\"hi\""));
  assert.ok(syntaxText(javascript, "number").includes("42"));
  assert.ok(syntaxText(javascript, "literal").includes("true"));
  assert.ok(syntaxText(javascript, "comment").includes("// note"));

  const typescript = render("```typescript\ninterface User { active: boolean; }\nconst value: User = true;\n```");
  assert.ok(syntaxText(typescript, "keyword").includes("interface"));
  assert.ok(syntaxText(typescript, "type").includes("User"));
  assert.ok(syntaxText(typescript, "type").includes("boolean"));
  assert.ok(syntaxText(typescript, "literal").includes("true"));
});

test("highlights Python, C, and C++ fences", () => {
  const python = render("```py\ndef greet(name: str):\n    return f\"Hi {name}\" # note\nvalue = None\n```");
  assert.ok(syntaxText(python, "keyword").includes("def"));
  assert.ok(syntaxText(python, "function").includes("greet"));
  assert.ok(syntaxText(python, "type").includes("str"));
  assert.ok(syntaxText(python, "string").includes("f\"Hi {name}\""));
  assert.ok(syntaxText(python, "literal").includes("None"));
  assert.ok(syntaxText(python, "comment").includes("# note"));

  const c = render("```c\n#include <stdio.h>\nint main(void) { return 0; } // ok\n```");
  assert.deepEqual(syntaxText(c, "preprocessor"), ["#include <stdio.h>"]);
  assert.ok(syntaxText(c, "type").includes("int"));
  assert.ok(syntaxText(c, "function").includes("main"));
  assert.ok(syntaxText(c, "keyword").includes("return"));
  assert.ok(syntaxText(c, "comment").includes("// ok"));

  const cpp = render("```c++\nclass Box { public: constexpr int size() { return 2; } };\nauto empty = nullptr;\n```");
  assert.ok(syntaxText(cpp, "type").includes("Box"));
  assert.ok(syntaxText(cpp, "keyword").includes("constexpr"));
  assert.ok(syntaxText(cpp, "function").includes("size"));
  assert.ok(syntaxText(cpp, "literal").includes("nullptr"));
});

test("highlights CSS and Bash fences", () => {
  const css = render("```css\n@media screen {\n  color: #fff;\n  margin: 1rem; /* note */\n}\n```");
  assert.ok(syntaxText(css, "keyword").includes("@media"));
  assert.ok(syntaxText(css, "property").includes("color"));
  assert.ok(syntaxText(css, "literal").includes("#fff"));
  assert.ok(syntaxText(css, "number").includes("1rem"));
  assert.ok(syntaxText(css, "comment").includes("/* note */"));

  const bash = render("```bash\n#!/usr/bin/env bash\nfor file in *.txt; do\n  echo \"$file\" # note\ndone\n```");
  assert.deepEqual(syntaxText(bash, "preprocessor"), ["#!/usr/bin/env bash"]);
  assert.ok(syntaxText(bash, "keyword").includes("for"));
  assert.ok(syntaxText(bash, "function").includes("echo"));
  assert.ok(syntaxText(bash, "string").includes("\"$file\""));
  assert.ok(syntaxText(bash, "comment").includes("# note"));
});

test("HTML fences highlight markup plus embedded CSS and JavaScript", () => {
  const source = "<style>body { color: #fff; }</style>\n" +
    "<script>const count = 3; // embedded\nalert(count);</script>\n" +
    "<button style=\"margin: 1rem\" onclick=\"run()\">Go</button>";
  const root = render(`\`\`\`html\n${source}\n\`\`\``);
  assert.equal(root.textContent, source);
  assert.ok(syntaxText(root, "tag").includes("style"));
  assert.ok(syntaxText(root, "attribute").includes("onclick"));
  assert.ok(syntaxText(root, "property").includes("color"));
  assert.ok(syntaxText(root, "literal").includes("#fff"));
  assert.ok(syntaxText(root, "keyword").includes("const"));
  assert.ok(syntaxText(root, "number").includes("3"));
  assert.ok(syntaxText(root, "comment").includes("// embedded"));
  assert.ok(syntaxText(root, "function").includes("run"));
  assert.equal(descendants(root, "STYLE").length, 0);
  assert.equal(descendants(root, "SCRIPT").length, 0);
  assert.equal(descendants(root, "BUTTON").length, 0);

  const multilineTag = render("```html\n<script\n type=\"module\">\nconst ready = true;\n</script>\n```");
  assert.ok(syntaxText(multilineTag, "attribute").includes("type"));
  assert.ok(syntaxText(multilineTag, "keyword").includes("const"));
  assert.ok(syntaxText(multilineTag, "literal").includes("true"));
});

test("unknown fences remain literal and supported multiline states are preserved", () => {
  const unknown = render("```brainfuck\n++[>++<-]\n```");
  assert.equal(syntaxText(unknown, "operator").length, 0);
  assert.equal(unknown.textContent, "++[>++<-]");

  const multiline = render("```js\n/* first\nstill comment */\nconst value = `two\nlines`;\n```");
  assert.deepEqual(syntaxText(multiline, "comment"), ["/* first", "still comment */"]);
  assert.deepEqual(syntaxText(multiline, "string"), ["`two", "lines`"]);
  assert.ok(syntaxText(multiline, "keyword").includes("const"));
});
