import assert from "node:assert/strict";
import test from "node:test";

import {
  detectIndentation, indentEditorSnapshot, outdentEditorSnapshot, reformatEditorSnapshot,
} from "../../../src/web/js/editor-indentation-v1.js";
import { createEditorHistory, recordEditorChange, undoEditorChange } from
  "../../../src/web/js/editor-history-v2.js";
import { analyzeStructuralLine } from "../../../src/web/js/syntax-v4.js";

const snapshot = (value, selectionStart = 0, selectionEnd = selectionStart,
  selectionDirection = "none") => ({ value, selectionStart, selectionEnd, selectionDirection });

test("indentation detection matches the native first-twenty-line policy", () => {
  assert.deepEqual(detectIndentation(
    "function run() {\n  if (ready) {\n    call();\n  }\n}", 4, "spaces"), {
    tabWidth: 2, tabStyle: "spaces", tabWidthDetected: true, tabStyleDetected: true,
  });
  assert.deepEqual(detectIndentation("if ready\n\tcall\nend", 8, "spaces"), {
    tabWidth: 8, tabStyle: "tab", tabWidthDetected: false, tabStyleDetected: true,
  });
  assert.deepEqual(detectIndentation("top\n  child\n     inconsistent", 6, "tab"), {
    tabWidth: 6, tabStyle: "spaces", tabWidthDetected: false, tabStyleDetected: true,
  });
  assert.deepEqual(detectIndentation("one line", 4, "spaces"), {
    tabWidth: 4, tabStyle: "spaces", tabWidthDetected: false, tabStyleDetected: false,
  });
  const afterLimit = "top_level();\n".repeat(20) + "  ignored_after_limit();\n";
  assert.equal(detectIndentation(afterLimit, 5, "spaces").tabWidthDetected, false);
});

test("caret and block Tab operations preserve textarea selection semantics", () => {
  assert.deepEqual(indentEditorSnapshot(snapshot("ab", 2), 4, "spaces"), snapshot("ab  ", 4));
  assert.deepEqual(indentEditorSnapshot(snapshot("ab", 1), 4, "tab"), snapshot("a\tb", 2));

  const forward = indentEditorSnapshot(snapshot("a\nb\nc", 0, 4, "forward"), 4, "spaces");
  assert.deepEqual(forward, snapshot("    a\n    b\nc", 0, 12, "forward"));
  const reverse = indentEditorSnapshot(snapshot("a\nb\nc", 0, 4, "backward"), 2, "spaces");
  assert.deepEqual(reverse, snapshot("  a\n  b\nc", 0, 8, "backward"));

  const outdented = outdentEditorSnapshot(snapshot(" \t  alpha\n    beta", 0, 17, "forward"), 4);
  assert.equal(outdented.value, " \talpha\nbeta");
  assert.equal(outdented.selectionStart, 0);
  assert.equal(outdented.selectionDirection, "forward");
});

test("manual indentation records as one existing custom-history operation", () => {
  const before = snapshot("a\nb", 0, 3, "forward");
  const history = createEditorHistory(before);
  const after = indentEditorSnapshot(before, 2, "spaces");
  assert.equal(recordEditorChange(history, before, after), true);
  assert.equal(history.undo.length, 1);
  assert.deepEqual(undoEditorChange(history, after), before);
});

test("reformat ports every native language profile", () => {
  for (const language of ["c", "cpp", "csharp", "java", "javascript", "typescript",
    "css", "json", "php", "perl", "rust", "go", "powershell"]) {
    const result = reformatEditorSnapshot(snapshot("if (ready) {\nvalue();\n}\n"),
      language, 4, "spaces");
    assert.equal(result.value, "if (ready) {\n    value();\n}\n", language);
  }
  const fixtures = [
    ["ruby", "if ready\nputs value\nelse\nputs other\nend\nitems.each do\nputs value\nend",
      "if ready\n    puts value\nelse\n    puts other\nend\nitems.each do\n    puts value\nend"],
    ["bash", "if ready; then\necho yes\nelse\necho no\nfi",
      "if ready; then\n    echo yes\nelse\n    echo no\nfi"],
    ["html", "<main>\n<script>\nfunction run() {\ncall();\n}\n</script>\n</main>",
      "<main>\n    <script>\n        function run() {\n            call();\n        }\n    </script>\n</main>"],
    ["htmlonly", "<main>\n<br>\n<span>x</span>\n</main>",
      "<main>\n    <br>\n    <span>x</span>\n</main>"],
    ["xml", "<root>\n<item>text</item>\n</root>",
      "<root>\n    <item>text</item>\n</root>"],
    ["sql", "BEGIN\nSELECT CASE\nWHEN ready THEN value\nEND\nEND",
      "BEGIN\n    SELECT CASE\n        WHEN ready THEN value\n    END\nEND"],
    ["python", "if ready:\n      call()\n        nested()\nnext_call()",
      "if ready:\n    call()\n        nested()\nnext_call()"],
    ["yaml", "root:\n\tchild:\n\t\tvalue: yes", "root:\n    child:\n        value: yes"],
    ["markdown", "root\n  child\n    nested", "root\n    child\n        nested"],
    ["toml", "[table]\n      value = 1", "[table]\n    value = 1"],
    ["ini", "[section]\n      value=yes", "[section]\n    value=yes"],
    ["assembly", "start:\nmov ax, bx\nnext:\nret", "start:\n    mov ax, bx\nnext:\n    ret"],
  ];
  for (const [language, input, expected] of fixtures) {
    assert.equal(reformatEditorSnapshot(snapshot(input), language, 4, "spaces").value,
      expected, language);
  }
  assert.equal(reformatEditorSnapshot(snapshot("if (x) {\ny();\n}"), "cpp", 4, "tab").value,
    "if (x) {\n\ty();\n}");
});

