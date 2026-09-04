import assert from "node:assert/strict";
import test from "node:test";

import { renderMarkdown } from "../../../src/web/js/highlight-v3.js";
import { canonicalLanguage } from "../../../src/web/js/syntax-v2.js";

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

test("recognizes every TUI fence language and compatibility alias", () => {
  const aliases = {
    text: ["text", "txt", "plain", "plaintext"],
    markdown: ["markdown", "md"], python: ["python", "py"], c: ["c"],
    cpp: ["cpp", "c++", "cxx"], csharp: ["csharp", "c#", "cs"], java: ["java"],
    javascript: ["javascript", "js"], typescript: ["typescript", "ts"],
    html: ["html", "html5", "html-multi", "htmlmulti"],
    htmlonly: ["htmlonly", "html-only"], css: ["css", "css3"], xml: ["xml"],
    json: ["json", "jsonl", "ndjson"], bash: ["bash", "sh", "shell"], php: ["php"],
    perl: ["perl", "pl"], ruby: ["ruby", "rb"], rust: ["rust", "rs"],
    go: ["go", "golang"], powershell: ["powershell", "pwsh", "ps1"],
    assembly: ["assembly", "assembler", "asm"], sql: ["sql"], toml: ["toml"],
    yaml: ["yaml", "yml"], ini: ["ini", "dosini"],
  };
  for (const [language, labels] of Object.entries(aliases)) {
    for (const label of labels) assert.equal(canonicalLanguage(label), language, label);
  }
});

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

test("highlights Java and C# fences", () => {
  const java = render("```java\npublic class Greeter {\n  String hello(int count) { return \"hi\"; }\n}\n```");
  assert.ok(syntaxText(java, "keyword").includes("public"));
  assert.ok(syntaxText(java, "type").includes("Greeter"));
  assert.ok(syntaxText(java, "type").includes("String"));
  assert.ok(syntaxText(java, "function").includes("hello"));
  assert.ok(syntaxText(java, "string").includes("\"hi\""));

  const csharp = render("```c#\npublic record User(string Name);\nbool Ready() => true; // done\n```");
  assert.ok(syntaxText(csharp, "keyword").includes("record"));
  assert.ok(syntaxText(csharp, "type").includes("User"));
  assert.ok(syntaxText(csharp, "type").includes("bool"));
  assert.ok(syntaxText(csharp, "function").includes("Ready"));
  assert.ok(syntaxText(csharp, "literal").includes("true"));
});

test("highlights XML and HTML-only markup without embedded-language coloring", () => {
  const xml = render("```xml\n<?xml version=\"1.0\"?>\n<root id=\"main\"><child /></root>\n```");
  assert.ok(syntaxText(xml, "preprocessor").includes("<?xml version=\"1.0\"?>"));
  assert.ok(syntaxText(xml, "tag").includes("root"));
  assert.ok(syntaxText(xml, "attribute").includes("id"));
  assert.ok(syntaxText(xml, "string").includes("main"));

  const htmlOnly = render("```htmlonly\n<script>const value = true;</script>\n```");
  assert.ok(syntaxText(htmlOnly, "tag").includes("script"));
  assert.equal(syntaxText(htmlOnly, "keyword").length, 0);
  assert.equal(syntaxText(htmlOnly, "literal").length, 0);
});

test("highlights JSON properties, literals, comments, and numbers", () => {
  const root = render("```jsonc\n{\"ready\": true, \"count\": 3, \"name\": \"Ainiux\"} // note\n```");
  assert.ok(syntaxText(root, "property").includes("\"ready\""));
  assert.ok(syntaxText(root, "literal").includes("true"));
  assert.ok(syntaxText(root, "number").includes("3"));
  assert.ok(syntaxText(root, "string").includes("\"Ainiux\""));
  assert.ok(syntaxText(root, "comment").includes("// note"));
});

test("highlights PHP, Perl, and Ruby fences", () => {
  const php = render("```php\n<?php function greet(string $name) { return \"Hi $name\"; } // note\n```");
  assert.ok(syntaxText(php, "preprocessor").includes("<?php"));
  assert.ok(syntaxText(php, "keyword").includes("function"));
  assert.ok(syntaxText(php, "function").includes("greet"));
  assert.ok(syntaxText(php, "type").includes("string"));
  assert.ok(syntaxText(php, "variable").includes("$name"));

  const perl = render("```perl\nsub greet { my ($name) = @_; return \"Hi\"; } # note\n```");
  assert.ok(syntaxText(perl, "keyword").includes("sub"));
  assert.ok(syntaxText(perl, "function").includes("greet"));
  assert.ok(syntaxText(perl, "variable").includes("$name"));
  assert.ok(syntaxText(perl, "comment").includes("# note"));

  const ruby = render("```rb\nclass Greeter\n  def call; @status = :ok; end\nend\n```");
  assert.ok(syntaxText(ruby, "type").includes("Greeter"));
  assert.ok(syntaxText(ruby, "function").includes("call"));
  assert.ok(syntaxText(ruby, "variable").includes("@status"));
  assert.ok(syntaxText(ruby, "literal").includes(":ok"));
});

