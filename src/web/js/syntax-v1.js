/* Dependency-free syntax highlighting for Markdown code fences. */
const MAX_HIGHLIGHT_BYTES = 1024 * 1024;
const MAX_LINE_BYTES = 64 * 1024;
const MAX_TOKENS = 50000;

const LANGUAGE_ALIASES = new Map(Object.entries({
  javascript: "javascript", js: "javascript", jsx: "javascript", node: "javascript",
  typescript: "typescript", ts: "typescript", tsx: "typescript",
  python: "python", py: "python", python3: "python",
  c: "c", h: "c",
  cpp: "cpp", "c++": "cpp", cxx: "cpp", cc: "cpp", hpp: "cpp", hxx: "cpp",
  html: "html", html5: "html", htm: "html",
  css: "css",
  bash: "bash", sh: "bash", shell: "bash", zsh: "bash",
}));

const words = (source) => new Set(source.split(" "));
const KEYWORDS = {
  javascript: words("async await break case catch class const continue debugger default delete do else export extends finally for from function get if import in instanceof let new of return set static super switch this throw try typeof var void while with yield"),
  typescript: words("abstract as asserts async await break case catch class const constructor continue debugger declare default delete do else enum export extends finally for from function get if implements import in infer instanceof interface is keyof let module namespace new of override private protected public readonly return satisfies set static super switch this throw try type typeof var void while with yield"),
  python: words("and as assert async await break case class continue def del elif else except finally for from global if import in is lambda match nonlocal not or pass raise return try while with yield"),
  c: words("auto break case const continue default do else enum extern for goto if inline register restrict return sizeof static struct switch typedef union volatile while _Alignas _Alignof _Atomic _Generic _Noreturn _Static_assert _Thread_local"),
  cpp: words("alignas alignof and and_eq asm auto bitand bitor break case catch class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do dynamic_cast else enum explicit export extern for friend goto if inline mutable namespace new noexcept not not_eq operator or or_eq private protected public register reinterpret_cast requires return sizeof static static_assert static_cast struct switch template this thread_local throw try typedef typeid typename union using virtual volatile while xor xor_eq"),
  bash: words("case coproc do done elif else esac fi for function if in select then time until while declare export local readonly return set source typeset unset"),
  css: new Set(),
};

const TYPES = {
  javascript: new Set(),
  typescript: words("any bigint boolean never number object string symbol unknown void"),
  python: words("bool bytes complex dict float frozenset int list memoryview object range set str tuple type"),
  c: words("bool char double float int long short signed unsigned void size_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t"),
  cpp: words("bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t size_t nullptr_t"),
  bash: new Set(),
  css: new Set(),
};

const LITERALS = {
  javascript: words("true false null undefined NaN Infinity"),
  typescript: words("true false null undefined NaN Infinity"),
  python: words("True False None NotImplemented"),
  c: words("true false null NULL"),
  cpp: words("true false null nullptr NULL"),
  bash: new Set(),
  css: new Set(),
};

const OPERATOR_CHARS = new Set("+-*/%=!<>?:&|^~.,;()[]{}");

export function canonicalLanguage(label) {
  return LANGUAGE_ALIASES.get(String(label || "").trim().toLowerCase()) || "";
}

function isIdentifierStart(character) {
  return Boolean(character && /[A-Za-z_$]/.test(character));
}

function isIdentifierPart(character) {
  return Boolean(character && /[A-Za-z0-9_$]/.test(character));
}

function isEscaped(line, position) {
  let slashes = 0;
  for (let index = position - 1; index >= 0 && line[index] === "\\"; index -= 1) slashes += 1;
  return slashes % 2 === 1;
}

function quotedEnd(line, start, quote) {
  for (let index = start + 1; index < line.length; index += 1) {
    if (line[index] === quote && !isEscaped(line, index)) return index + 1;
  }
  return line.length;
}

function token(tokens, start, end, role) {
  if (end > start && tokens.length < MAX_TOKENS) tokens.push({ start, end, role });
}

function numberEnd(line, start) {
  let end = start + 1;
  while (end < line.length && /[A-Za-z0-9_.]/.test(line[end])) end += 1;
  if (end < line.length && /[+-]/.test(line[end]) && /[eEpP]/.test(line[end - 1])) end += 1;
  while (end < line.length && /[A-Za-z0-9_.]/.test(line[end])) end += 1;
  return end;
}

