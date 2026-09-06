import { analyzeStructuralLine, canonicalLanguage } from "./syntax-v4.js";

const MAX_TAB_WIDTH = 32;
const DETECTION_LINES = 20;
const MAX_REFORMAT_BYTES = 1024 * 1024;

function width(value) {
  const parsed = Number(value);
  return Number.isInteger(parsed) ? Math.max(1, Math.min(MAX_TAB_WIDTH, parsed)) : 4;
}

function style(value) {
  return value === "tab" || value === "tabs" ? "tab" : "spaces";
}

function boundedUtf8Length(value, limit) {
  let bytes = 0;
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code < 0x80) bytes += 1;
    else if (code < 0x800) bytes += 2;
    else if (code >= 0xd800 && code <= 0xdbff && index + 1 < value.length &&
        value.charCodeAt(index + 1) >= 0xdc00 && value.charCodeAt(index + 1) <= 0xdfff) {
      bytes += 4;
      index += 1;
    } else bytes += 3;
    if (bytes > limit) return bytes;
  }
  return bytes;
}

function snapshot(value) {
  const text = String(value && value.value !== undefined ? value.value : "");
  const start = Math.max(0, Math.min(text.length, Number(value && value.selectionStart) || 0));
  const end = Math.max(start, Math.min(text.length, Number(value && value.selectionEnd) || start));
  const direction = value && ["forward", "backward"].includes(value.selectionDirection)
    ? value.selectionDirection : "none";
  return { value: text, selectionStart: start, selectionEnd: end, selectionDirection: direction };
}

function splitLines(text) {
  const lines = [];
  let start = 0;
  while (start <= text.length) {
    const newline = text.indexOf("\n", start);
    const physicalEnd = newline < 0 ? text.length : newline;
    const cr = physicalEnd > start && text[physicalEnd - 1] === "\r";
    lines.push({ start, text: text.slice(start, cr ? physicalEnd - 1 : physicalEnd),
      ending: newline < 0 ? "" : cr ? "\r\n" : "\n" });
    if (newline < 0) break;
    start = newline + 1;
  }
  return lines;
}

function lineForOffset(lines, offset) {
  let low = 0;
  let high = lines.length;
  while (low + 1 < high) {
    const middle = Math.floor((low + high) / 2);
    if (lines[middle].start <= offset) low = middle;
    else high = middle;
  }
  return low;
}

function leadingBytes(line) {
  let count = 0;
  while (count < line.length && (line[count] === " " || line[count] === "\t")) count += 1;
  return count;
}

function leadingColumn(line, tabWidth) {
  let column = 0;
  for (let index = 0; index < leadingBytes(line); index += 1) {
    column = line[index] === "\t" ? column + tabWidth - (column % tabWidth) : column + 1;
  }
  return column;
}

export function detectIndentation(source, fallbackWidth = 4, fallbackStyle = "spaces") {
  const text = String(source ?? "");
  const differences = new Array(MAX_TAB_WIDTH + 1).fill(0);
  let differenceCount = 0;
  let spaceIndentedLines = 0;
  let tabIndentedLines = 0;
  let mixedIndentedLines = 0;
  let previousSpaceIndent = 0;
  let havePreviousSpaceLine = false;
  const lines = splitLines(text);
  for (let lineNumber = 0; lineNumber < Math.min(DETECTION_LINES, lines.length); lineNumber += 1) {
    const line = lines[lineNumber].text;
    let spaces = 0;
    let tabs = 0;
    let position = 0;
    while (position < line.length && (line[position] === " " || line[position] === "\t")) {
      if (line[position] === " ") spaces += 1;
      else tabs += 1;
      position += 1;
    }
    if (position >= line.length) continue;
    if (spaces > 0 && tabs === 0) spaceIndentedLines += 1;
    else if (tabs > 0 && spaces === 0) tabIndentedLines += 1;
    else if (spaces > 0 && tabs > 0) mixedIndentedLines += 1;
    if (tabs === 0) {
      if (havePreviousSpaceLine) {
        const difference = Math.abs(spaces - previousSpaceIndent);
        if (difference > 0 && difference <= MAX_TAB_WIDTH) {
          differences[difference] += 1;
          differenceCount += 1;
        }
      }
      previousSpaceIndent = spaces;
      havePreviousSpaceLine = true;
    } else havePreviousSpaceLine = false;
  }

  const result = { tabWidth: width(fallbackWidth), tabStyle: style(fallbackStyle),
    tabWidthDetected: false, tabStyleDetected: false };
  if (spaceIndentedLines > 0 && tabIndentedLines === 0 && mixedIndentedLines === 0) {
    result.tabStyle = "spaces";
    result.tabStyleDetected = true;
  } else if (tabIndentedLines > 0 && spaceIndentedLines === 0 && mixedIndentedLines === 0) {
    result.tabStyle = "tab";
    result.tabStyleDetected = true;
  }
  if (!result.tabStyleDetected || result.tabStyle !== "spaces" || differenceCount === 0) return result;
  if (differences[1] === differenceCount) {
    result.tabWidth = 1;
    result.tabWidthDetected = true;
    return result;
  }
  let bestWidth = 0;
  let bestScore = 0;
  let bestExact = 0;
  let ambiguous = false;
  for (let candidate = 2; candidate <= MAX_TAB_WIDTH; candidate += 1) {
    let score = 0;
    for (let difference = candidate; difference <= MAX_TAB_WIDTH; difference += candidate) {
      score += differences[difference];
    }
    const exact = differences[candidate];
    if (score > bestScore || (score === bestScore && exact > bestExact)) {
      bestWidth = candidate;
      bestScore = score;
      bestExact = exact;
      ambiguous = false;
    } else if (score > 0 && score === bestScore && exact === bestExact) ambiguous = true;
  }
  if (bestWidth > 0 && !ambiguous && bestScore * 3 >= differenceCount * 2) {
    result.tabWidth = bestWidth;
    result.tabWidthDetected = true;
  }
  return result;
}

