import { appendHighlightedCode, canonicalLanguage } from "./syntax-v3.js";

const MAX_INLINE_DEPTH = 32;
const SAFE_SCHEMES = new Set(["http:", "https:"]);

function createElement(documentRef, tag, className = "") {
  const node = documentRef.createElement(tag);
  if (className) node.className = className;
  return node;
}

function appendText(documentRef, parent, text) {
  if (text) parent.append(documentRef.createTextNode(text));
}

function leadingIndent(line) {
  let width = 0;
  let offset = 0;
  while (offset < line.length) {
    if (line[offset] === " ") width += 1;
    else if (line[offset] === "\t") width += 4;
    else break;
    offset += 1;
  }
  return { width, offset };
}

function stripIndent(line, width) {
  let consumed = 0;
  let offset = 0;
  while (offset < line.length && consumed < width) {
    if (line[offset] === " ") consumed += 1;
    else if (line[offset] === "\t") consumed += 4;
    else break;
    offset += 1;
  }
  return line.slice(offset);
}

function fenceOpen(line) {
  const match = line.match(/^ {0,3}(`{3,}|~{3,})(.*)$/);
  if (!match) return null;
  return {
    marker: match[1][0],
    length: match[1].length,
    language: match[2].trim().split(/\s+/, 1)[0].toLowerCase(),
  };
}

function isFenceClose(line, fence) {
  const match = line.match(/^ {0,3}(`+|~+)\s*$/);
  return Boolean(match && match[1][0] === fence.marker && match[1].length >= fence.length);
}

function trailingFenceClose(line, fence) {
  const trimmed = line.replace(/\s+$/, "");
  let start = trimmed.length;
  while (start > 0 && trimmed[start - 1] === fence.marker) start -= 1;
  if (start === 0 || trimmed.length - start < fence.length) return null;
  return trimmed.slice(0, start).replace(/\s+$/, "");
}