test("reformat ignores lexical structure and preserves protected multiline regions", () => {
  assert.equal(reformatEditorSnapshot(snapshot(
    "if (ready) {\nconst char *text = \"}\"; // {\ncall();\n}"), "cpp").value,
  "if (ready) {\n    const char *text = \"}\"; // {\n    call();\n}");

  const markdown = "```cpp\n   if (x) {\n bad();\n   }\n```\nafter";
  assert.equal(reformatEditorSnapshot(snapshot(markdown), "markdown").value, markdown);
  const heredoc = "if ready; then\ncat <<EOF\n }\nEOF\necho yes\nfi";
  assert.equal(reformatEditorSnapshot(snapshot(heredoc), "bash").value,
    "if ready; then\n    cat <<EOF\n }\nEOF\n    echo yes\nfi");
  const blockComment = "if (ready) {\n/* comment\n }\n*/\ncall();\n}";
  assert.equal(reformatEditorSnapshot(snapshot(blockComment), "cpp").value,
    "if (ready) {\n    /* comment\n }\n*/\n    call();\n}");
  const yaml = "root: |\n   literal {\n one\nnext: yes";
  assert.equal(reformatEditorSnapshot(snapshot(yaml), "yaml").value,
    "root: |\n   literal {\n one\nnext: yes");
  const cdata = "<root>\n<![CDATA[\n odd <tag>\n]]>\n</root>";
  assert.equal(reformatEditorSnapshot(snapshot(cdata), "xml").value,
    "<root>\n    <![CDATA[\n odd <tag>\n]]>\n</root>");

  const longLine = "x".repeat(64 * 1024 + 1) + "\n  preserve_after_unsafe_line();";
  const limited = reformatEditorSnapshot(snapshot(longLine), "cpp");
  assert.equal(limited.value, longLine);
  assert.match(limited.warning, /pathological long lines/);

  const whitespace = "if (ready) {\r\n   \r\ncall();\r\n}\r\n";
  assert.equal(reformatEditorSnapshot(snapshot(whitespace), "cpp").value,
    "if (ready) {\r\n   \r\n    call();\r\n}\r\n");
  const oversized = "é".repeat(512 * 1024 + 1);
  const bounded = reformatEditorSnapshot(snapshot(oversized), "cpp");
  assert.equal(bounded.value, oversized);
  assert.equal(bounded.workLimited, true);
});

test("adaptive reformat maps full-file cursors and forward or reverse selected lines", () => {
  const source = "if (ready) {\ncall();\n}\nafter();";
  const selectionEnd = source.indexOf("after");
  const forward = reformatEditorSnapshot(snapshot(source, 0, selectionEnd, "forward"), "cpp");
  assert.equal(forward.value, "if (ready) {\n    call();\n}\nafter();");
  assert.equal(forward.selectionStart, 0);
  assert.equal(forward.selectionEnd, forward.value.indexOf("after"));
  assert.equal(forward.selectionDirection, "forward");
  const reverse = reformatEditorSnapshot(snapshot(source, 0, selectionEnd, "backward"), "cpp");
  assert.equal(reverse.selectionDirection, "backward");

  const cursor = source.indexOf("call") + 2;
  const whole = reformatEditorSnapshot(snapshot(source, cursor), "cpp");
  assert.equal(whole.selectionStart, whole.value.indexOf("\n") + 1 + 2);
  assert.equal(whole.selectionEnd, whole.selectionStart);
  assert.equal(whole.selectionDirection, "none");
  assert.equal(reformatEditorSnapshot(snapshot("plain text"), "text").unsupported, true);
});

test("structural analysis exposes bounded opaque state and embedded context", () => {
  let result = analyzeStructuralLine("```js", "markdown");
  assert.equal(result.workLimited, false);
  result = analyzeStructuralLine("const value = '{';", "markdown", result.nextState);
  assert.equal(result.protectedRegion, true);
  assert.equal(result.embeddedLanguage, "javascript");
  assert.ok(result.semanticSpans.some((span) => span.role === "string"));
  result = analyzeStructuralLine("```", "markdown", result.nextState);
  assert.equal(result.protectedRegion, true);
  assert.deepEqual(result.nextState, {});
  result = analyzeStructuralLine("é".repeat(32769), "cpp");
  assert.equal(result.workLimited, true);
});