function selectedLineRange(current, lines) {
  const first = lineForOffset(lines, current.selectionStart);
  let last = lineForOffset(lines, current.selectionEnd);
  if (current.selectionEnd > current.selectionStart && last > first &&
      current.selectionEnd === lines[last].start) last -= 1;
  return { first, last };
}

function mapOffset(offset, changes) {
  let mapped = offset;
  for (const change of changes) {
    if (offset < change.position) break;
    if (change.removed === 0 && offset === change.position) continue;
    const removedEnd = change.position + change.removed;
    if (offset < removedEnd) {
      const relative = offset - change.position;
      return mapped - relative + Math.min(relative, change.inserted);
    }
    mapped += change.inserted - change.removed;
  }
  return mapped;
}

function transformSelectedLines(current, tabWidth, tabStyle, outdent) {
  const lines = splitLines(current.value);
  const range = selectedLineRange(current, lines);
  const changes = [];
  const replacements = [];
  const prefix = tabStyle === "tab" ? "\t" : " ".repeat(tabWidth);
  for (let index = range.first; index <= range.last; index += 1) {
    const line = lines[index];
    if (!outdent) {
      changes.push({ position: line.start, removed: 0, inserted: prefix.length });
      replacements.push({ start: line.start, count: 0, text: prefix });
      continue;
    }
    const leading = leadingBytes(line.text);
    if (!leading) continue;
    const column = leadingColumn(line.text, tabWidth);
    const target = column === 0 ? 0 : column - (column % tabWidth === 0 ? tabWidth : column % tabWidth);
    let keep = 0;
    let keptColumn = 0;
    while (keep < leading) {
      const next = line.text[keep] === "\t"
        ? keptColumn + tabWidth - (keptColumn % tabWidth) : keptColumn + 1;
      if (next > target) break;
      keptColumn = next;
      keep += 1;
    }
    const removed = leading - keep;
    if (removed) {
      const position = line.start + keep;
      changes.push({ position, removed, inserted: 0 });
      replacements.push({ start: position, count: removed, text: "" });
    }
  }
  let value = current.value;
  for (let index = replacements.length - 1; index >= 0; index -= 1) {
    const item = replacements[index];
    value = value.slice(0, item.start) + item.text + value.slice(item.start + item.count);
  }
  return { value, selectionStart: mapOffset(current.selectionStart, changes),
    selectionEnd: mapOffset(current.selectionEnd, changes),
    selectionDirection: current.selectionDirection };
}