function heading(line) {
  const match = line.match(/^ {0,3}(#{1,6})(?:[ \t]+(.*)|[ \t]*)$/);
  if (!match) return null;
  const text = (match[2] || "").replace(/[ \t]+#+[ \t]*$/, "").trimEnd();
  return { level: match[1].length, text };
}

function setextLevel(line) {
  const match = line.match(/^ {0,3}(=+|-+)\s*$/);
  if (!match || match[1].length < 2) return 0;
  return match[1][0] === "=" ? 1 : 2;
}

function isHorizontalRule(line) {
  const compact = line.trim().replace(/[ \t]/g, "");
  return compact.length >= 3 && (/^\*+$/.test(compact) || /^-+$/.test(compact) || /^_+$/.test(compact));
}

function quoteText(line) {
  const match = line.match(/^ {0,3}>[ \t]?(.*)$/);
  return match ? match[1] : null;
}

function listMarker(line) {
  const indent = leadingIndent(line);
  const rest = line.slice(indent.offset);
  const unordered = rest.match(/^[-+*][ \t]+(.*)$/);
  if (unordered) {
    return { ordered: false, indent: indent.width, content: unordered[1], contentIndent: indent.width + rest.length - unordered[1].length };
  }
  const ordered = rest.match(/^(\d{1,9})[.)][ \t]+(.*)$/);
  if (!ordered) return null;
  return {
    ordered: true,
    start: Number(ordered[1]),
    indent: indent.width,
    content: ordered[2],
    contentIndent: indent.width + rest.length - ordered[2].length,
  };
}

function isIndentedCode(line) {
  return leadingIndent(line).width >= 4 && !listMarker(line);
}

function splitTableRow(line) {
  let source = line.trim();
  if (!source.includes("|")) return null;
  if (source.startsWith("|")) source = source.slice(1);
  if (source.endsWith("|") && !source.endsWith("\\|")) source = source.slice(0, -1);
  const cells = [];
  let cell = "";
  let codeRun = 0;
  for (let i = 0; i < source.length;) {
    if (source[i] === "\\" && i + 1 < source.length) {
      cell += source.slice(i, i + 2);
      i += 2;
      continue;
    }
    if (source[i] === "`") {
      let end = i + 1;
      while (end < source.length && source[end] === "`") end += 1;
      const run = end - i;
      if (codeRun === 0) codeRun = run;
      else if (codeRun === run) codeRun = 0;
      cell += source.slice(i, end);
      i = end;
      continue;
    }
    if (source[i] === "|" && codeRun === 0) {
      cells.push(cell.trim());
      cell = "";
    } else {
      cell += source[i];
    }
    i += 1;
  }
  cells.push(cell.trim());
  return cells.length >= 2 ? cells : null;
}

function tableSeparator(line) {
  const cells = splitTableRow(line);
  if (!cells || !cells.every((cell) => /^:?-{3,}:?$/.test(cell.trim()))) return null;
  return cells.map((cell) => {
    const value = cell.trim();
    if (value.startsWith(":") && value.endsWith(":")) return "center";
    if (value.endsWith(":")) return "right";
    return "left";
  });
}

function isTableStart(lines, index) {
  const header = splitTableRow(lines[index] || "");
  const aligns = tableSeparator(lines[index + 1] || "");
  return Boolean(header && aligns && header.length === aligns.length);
}

function decodeEntityAt(input, offset) {
  const match = input.slice(offset).match(/^&(#(?:x[0-9a-f]+|\d+)|amp|lt|gt|quot|apos);/i);
  if (!match) return null;
  const named = { amp: "&", lt: "<", gt: ">", quot: "\"", apos: "'" };
  let value = named[match[1].toLowerCase()];
  if (match[1][0] === "#") {
    const hex = match[1][1].toLowerCase() === "x";
    const number = Number.parseInt(match[1].slice(hex ? 2 : 1), hex ? 16 : 10);
    value = Number.isFinite(number) && number > 0 && number <= 0x10ffff
      ? String.fromCodePoint(number) : "�";
  }
  return { value, length: match[0].length };
}

function spendWork(work) {
  work.remaining -= 1;
  return work.remaining >= 0;
}

function findClosing(input, delimiter, offset, work) {
  for (let cursor = offset; cursor <= input.length - delimiter.length; cursor += 1) {
    if (!spendWork(work)) return -1;
    if (input[cursor] === "\\") {
      cursor += 1;
      continue;
    }
    if (input.startsWith(delimiter, cursor) && cursor > offset && !/\s/.test(input[cursor - 1])) {
      return cursor;
    }
  }
  return -1;
}

function delimiterCanOpen(input, offset, delimiter) {
  const after = input[offset + delimiter.length] || "";
  if (!after || /\s/.test(after)) return false;
  if (delimiter.includes("_")) {
    const before = input[offset - 1] || "";
    if (/[\p{L}\p{N}]/u.test(before) && /[\p{L}\p{N}]/u.test(after)) return false;
  }
  return true;
}

function matchingParen(input, offset, work) {
  let depth = 0;
  for (let cursor = offset; cursor < input.length; cursor += 1) {
    if (!spendWork(work)) return -1;
    if (input[cursor] === "\\") {
      cursor += 1;
      continue;
    }
    if (input[cursor] === "(") depth += 1;
    else if (input[cursor] === ")" && --depth === 0) return cursor;
  }
  return -1;
}

function matchingBracket(input, offset, work) {
  let depth = 0;
  for (let cursor = offset; cursor < input.length; cursor += 1) {
    if (!spendWork(work)) return -1;
    if (input[cursor] === "\\") {
      cursor += 1;
      continue;
    }
    if (input[cursor] === "[") depth += 1;
    else if (input[cursor] === "]" && --depth === 0) return cursor;
  }
  return -1;
}

function parseDestination(source) {
  const trimmed = source.trim();
  let match = trimmed.match(/^<([^<>\s]+)>(?:\s+(?:"([^"]*)"|'([^']*)'|\(([^)]*)\)))?$/);
  if (!match) match = trimmed.match(/^(\S+?)(?:\s+(?:"([^"]*)"|'([^']*)'|\(([^)]*)\)))?$/);
  if (!match) return null;
  return { url: match[1].replace(/\\([()])/g, "$1"), title: match[2] || match[3] || match[4] || "" };
}

function safeUrl(source) {
  try {
    const parsed = new URL(source);
    if (!SAFE_SCHEMES.has(parsed.protocol) || parsed.username || parsed.password) return null;
    return parsed.href;
  } catch (_) {
    return null;
  }
}

function linkAt(input, offset, work) {
  if (input[offset] !== "[") return null;
  const labelEnd = matchingBracket(input, offset, work);
  if (labelEnd < 0 || input[labelEnd + 1] !== "(") return null;
  const targetEnd = matchingParen(input, labelEnd + 1, work);
  if (targetEnd < 0) return null;
  const destination = parseDestination(input.slice(labelEnd + 2, targetEnd));
  return {
    end: targetEnd + 1,
    label: input.slice(offset + 1, labelEnd),
    destination,
    literal: input.slice(offset, targetEnd + 1),
  };
}

function bareUrlAt(input, offset) {
  if (!(input.startsWith("http://", offset) || input.startsWith("https://", offset))) return null;
  if (offset > 0 && /[\p{L}\p{N}_]/u.test(input[offset - 1])) return null;
  let end = offset;
  while (end < input.length && !/[\s<>]/.test(input[end])) end += 1;
  while (end > offset && /[.,;:!?]/.test(input[end - 1])) end -= 1;
  for (const [open, close] of [["(", ")"], ["[", "]"], ["{", "}"]]) {
    while (end > offset && input[end - 1] === close &&
           input.slice(offset, end).split(close).length > input.slice(offset, end).split(open).length) end -= 1;
  }
  if (end === offset) return null;
  const label = input.slice(offset, end);
  const url = safeUrl(label);
  return url ? { end, label, url } : null;
}

function appendAnchor(documentRef, parent, label, href, depth, work, title = "", richLabel = true) {
  const anchor = createElement(documentRef, "a", "md-link");
  anchor.setAttribute("href", href);
  anchor.setAttribute("target", "_blank");
  anchor.setAttribute("rel", "noopener noreferrer");
  anchor.setAttribute("referrerpolicy", "no-referrer");
  if (title) anchor.setAttribute("title", title);
  if (richLabel) appendInline(documentRef, anchor, label, depth + 1, work);
  else appendText(documentRef, anchor, label);
  parent.append(anchor);
}

function appendInline(documentRef, parent, input, depth = 0,
                      work = { remaining: Math.max(1024, input.length * 16) }) {
  if (depth > MAX_INLINE_DEPTH) {
    appendText(documentRef, parent, input);
    return;
  }
  for (let offset = 0; offset < input.length;) {
    if (input[offset] === "\\" && offset + 1 < input.length && /[!"#$%&'()*+,\-./:;<=>?@[\\\]^_`{|}~]/.test(input[offset + 1])) {
      appendText(documentRef, parent, input[offset + 1]);
      offset += 2;
      continue;
    }
    if (input[offset] === "&") {
      const entity = decodeEntityAt(input, offset);
      if (entity) {
        appendText(documentRef, parent, entity.value);
        offset += entity.length;
        continue;
      }
    }
    if (input[offset] === "`") {
      let runEnd = offset + 1;
      while (runEnd < input.length && input[runEnd] === "`") runEnd += 1;
      const delimiter = input.slice(offset, runEnd);
      const close = input.indexOf(delimiter, runEnd);
      if (close >= 0) {
        let codeText = input.slice(runEnd, close).replace(/\n/g, " ");
        if (/^\s.*\s$/.test(codeText) && !/^\s+$/.test(codeText)) codeText = codeText.slice(1, -1);
        const code = createElement(documentRef, "code", "md-inline-code");
        appendText(documentRef, code, codeText);
        parent.append(code);
        offset = close + delimiter.length;
        continue;
      }
    }
    if (input.startsWith("![", offset)) {
      const image = linkAt(input, offset + 1, work);
      if (image) {
        appendText(documentRef, parent, `!${image.literal}`);
        offset = image.end;
      } else {
        appendText(documentRef, parent, "![");
        offset += 2;
      }
      continue;
    }
    if (input[offset] === "[") {
      const link = linkAt(input, offset, work);
      if (link) {
        const href = link.destination ? safeUrl(link.destination.url) : null;
        if (href) appendAnchor(documentRef, parent, link.label, href, depth, work, link.destination.title);
        else appendText(documentRef, parent, link.literal);
        offset = link.end;
        continue;
      }
    }
    if (input[offset] === "<") {
      const close = input.indexOf(">", offset + 1);
      if (close > offset + 1) {
        const label = input.slice(offset + 1, close);
        const href = safeUrl(label);
        if (href) {
          appendAnchor(documentRef, parent, label, href, depth, work, "", false);
          offset = close + 1;
          continue;
        }
        if (/^(?:\/?[a-z][\s\S]*|!--[\s\S]*)$/i.test(label)) {
          appendText(documentRef, parent, input.slice(offset, close + 1));
          offset = close + 1;
          continue;
        }
      }
    }
    const bare = bareUrlAt(input, offset);
    if (bare) {
      appendAnchor(documentRef, parent, bare.label, bare.url, depth, work, "", false);
      offset = bare.end;
      continue;
    }
    const delimiters = ["***", "___", "**", "__", "~~", "*", "_"];
    let formatted = false;
    for (const delimiter of delimiters) {
      if (!input.startsWith(delimiter, offset) || !delimiterCanOpen(input, offset, delimiter)) continue;
      const close = findClosing(input, delimiter, offset + delimiter.length, work);
      if (close < 0) continue;
      const body = input.slice(offset + delimiter.length, close);
      let node;
      if (delimiter.length === 3) {
        node = createElement(documentRef, "strong", "md-emphasis");
        const emphasis = createElement(documentRef, "em", "md-emphasis");
        appendInline(documentRef, emphasis, body, depth + 1, work);
        node.append(emphasis);
      } else {
        const tag = delimiter === "~~" ? "del" : delimiter.length === 2 ? "strong" : "em";
        node = createElement(documentRef, tag, "md-emphasis");
        appendInline(documentRef, node, body, depth + 1, work);
      }
      parent.append(node);
      offset = close + delimiter.length;
      formatted = true;
      break;
    }
    if (formatted) continue;
    appendText(documentRef, parent, input[offset]);
    offset += 1;
  }
}

function appendParagraph(documentRef, parent, lines) {
  const paragraph = createElement(documentRef, "p");
  for (let index = 0; index < lines.length; index += 1) {
    const hardBreak = /(?: {2,}|\\)$/.test(lines[index]);
    const text = hardBreak ? lines[index].replace(/(?: {2,}|\\)$/, "") : lines[index];
    appendInline(documentRef, paragraph, text);
    if (index + 1 < lines.length) {
      if (hardBreak) paragraph.append(createElement(documentRef, "br"));
      else appendText(documentRef, paragraph, " ");
    }
  }
  parent.append(paragraph);
}

function blockStart(lines, index) {
  const line = lines[index] || "";
  if (!line.trim()) return true;
  if (fenceOpen(line) || heading(line) || quoteText(line) !== null || listMarker(line) || isIndentedCode(line) || isHorizontalRule(line)) return true;
  if (isTableStart(lines, index)) return true;
  return index + 1 < lines.length && Boolean(line.trim()) && setextLevel(lines[index + 1]) > 0;
}

function appendList(documentRef, parent, lines, start, depth) {
  const first = listMarker(lines[start]);
  const list = createElement(documentRef, first.ordered ? "ol" : "ul");
  if (first.ordered && first.start !== 1) list.setAttribute("start", String(first.start));
  const baseIndent = first.indent;
  let index = start;
  while (index < lines.length) {
    const marker = listMarker(lines[index]);
    if (!marker || marker.indent !== baseIndent || marker.ordered !== first.ordered) break;
    const itemLines = [marker.content];
    index += 1;
    while (index < lines.length) {
      const next = listMarker(lines[index]);
      if (next && next.indent <= baseIndent) break;
      if (lines[index].trim() && leadingIndent(lines[index]).width <= baseIndent) break;
      itemLines.push(lines[index].trim() ? stripIndent(lines[index], marker.contentIndent) : "");
      index += 1;
    }
    while (itemLines.length && !itemLines[itemLines.length - 1].trim()) itemLines.pop();
    const item = createElement(documentRef, "li");
    appendBlocks(documentRef, item, itemLines, depth + 1);
    list.append(item);
  }
  parent.append(list);
  return index;
}

function appendTable(documentRef, parent, lines, start) {
  const headers = splitTableRow(lines[start]);
  const aligns = tableSeparator(lines[start + 1]);
  const wrapper = createElement(documentRef, "div", "markdown-table-scroll");
  wrapper.setAttribute("role", "region");
  wrapper.setAttribute("aria-label", "Markdown table");
  wrapper.setAttribute("tabindex", "0");
  const table = createElement(documentRef, "table");
  const head = createElement(documentRef, "thead");
  const headerRow = createElement(documentRef, "tr");
  headers.forEach((header, column) => {
    const cell = createElement(documentRef, "th", `md-align-${aligns[column]}`);
    cell.setAttribute("scope", "col");
    appendInline(documentRef, cell, header);
    headerRow.append(cell);
  });
  head.append(headerRow);
  table.append(head);
  const body = createElement(documentRef, "tbody");
  let index = start + 2;
  while (index < lines.length && lines[index].trim()) {
    const sourceCells = splitTableRow(lines[index]);
    if (!sourceCells || tableSeparator(lines[index])) break;
    const row = createElement(documentRef, "tr");
    for (let column = 0; column < headers.length; column += 1) {
      const cell = createElement(documentRef, "td", `md-align-${aligns[column]}`);
      appendInline(documentRef, cell, sourceCells[column] || "");
      row.append(cell);
    }
    body.append(row);
    index += 1;
  }
  table.append(body);
  wrapper.append(table);
  parent.append(wrapper);
  return index;
}

function appendBlocks(documentRef, parent, lines, depth = 0) {
  if (depth > MAX_INLINE_DEPTH) {
    appendText(documentRef, parent, lines.join("\n"));
    return;
  }
  for (let index = 0; index < lines.length;) {
    if (!lines[index].trim()) {
      index += 1;
      continue;
    }
    const fence = fenceOpen(lines[index]);
    if (fence) {
      const codeLines = [];
      index += 1;
      while (index < lines.length && !isFenceClose(lines[index], fence)) {
        const trailing = trailingFenceClose(lines[index], fence);
        if (trailing !== null) {
          codeLines.push(trailing);
          index += 1;
          break;
        }
        codeLines.push(lines[index]);
        index += 1;
      }
      if (index < lines.length && isFenceClose(lines[index], fence)) index += 1;
      const pre = createElement(documentRef, "pre", "md-code-block");
      const code = createElement(documentRef, "code");
      const safeLanguage = /^[a-z0-9_+#.-]{1,32}$/.test(fence.language) ? fence.language : "";
      if (safeLanguage) code.setAttribute("data-language", safeLanguage);
      const source = codeLines.join("\n");
      appendHighlightedCode(documentRef, code, source, canonicalLanguage(safeLanguage));
      pre.append(code);
      parent.append(pre);
      continue;
    }
    const atx = heading(lines[index]);
    if (atx) {
      const node = createElement(documentRef, `h${atx.level}`, "md-heading");
      appendInline(documentRef, node, atx.text);
      parent.append(node);
      index += 1;
      continue;
    }
    const setext = index + 1 < lines.length ? setextLevel(lines[index + 1]) : 0;
    if (setext && lines[index].trim()) {
      const node = createElement(documentRef, `h${setext}`, "md-heading");
      appendInline(documentRef, node, lines[index].trim());
      parent.append(node);
      index += 2;
      continue;
    }
    if (isTableStart(lines, index)) {
      index = appendTable(documentRef, parent, lines, index);
      continue;
    }
    const quoted = quoteText(lines[index]);
    if (quoted !== null) {
      const quoteLines = [];
      while (index < lines.length) {
        const text = quoteText(lines[index]);
        if (text === null) break;
        quoteLines.push(text);
        index += 1;
      }
      const blockquote = createElement(documentRef, "blockquote");
      appendBlocks(documentRef, blockquote, quoteLines, depth + 1);
      parent.append(blockquote);
      continue;
    }
    const marker = listMarker(lines[index]);
    if (marker) {
      index = appendList(documentRef, parent, lines, index, depth);
      continue;
    }
    if (isHorizontalRule(lines[index])) {
      parent.append(createElement(documentRef, "hr"));
      index += 1;
      continue;
    }
    if (isIndentedCode(lines[index])) {
      const codeLines = [];
      while (index < lines.length && (isIndentedCode(lines[index]) || !lines[index].trim())) {
        codeLines.push(lines[index].trim() ? stripIndent(lines[index], 4) : "");
        index += 1;
      }
      const pre = createElement(documentRef, "pre", "md-code-block");
      const code = createElement(documentRef, "code");
      appendText(documentRef, code, codeLines.join("\n").replace(/\n+$/, ""));
      pre.append(code);
      parent.append(pre);
      continue;
    }
    const paragraphLines = [];
    while (index < lines.length && lines[index].trim() && !blockStart(lines, index)) {
      paragraphLines.push(lines[index]);
      index += 1;
    }
    if (!paragraphLines.length) {
      paragraphLines.push(lines[index]);
      index += 1;
    }
    appendParagraph(documentRef, parent, paragraphLines);
  }
}

export function renderMarkdown(markdown, documentRef = document) {
  const fragment = documentRef.createDocumentFragment();
  const root = createElement(documentRef, "div", "markdown-body");
  const lines = String(markdown ?? "").replace(/\r\n?/g, "\n").split("\n");
  appendBlocks(documentRef, root, lines);
  fragment.append(root);
  return fragment;
}