test("highlights Rust, Go, and PowerShell fences", () => {
  const rust = render("```rust\npub fn main() { let value: Option<i32> = true; println!(\"ok\"); }\n/* outer /* nested */ done */\n```");
  assert.ok(syntaxText(rust, "keyword").includes("fn"));
  assert.ok(syntaxText(rust, "function").includes("main"));
  assert.ok(syntaxText(rust, "type").includes("Option"));
  assert.ok(syntaxText(rust, "function").includes("println"));
  assert.ok(syntaxText(rust, "comment").includes("/* outer /* nested */ done */"));

  const go = render("```go\nfunc main() { var names map[string]bool; names[\"x\"] = true }\n```");
  assert.ok(syntaxText(go, "keyword").includes("func"));
  assert.ok(syntaxText(go, "function").includes("main"));
  assert.ok(syntaxText(go, "type").includes("string"));
  assert.ok(syntaxText(go, "literal").includes("true"));

  const powershell = render("```pwsh\nfunction Get-Greeting([string]$Name) { Write-Host $Name -ForegroundColor Green }\n$ready = $true # note\n```");
  assert.ok(syntaxText(powershell, "keyword").includes("function"));
  assert.ok(syntaxText(powershell, "function").includes("Get-Greeting"));
  assert.ok(syntaxText(powershell, "type").includes("string"));
  assert.ok(syntaxText(powershell, "variable").includes("$Name"));
  assert.ok(syntaxText(powershell, "literal").includes("$true"));
});

test("highlights Assembly and SQL fences", () => {
  const assembly = render("```asm\n.global _start\n_start:\n  mov $1, %rax ; exit\n```");
  assert.ok(syntaxText(assembly, "preprocessor").includes(".global"));
  assert.ok(syntaxText(assembly, "function").includes("_start"));
  assert.ok(syntaxText(assembly, "keyword").includes("mov"));
  assert.ok(syntaxText(assembly, "type").includes("%rax"));
  assert.ok(syntaxText(assembly, "comment").includes("; exit"));

  const sql = render("```sql\nSELECT count(*) FROM users WHERE active = TRUE AND score > 10; -- note\n```");
  assert.ok(syntaxText(sql, "keyword").includes("SELECT"));
  assert.ok(syntaxText(sql, "function").includes("count"));
  assert.ok(syntaxText(sql, "literal").includes("TRUE"));
  assert.ok(syntaxText(sql, "number").includes("10"));
  assert.ok(syntaxText(sql, "comment").includes("-- note"));
});

test("highlights TOML, YAML, INI, Markdown, and plain-text fences", () => {
  const toml = render("```toml\n[server]\nport = 8080\nenabled = true # note\n```");
  assert.ok(syntaxText(toml, "heading").includes("[server]"));
  assert.ok(syntaxText(toml, "property").includes("port"));
  assert.ok(syntaxText(toml, "literal").includes("true"));

  const yaml = render("```yaml\nservice: &main\n  enabled: true\n  description: |\n    two lines\n```");
  assert.ok(syntaxText(yaml, "property").includes("service"));
  assert.ok(syntaxText(yaml, "variable").includes("&main"));
  assert.ok(syntaxText(yaml, "literal").includes("true"));
  assert.ok(syntaxText(yaml, "string").includes("    two lines"));

  const ini = render("```ini\n[server]\nport=8080\n; note\n```");
  assert.ok(syntaxText(ini, "heading").includes("[server]"));
  assert.ok(syntaxText(ini, "property").includes("port"));
  assert.ok(syntaxText(ini, "comment").includes("; note"));

  const markdown = render("````markdown\n# Title\n**bold** and [link](https://example.test)\n````");
  assert.ok(syntaxText(markdown, "heading").includes("# Title"));
  assert.ok(syntaxText(markdown, "emphasis").includes("**bold**"));
  assert.ok(syntaxText(markdown, "link").includes("[link](https://example.test)"));

  const plain = render("```text\nconst untouched = true;\n```");
  assert.equal(descendants(plain, "SPAN").length, 0);
  assert.equal(plain.textContent, "const untouched = true;");
});