function characterWidth(codePoint) {
  if (codePoint === 0 || (codePoint >= 0x300 && codePoint <= 0x36f) ||
      (codePoint >= 0x1ab0 && codePoint <= 0x1aff) ||
      (codePoint >= 0x1dc0 && codePoint <= 0x1dff) ||
      (codePoint >= 0xfe00 && codePoint <= 0xfe0f) ||
      (codePoint >= 0xfe20 && codePoint <= 0xfe2f)) return 0;
  return codePoint >= 0x1100 && (codePoint <= 0x115f || codePoint === 0x2329 || codePoint === 0x232a ||
    (codePoint >= 0x2e80 && codePoint <= 0xa4cf) || (codePoint >= 0xac00 && codePoint <= 0xd7a3) ||
    (codePoint >= 0xf900 && codePoint <= 0xfaff) || (codePoint >= 0xfe10 && codePoint <= 0xfe19) ||
    (codePoint >= 0xfe30 && codePoint <= 0xfe6f) || (codePoint >= 0xff00 && codePoint <= 0xff60) ||
    (codePoint >= 0xffe0 && codePoint <= 0xffe6) || (codePoint >= 0x1f300 && codePoint <= 0x1faff) ||
    (codePoint >= 0x20000 && codePoint <= 0x3fffd)) ? 2 : 1;
}

function displayColumn(text, start, end, tabWidth) {
  let column = 0;
  for (let index = start; index < end;) {
    const point = text.codePointAt(index);
    if (point === 9) column += tabWidth - (column % tabWidth);
    else column += characterWidth(point);
    index += point > 0xffff ? 2 : 1;
  }
  return column;
}

export function indentEditorSnapshot(value, tabWidth = 4, tabStyle = "spaces") {
  const current = snapshot(value);
  const normalizedWidth = width(tabWidth);
  const normalizedStyle = style(tabStyle);
  if (current.selectionStart !== current.selectionEnd) {
    return transformSelectedLines(current, normalizedWidth, normalizedStyle, false);
  }
  const lineStart = current.value.lastIndexOf("\n", Math.max(0, current.selectionStart - 1)) + 1;
  const column = displayColumn(current.value, lineStart, current.selectionStart, normalizedWidth);
  const insert = normalizedStyle === "tab" ? "\t" : " ".repeat(normalizedWidth - (column % normalizedWidth));
  const position = current.selectionStart;
  return { value: current.value.slice(0, position) + insert + current.value.slice(position),
    selectionStart: position + insert.length, selectionEnd: position + insert.length,
    selectionDirection: "none" };
}

export function outdentEditorSnapshot(value, tabWidth = 4) {
  const current = snapshot(value);
  return transformSelectedLines(current, width(tabWidth), "spaces", true);
}

const BRACE_LANGUAGES = new Set([
  "c", "cpp", "csharp", "java", "javascript", "typescript", "css", "json", "php",
  "perl", "rust", "go", "powershell",
]);
const TOPOLOGY_LANGUAGES = new Set(["python", "yaml", "markdown", "toml", "ini"]);
const RUBY_CLOSERS = new Set(["end", "else", "elsif", "when", "rescue", "ensure"]);
const RUBY_MIDDLES = new Set(["else", "elsif", "when", "rescue", "ensure"]);
const BASH_CLOSERS = new Set(["fi", "done", "esac", "else", "elif"]);
const BASH_MIDDLES = new Set(["else", "elif"]);
const SQL_CLOSERS = new Set(["end"]);
const NO_MIDDLES = new Set();
const RUBY_OPENERS = new Set(["begin", "case", "class", "def", "do", "for", "if",
  "module", "unless", "until", "while"]);
const BASH_OPENERS = new Set(["case", "for", "if", "select", "until", "while"]);
const SQL_OPENERS = new Set(["begin", "case", "loop"]);

function profileFor(language) {
  if (BRACE_LANGUAGES.has(language)) return "brace";
  if (language === "ruby" || language === "bash" || language === "sql") return language;
  if (["html", "htmlonly", "xml"].includes(language)) return "markup";
  if (TOPOLOGY_LANGUAGES.has(language)) return "topology";
  if (language === "assembly") return "assembly";
  return "unsupported";
}

function structuralCode(line, spans) {
  // Lexer offsets and textarea offsets are UTF-16. Mask by code unit so strings
  // containing astral characters retain every structural token's exact offset.
  const code = line.split("");
  for (const span of spans) {
    if (span.role !== "comment" && span.role !== "string") continue;
    for (let index = Math.max(0, span.start); index < Math.min(code.length, span.end); index += 1) {
      code[index] = " ";
    }
  }
  return code.join("");
}

function words(code) {
  return [...code.matchAll(/[A-Za-z_][A-Za-z0-9_]*/g)].map((match) => match[0].toLowerCase());
}