function regexEnd(line, start) {
  let bracket = false;
  for (let end = start + 1; end < line.length; end += 1) {
    if (line[end] === "[" && !isEscaped(line, end)) bracket = true;
    else if (line[end] === "]" && !isEscaped(line, end)) bracket = false;
    else if (line[end] === "/" && !bracket && !isEscaped(line, end)) {
      end += 1;
      while (end < line.length && /[A-Za-z]/.test(line[end])) end += 1;
      return end;
    }
  }
  return start + 1;
}

function javascriptRegexContext(line, slash) {
  const before = line.slice(0, slash).trimEnd();
  if (!before || /[=([{,:;!&|?]$/.test(before)) return true;
  const match = before.match(/([A-Za-z_$][\w$]*)$/);
  return Boolean(match && /^(return|case|throw|yield|await|else|do)$/.test(match[1]));
}

function classifyWord(language, line, start, end, declaration) {
  const value = line.slice(start, end);
  if (LITERALS[language].has(value)) return "literal";
  if (KEYWORDS[language].has(value)) return "keyword";
  if (TYPES[language].has(value)) return "type";
  if (declaration === "function") return "function";
  if (declaration === "type") return "type";
  let after = end;
  while (after < line.length && /\s/.test(line[after])) after += 1;
  return line[after] === "(" ? "function" : "";
}

function scanCLike(line, language, previous = {}) {
  const tokens = [];
  const state = { ...previous };
  let position = 0;
  let declaration = "";
  const first = line.search(/\S/);
  if ((language === "c" || language === "cpp") && first >= 0 && line[first] === "#" && !state.mode) {
    token(tokens, first, line.length, "preprocessor");
    return { tokens, state };
  }
  if (state.mode === "block-comment") {
    const close = line.indexOf("*/");
    if (close < 0) {
      token(tokens, 0, line.length, "comment");
      return { tokens, state };
    }
    token(tokens, 0, close + 2, "comment");
    state.mode = "";
    position = close + 2;
  } else if (state.mode === "template") {
    let close = line.indexOf("`");
    while (close >= 0 && isEscaped(line, close)) close = line.indexOf("`", close + 1);
    if (close < 0) {
      token(tokens, 0, line.length, "string");
      return { tokens, state };
    }
    token(tokens, 0, close + 1, "string");
    state.mode = "";
    position = close + 1;
  } else if (state.mode === "raw-string") {
    const close = line.indexOf(state.delimiter);
    if (close < 0) {
      token(tokens, 0, line.length, "string");
      return { tokens, state };
    }
    token(tokens, 0, close + state.delimiter.length, "string");
    position = close + state.delimiter.length;
    state.mode = "";
    state.delimiter = "";
  }
  while (position < line.length) {
    if (line.startsWith("/*", position)) {
      const close = line.indexOf("*/", position + 2);
      if (close < 0) {
        token(tokens, position, line.length, "comment");
        state.mode = "block-comment";
        break;
      }
      token(tokens, position, close + 2, "comment");
      position = close + 2;
      continue;
    }
    if (language !== "css" && line.startsWith("//", position)) {
      token(tokens, position, line.length, "comment");
      break;
    }
    if (language === "cpp" && line.startsWith('R"', position)) {
      const open = line.indexOf("(", position + 2);
      if (open >= 0 && open - position <= 18) {
        const delimiter = `)${line.slice(position + 2, open)}"`;
        const close = line.indexOf(delimiter, open + 1);
        if (close < 0) {
          token(tokens, position, line.length, "string");
          state.mode = "raw-string";
          state.delimiter = delimiter;
          break;
        }
        token(tokens, position, close + delimiter.length, "string");
        position = close + delimiter.length;
        continue;
      }
    }
    const character = line[position];
    if (character === "\"" || character === "'") {
      const end = quotedEnd(line, position, character);
      token(tokens, position, end, "string");
      position = end;
      continue;
    }
    if ((language === "javascript" || language === "typescript") && character === "`") {
      let close = line.indexOf("`", position + 1);
      while (close >= 0 && isEscaped(line, close)) close = line.indexOf("`", close + 1);
      const end = close < 0 ? line.length : close + 1;
      token(tokens, position, end, "string");
      position = end;
      if (close < 0) state.mode = "template";
      continue;
    }
    if ((language === "javascript" || language === "typescript") && character === "/" && javascriptRegexContext(line, position)) {
      const end = regexEnd(line, position);
      if (end > position + 1) {
        token(tokens, position, end, "string");
        position = end;
        continue;
      }
    }
    if (isIdentifierStart(character)) {
      const start = position++;
      while (position < line.length && isIdentifierPart(line[position])) position += 1;
      const value = line.slice(start, position);
      const role = classifyWord(language, line, start, position, declaration);
      if (role) token(tokens, start, position, role);
      declaration = value === "function" ? "function" :
        /^(class|interface|type|enum|struct|union)$/.test(value) ? "type" : "";
      continue;
    }
    if (/\d/.test(character) || (character === "." && /\d/.test(line[position + 1] || ""))) {
      const end = numberEnd(line, position);
      token(tokens, position, end, "number");
      position = end;
      continue;
    }
    if (OPERATOR_CHARS.has(character)) token(tokens, position, position + 1, "operator");
    position += 1;
  }
  return { tokens, state };
}

function scanPython(line, previous = {}) {
  const tokens = [];
  const state = { ...previous };
  let position = 0;
  let declaration = "";
  if (state.mode === "triple") {
    const close = line.indexOf(state.delimiter);
    if (close < 0) {
      token(tokens, 0, line.length, "string");
      return { tokens, state };
    }
    token(tokens, 0, close + 3, "string");
    position = close + 3;
    state.mode = "";
    state.delimiter = "";
  }
  while (position < line.length) {
    if (line[position] === "#") {
      token(tokens, position, line.length, "comment");
      break;
    }
    const stringMatch = line.slice(position).match(/^(?:[rRuUbBfF]{1,3})?(?:'''|"""|'|")/);
    if (stringMatch && (position === 0 || !isIdentifierPart(line[position - 1]))) {
      const start = position;
      const delimiter = stringMatch[0].endsWith("'''") ? "'''" :
        stringMatch[0].endsWith('"""') ? '"""' : stringMatch[0].slice(-1);
      const contentStart = position + stringMatch[0].length;
      if (delimiter.length === 3) {
        const close = line.indexOf(delimiter, contentStart);
        const end = close < 0 ? line.length : close + 3;
        token(tokens, start, end, "string");
        position = end;
        if (close < 0) {
          state.mode = "triple";
          state.delimiter = delimiter;
        }
      } else {
        const end = quotedEnd(line, contentStart - 1, delimiter);
        token(tokens, start, end, "string");
        position = end;
      }
      continue;
    }
    if (line[position] === "@" && isIdentifierStart(line[position + 1])) {
      let end = position + 2;
      while (end < line.length && (isIdentifierPart(line[end]) || line[end] === ".")) end += 1;
      token(tokens, position, end, "preprocessor");
      position = end;
      continue;
    }
    if (isIdentifierStart(line[position])) {
      const start = position++;
      while (position < line.length && isIdentifierPart(line[position])) position += 1;
      const value = line.slice(start, position);
      const role = classifyWord("python", line, start, position, declaration);
      if (role) token(tokens, start, position, role);
      declaration = value === "def" ? "function" : value === "class" ? "type" : "";
      continue;
    }
    if (/\d/.test(line[position]) || (line[position] === "." && /\d/.test(line[position + 1] || ""))) {
      const end = numberEnd(line, position);
      token(tokens, position, end, "number");
      position = end;
      continue;
    }
    if (OPERATOR_CHARS.has(line[position])) token(tokens, position, position + 1, "operator");
    position += 1;
  }
  return { tokens, state };
}

function scanBash(line, previous = {}) {
  const tokens = [];
  const state = { ...previous };
  if (state.mode === "heredoc") {
    const candidate = state.stripTabs ? line.replace(/^\t+/, "") : line;
    if (candidate.trimEnd() === state.delimiter) {
      const start = line.indexOf(state.delimiter);
      token(tokens, start, start + state.delimiter.length, "preprocessor");
      return { tokens, state: {} };
    }
    token(tokens, 0, line.length, "string");
    return { tokens, state };
  }
  if (line.startsWith("#!")) {
    token(tokens, 0, line.length, "preprocessor");
    return { tokens, state };
  }
  let position = 0;
  let commandPosition = true;
  while (position < line.length) {
    const character = line[position];
    if (character === "#" && (position === 0 || /[\s;|&()]/.test(line[position - 1]))) {
      token(tokens, position, line.length, "comment");
      break;
    }
    if (character === "'" || character === "\"") {
      const end = quotedEnd(line, position, character);
      token(tokens, position, end, "string");
      position = end;
      commandPosition = false;
      continue;
    }
    if (character === "$") {
      let end = position + 1;
      if (line[end] === "{") {
        const close = line.indexOf("}", end + 1);
        end = close < 0 ? line.length : close + 1;
      } else if (line[end] === "(") {
        const close = line.indexOf(")", end + 1);
        end = close < 0 ? line.length : close + 1;
      } else if (/[@*#?$!0-9-]/.test(line[end] || "")) end += 1;
      else while (end < line.length && isIdentifierPart(line[end])) end += 1;
      token(tokens, position, end, "variable");
      position = end;
      commandPosition = false;
      continue;
    }
    if (isIdentifierStart(character)) {
      const start = position++;
      while (position < line.length && (isIdentifierPart(line[position]) || line[position] === "-")) position += 1;
      const value = line.slice(start, position);
      if (KEYWORDS.bash.has(value)) token(tokens, start, position, "keyword");
      else if (commandPosition) token(tokens, start, position, "function");
      else {
        let after = position;
        while (after < line.length && /\s/.test(line[after])) after += 1;
        if (line[after] === "=") token(tokens, start, position, "variable");
      }
      commandPosition = /^(do|then|else|elif)$/.test(value);
      continue;
    }
    if (/\d/.test(character)) {
      const end = numberEnd(line, position);
      token(tokens, position, end, "number");
      position = end;
      commandPosition = false;
      continue;
    }
    if (OPERATOR_CHARS.has(character)) {
      token(tokens, position, position + 1, "operator");
      if (/[;|&()]/.test(character)) commandPosition = true;
    } else if (!/\s/.test(character)) commandPosition = false;
    position += 1;
  }
  const heredoc = line.match(/<<(-)?[\t ]*(['"]?)([A-Za-z_][A-Za-z0-9_]*)\2/);
  const heredocHidden = heredoc && tokens.some((item) =>
    (item.role === "string" || item.role === "comment") &&
    item.start <= heredoc.index && heredoc.index < item.end);
  if (heredoc && !heredocHidden) {
    const start = heredoc.index;
    const end = start + heredoc[0].length;
    for (let index = tokens.length - 1; index >= 0; index -= 1) {
      if (tokens[index].start < end && tokens[index].end > start) tokens.splice(index, 1);
    }
    token(tokens, start, end, "preprocessor");
    state.mode = "heredoc";
    state.delimiter = heredoc[3];
    state.stripTabs = Boolean(heredoc[1]);
  }
  return { tokens, state };
}

function scanCss(line, previous = {}) {
  const result = scanCLike(line, "css", previous);
  if (result.state.mode) return result;
  const occupied = result.tokens.slice().sort((left, right) => left.start - right.start);
  let occupiedIndex = 0;
  const covering = (position) => {
    while (occupiedIndex < occupied.length && occupied[occupiedIndex].end <= position) occupiedIndex += 1;
    const item = occupied[occupiedIndex];
    return item && item.start <= position && position < item.end ? item : null;
  };
  const priorityToken = (start, end, role) => {
    for (let index = result.tokens.length - 1; index >= 0; index -= 1) {
      if (result.tokens[index].start < end && result.tokens[index].end > start) result.tokens.splice(index, 1);
    }
    token(result.tokens, start, end, role);
  };
  for (let position = 0; position < line.length;) {
    if (line.startsWith("--", position)) {
      const customProperty = line.slice(position).match(/^--[A-Za-z0-9_-]+(?=\s*:)/);
      if (customProperty) {
        priorityToken(position, position + customProperty[0].length, "property");
        position += customProperty[0].length;
        continue;
      }
    }
    const item = covering(position);
    if (item) {
      position = item.end;
      continue;
    }
    if (line[position] === "@") {
      let end = position + 1;
      while (end < line.length && /[A-Za-z-]/.test(line[end])) end += 1;
      priorityToken(position, end, "keyword");
      position = end;
      continue;
    }
    if (line[position] === "#") {
      const match = line.slice(position).match(/^#[0-9A-Fa-f]{3,8}\b/);
      if (match) {
        priorityToken(position, position + match[0].length, "literal");
        position += match[0].length;
        continue;
      }
    }
    if (/[A-Za-z_-]/.test(line[position])) {
      const start = position++;
      while (position < line.length && /[A-Za-z0-9_-]/.test(line[position])) position += 1;
      let after = position;
      while (after < line.length && /\s/.test(line[after])) after += 1;
      if (line[after] === ":") priorityToken(start, position, "property");
      continue;
    }
    position += 1;
  }
  result.tokens.sort((left, right) => left.start - right.start || left.end - right.end);
  return result;
}

function scanTag(line, start, end, tokens, continuationName = "") {
  let position = start;
  let name = continuationName;
  if (!continuationName) {
    token(tokens, position, position + 1, "tag");
    position += 1;
    if (line[position] === "/") token(tokens, position, ++position, "tag");
    while (position < end && /\s/.test(line[position])) position += 1;
    const nameStart = position;
    while (position < end && /[A-Za-z0-9:_-]/.test(line[position])) position += 1;
    name = line.slice(nameStart, position).toLowerCase();
    token(tokens, nameStart, position, "tag");
  }
  while (position < end) {
    if (line[position] === ">") {
      token(tokens, position, position + 1, "tag");
      break;
    }
    if (line[position] === "/" && line[position + 1] === ">") {
      token(tokens, position, position + 2, "tag");
      break;
    }
    if (!/[A-Za-z_:]/.test(line[position])) {
      position += 1;
      continue;
    }
    const attributeStart = position++;
    while (position < end && /[A-Za-z0-9:_.-]/.test(line[position])) position += 1;
    const attribute = line.slice(attributeStart, position).toLowerCase();
    token(tokens, attributeStart, position, "attribute");
    while (position < end && /\s/.test(line[position])) position += 1;
    if (line[position] !== "=") continue;
    token(tokens, position, position + 1, "operator");
    position += 1;
    while (position < end && /\s/.test(line[position])) position += 1;
    if (line[position] === "'" || line[position] === "\"") {
      const quote = line[position];
      const valueStart = position;
      const valueEnd = Math.min(quotedEnd(line, position, quote), end);
      token(tokens, valueStart, valueStart + 1, "string");
      const contentEnd = valueEnd > valueStart + 1 && line[valueEnd - 1] === quote ? valueEnd - 1 : valueEnd;
      if (attribute === "style" || attribute.startsWith("on")) {
        const embedded = attribute === "style" ?
          scanCss(line.slice(valueStart + 1, contentEnd)) :
          scanCLike(line.slice(valueStart + 1, contentEnd), "javascript");
        for (const item of embedded.tokens) {
          token(tokens, valueStart + 1 + item.start, valueStart + 1 + item.end, item.role);
        }
      } else token(tokens, valueStart + 1, contentEnd, "string");
      if (contentEnd < valueEnd) token(tokens, contentEnd, valueEnd, "string");
      position = valueEnd;
    } else {
      const valueStart = position;
      while (position < end && !/[\s>]/.test(line[position])) position += 1;
      token(tokens, valueStart, position, "string");
    }
  }
  return name;
}

function tagEnd(line, start) {
  let quote = "";
  for (let position = start + 1; position < line.length; position += 1) {
    if (quote) {
      if (line[position] === quote && !isEscaped(line, position)) quote = "";
    } else if (line[position] === "'" || line[position] === "\"") quote = line[position];
    else if (line[position] === ">") return position + 1;
  }
  return line.length;
}

function scanHtml(line, previous = {}) {
  const tokens = [];
  const state = { mode: "html", nested: {}, ...previous };
  let position = 0;
  while (position < line.length) {
    if (state.mode === "tag") {
      const end = tagEnd(line, -1);
      scanTag(line, 0, end, tokens, state.tagName);
      if (!line.slice(0, end).endsWith(">")) break;
      const name = state.tagName;
      const closing = state.closing;
      const selfClosing = line.slice(0, end).trimEnd().endsWith("/>");
      state.mode = "html";
      state.tagName = "";
      state.closing = false;
      position = end;
      if (!closing && !selfClosing && (name === "script" || name === "style")) {
        state.mode = name;
        state.nested = {};
      }
      continue;
    }
    if (state.mode === "script" || state.mode === "style") {
      const closeText = state.mode === "script" ? "</script" : "</style";
      const close = line.toLowerCase().indexOf(closeText, position);
      const codeEnd = close < 0 ? line.length : close;
      const embedded = state.mode === "script" ?
        scanCLike(line.slice(position, codeEnd), "javascript", state.nested) :
        scanCss(line.slice(position, codeEnd), state.nested);
      for (const item of embedded.tokens) token(tokens, position + item.start, position + item.end, item.role);
      state.nested = embedded.state;
      if (close < 0) break;
      state.mode = "html";
      state.nested = {};
      position = close;
      continue;
    }
    if (state.mode === "html-comment") {
      const close = line.indexOf("-->", position);
      if (close < 0) {
        token(tokens, position, line.length, "comment");
        break;
      }
      token(tokens, position, close + 3, "comment");
      state.mode = "html";
      position = close + 3;
      continue;
    }
    const open = line.indexOf("<", position);
    if (open < 0) break;
    if (line.startsWith("<!--", open)) {
      const close = line.indexOf("-->", open + 4);
      if (close < 0) {
        token(tokens, open, line.length, "comment");
        state.mode = "html-comment";
        break;
      }
      token(tokens, open, close + 3, "comment");
      position = close + 3;
      continue;
    }
    const end = tagEnd(line, open);
    if (line.startsWith("<!", open) || line.startsWith("<?", open)) {
      token(tokens, open, end, "preprocessor");
      position = end;
      continue;
    }
    const name = scanTag(line, open, end, tokens);
    const closing = line[open + 1] === "/";
    const selfClosing = line.slice(open, end).trimEnd().endsWith("/>");
    position = end;
    if (!line.slice(open, end).endsWith(">")) {
      state.mode = "tag";
      state.tagName = name;
      state.closing = closing;
      break;
    }
    if (!closing && !selfClosing && (name === "script" || name === "style")) {
      state.mode = name;
      state.nested = {};
    }
  }
  tokens.sort((left, right) => left.start - right.start || left.end - right.end);
  return { tokens, state };
}

function scanLine(line, language, state) {
  if (language === "python") return scanPython(line, state);
  if (language === "bash") return scanBash(line, state);
  if (language === "css") return scanCss(line, state);
  if (language === "html") return scanHtml(line, state);
  return scanCLike(line, language, state);
}

function appendText(documentRef, parent, text) {
  if (text) parent.append(documentRef.createTextNode(text));
}

function appendLine(documentRef, parent, line, tokens) {
  let position = 0;
  for (const item of tokens) {
    if (item.start < position || item.end > line.length) continue;
    appendText(documentRef, parent, line.slice(position, item.start));
    const span = documentRef.createElement("span");
    span.className = `syntax-${item.role}`;
    appendText(documentRef, span, line.slice(item.start, item.end));
    parent.append(span);
    position = item.end;
  }
  appendText(documentRef, parent, line.slice(position));
}

export function appendHighlightedCode(documentRef, parent, source, languageLabel) {
  const text = String(source ?? "");
  const language = canonicalLanguage(languageLabel);
  if (!language || text.length > MAX_HIGHLIGHT_BYTES) {
    appendText(documentRef, parent, text);
    return false;
  }
  const lines = text.replace(/\r\n?/g, "\n").split("\n");
  let state = {};
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index];
    if (line.length > MAX_LINE_BYTES) {
      appendText(documentRef, parent, line);
      state = {};
    } else {
      const result = scanLine(line, language, state);
      appendLine(documentRef, parent, line, result.tokens);
      state = result.state;
    }
    if (index + 1 < lines.length) appendText(documentRef, parent, "\n");
  }
  return true;
}