function leadingClosers(code) {
  let position = leadingBytes(code);
  let count = 0;
  while (position < code.length && "}])".includes(code[position])) {
    count += 1;
    position += 1;
    while (position < code.length && (code[position] === " " || code[position] === "\t")) position += 1;
  }
  return count;
}

function braceCounts(code) {
  let opens = 0;
  let closes = 0;
  for (const character of code) {
    if ("{[(".includes(character)) opens += 1;
    else if ("}])".includes(character)) closes += 1;
  }
  return { opens, closes };
}

const VOID_TAGS = new Set(["area", "base", "br", "col", "embed", "hr", "img", "input",
  "link", "meta", "param", "source", "track", "wbr"]);

function markupCounts(code) {
  let leadingCloses = 0;
  let opens = 0;
  let closes = 0;
  let sawContent = false;
  const expression = /<\s*(\/?)\s*([A-Za-z0-9:_-]+)([^>]*)>/g;
  for (const match of code.matchAll(expression)) {
    if (match[0].startsWith("<!--") || match[0].startsWith("<!") || match[0].startsWith("<?")) continue;
    const closing = Boolean(match[1]);
    const selfClosing = /\/\s*>$/.test(match[0]);
    if (closing) {
      closes += 1;
      if (!sawContent) leadingCloses += 1;
    } else if (!selfClosing && !VOID_TAGS.has(match[2].toLowerCase())) opens += 1;
    sawContent = true;
  }
  return { leadingCloses, opens, closes };
}

function topologyDepth(structural, column) {
  if (!structural.topologyInitialized) {
    structural.topologyColumns[0] = column;
    structural.topologyInitialized = true;
    return 0;
  }
  if (column < structural.topologyColumns[0]) {
    structural.topologyColumns = [column];
    return 0;
  }
  while (structural.topologyColumns.length > 1 &&
      column < structural.topologyColumns.at(-1)) structural.topologyColumns.pop();
  if (column > structural.topologyColumns.at(-1)) structural.topologyColumns.push(column);
  return structural.topologyColumns.length - 1;
}

function desiredDepth(profile, original, code, structural, tabWidth) {
  if (profile === "topology") return { depth: topologyDepth(structural, leadingColumn(original, tabWidth)), middle: false };
  if (profile === "assembly") {
    const start = leadingBytes(code);
    const colon = code.indexOf(":", start);
    const whitespace = code.slice(start).search(/[ \t]/);
    return { depth: colon >= 0 && (whitespace < 0 || colon < start + whitespace) ? 0 : 1, middle: false };
  }
  if (profile === "brace") return { depth: Math.max(0, structural.depth - leadingClosers(code)), middle: false };
  if (profile === "markup") {
    const counts = markupCounts(code);
    return { depth: Math.max(0, structural.depth - counts.leadingCloses), middle: false };
  }
  const tokens = words(code);
  if (!tokens.length) return { depth: structural.depth, middle: false };
  const closers = profile === "ruby" ? RUBY_CLOSERS :
    profile === "bash" ? BASH_CLOSERS : SQL_CLOSERS;
  const middles = profile === "ruby" ? RUBY_MIDDLES :
    profile === "bash" ? BASH_MIDDLES : NO_MIDDLES;
  return { depth: closers.has(tokens[0]) ? Math.max(0, structural.depth - 1) : structural.depth,
    middle: middles.has(tokens[0]) };
}

function advanceState(profile, code, structural, middle) {
  if (profile === "topology" || profile === "assembly") return;
  if (profile === "brace") {
    const { opens, closes } = braceCounts(code);
    structural.depth = Math.max(0, structural.depth + opens - closes);
    return;
  }
  if (profile === "markup") {
    const { opens, closes } = markupCounts(code);
    structural.depth = Math.max(0, structural.depth + opens - closes);
    return;
  }
  const tokens = words(code);
  if (!tokens.length) return;
  const openers = profile === "ruby" ? RUBY_OPENERS :
    profile === "bash" ? BASH_OPENERS : SQL_OPENERS;
  const closes = ["end", "fi", "done", "esac"].includes(tokens[0]) || middle;
  if (closes) structural.depth = Math.max(0, structural.depth - 1);
  const rubyDo = profile === "ruby" && tokens.includes("do");
  const sqlCase = profile === "sql" && tokens.includes("case");
  if (openers.has(tokens[0]) || rubyDo || sqlCase || middle) structural.depth += 1;
}

function offsetForColumn(line, target, tabWidth) {
  let column = 0;
  let offset = 0;
  while (offset < line.length) {
    const point = line.codePointAt(offset);
    const next = point === 9 ? column + tabWidth - (column % tabWidth) : column + characterWidth(point);
    if (next > target) break;
    column = next;
    offset += point > 0xffff ? 2 : 1;
  }
  return offset;
}

export function reformatEditorSnapshot(value, languageLabel, tabWidth = 4, tabStyle = "spaces") {
  const current = snapshot(value);
  const language = canonicalLanguage(languageLabel);
  const profile = profileFor(language);
  if (profile === "unsupported") return { ...current, unsupported: true };
  if (boundedUtf8Length(current.value, MAX_REFORMAT_BYTES) > MAX_REFORMAT_BYTES) {
    return { ...current, workLimited: true, warning: "This draft is too large to reformat safely (1 MiB limit)." };
  }
  const normalizedWidth = width(tabWidth);
  const normalizedStyle = style(tabStyle);
  const lines = splitLines(current.value);
  const selected = current.selectionStart !== current.selectionEnd;
  const range = selected ? selectedLineRange(current, lines) : { first: 0, last: lines.length - 1 };
  const formatted = lines.map((line) => line.text);
  const structural = { depth: 0, embeddedDepth: 0, topologyColumns: [0], topologyInitialized: false };
  let lexical = {};
  let classificationUnsafe = false;
  let warnedLimited = false;
  for (let index = 0; index <= range.last; index += 1) {
    const original = lines[index].text;
    const analysis = analyzeStructuralLine(original, language, lexical);
    lexical = analysis.nextState;
    if (analysis.workLimited) {
      classificationUnsafe = true;
      warnedLimited = true;
    }
    const code = structuralCode(original, analysis.semanticSpans);
    if (leadingBytes(original) === original.length) continue;
    const embeddedMarkup = profile === "markup" &&
      (analysis.embeddedLanguage === "javascript" || analysis.embeddedLanguage === "css") && !code.includes("<");
    let middle = false;
    let depth;
    if (embeddedMarkup) {
      depth = structural.depth + Math.max(0, structural.embeddedDepth - leadingClosers(code));
    } else {
      const desired = desiredDepth(profile, original, code, structural, normalizedWidth);
      depth = desired.depth;
      middle = desired.middle;
    }
    if (index >= range.first && !analysis.protectedRegion && !classificationUnsafe) {
      const indent = language === "yaml" || normalizedStyle === "spaces"
        ? " ".repeat(depth * normalizedWidth) : "\t".repeat(depth);
      formatted[index] = indent + original.slice(leadingBytes(original));
    }
    if (embeddedMarkup) {
      const { opens, closes } = braceCounts(code);
      structural.embeddedDepth = Math.max(0, structural.embeddedDepth + opens - closes);
    } else {
      if (profile === "markup") structural.embeddedDepth = 0;
      advanceState(profile, code, structural, middle);
    }
  }
  const valueText = lines.map((line, index) => formatted[index] + line.ending).join("");
  if (boundedUtf8Length(valueText, MAX_REFORMAT_BYTES) > MAX_REFORMAT_BYTES) {
    return { ...current, workLimited: true,
      warning: "The reformatted draft would exceed the 1 MiB editor limit, so it was preserved." };
  }
  if (selected) {
    const refreshed = splitLines(valueText);
    const selectionStart = refreshed[range.first].start;
    const selectionEnd = range.last + 1 < refreshed.length ? refreshed[range.last + 1].start : valueText.length;
    return { value: valueText, selectionStart, selectionEnd,
      selectionDirection: current.selectionDirection === "backward" ? "backward" : "forward",
      warning: warnedLimited ? "Some pathological long lines were preserved because they could not be classified safely" : "" };
  }
  const oldLines = lines;
  const cursorLine = lineForOffset(oldLines, current.selectionEnd);
  const oldLine = oldLines[cursorLine];
  const cursorColumn = displayColumn(current.value, oldLine.start, current.selectionEnd, normalizedWidth);
  const refreshed = splitLines(valueText);
  const mappedLine = refreshed[Math.min(cursorLine, refreshed.length - 1)];
  const cursor = mappedLine.start + offsetForColumn(mappedLine.text, cursorColumn, normalizedWidth);
  return { value: valueText, selectionStart: cursor, selectionEnd: cursor,
    selectionDirection: "none",
    warning: warnedLimited ? "Some pathological long lines were preserved because they could not be classified safely" : "" };
}
