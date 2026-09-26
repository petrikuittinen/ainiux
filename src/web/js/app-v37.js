import { renderMarkdown } from "./highlight-v5.js";
import { createSelector } from "./selector-v3.js";
import { appendHighlightedCode, languageForPath } from "./syntax-v4.js";
import {
  detectIndentation, indentEditorSnapshot, outdentEditorSnapshot, reformatEditorSnapshot,
} from "./editor-indentation-v1.js";
import {
  normalizeImageCatalog, selectImageModel, imageFileError, customDimensionError,
  resetImageFormValues,
} from "./image-options-v1.js";
import {
  normalizeVideoCatalog, selectVideoModel, videoFileError,
  cropVideoFileName, videoInputStatus,
} from "./video-options-v3.js";
import {
  createEditorHistory, editorHistoryDirection, recordEditorChange, redoEditorChange,
  undoEditorChange, updateEditorHistorySelection,
} from "./editor-history-v2.js";

const API_ROOT = "/ainiux/v1";
const TOKEN_STORAGE_KEY = "ainiux.controller.token.v1";
const CSRF_HEADER = "X-Ainiux-CSRF-Token";
const THEME_STORAGE_KEY = "ainiux.ui.theme.v1";
const THINKING_STORAGE_KEY = "ainiux.chat.thinking.v1";
const TERMINAL_STATES = new Set(["succeeded", "failed", "cancelled"]);
const MAX_EDITOR_BYTES = 1024 * 1024;
const MAX_CONVERSATION_NOTICES = 150;
const SURFACE_NOTICE_IDS = new Map([
  ["jobs-panel", "jobs-notice"],
  ["image-panel", "image-notice"],
  ["video-panel", "video-notice"],
  ["workspace-panel", "workspace-notice"],
  ["settings-panel", "settings-notice"],
]);

const state = {
  token: "",
  csrfToken: "",
  authenticated: false,
  connected: false,
  reconnectAttempt: 0,
  reconnectTimer: null,
  capabilities: null,
  status: null,
  jobs: new Map(),
  streams: new Map(),
  threads: [],
  thread: null,
  chatInitialized: false,
  startingNewChat: false,
  chatEdit: null,
  chatBusy: new Set(),
  chatPendingJobId: "",
  chatPendingJobByThread: new Map(),
  chatStreams: new Map(),
  chatRegenerateQueued: false,
  showThinkingTraces: false,
  chatWebSearch: false,
  chatMetrics: new Map(),
  sessions: [],
  session: null,
  agentInitializing: false,
  agentSettingsPending: false,
  agentLogs: new Map(),
  agentActivities: new Map(),
  agentSeenEvents: new Map(),
  agentClock: null,
  agentClockTimer: null,
  modelCatalogs: new Map(),
  workspaceSettings: null,
  agentHistory: new Map(),
  imageJobId: "",
  imageSubmitting: false,
  imageResult: null,
  imageError: "",
  imageCatalog: null,
  imageInputs: [],
  chatInputs: [],
  videoJobId: "",
  videoSubmitting: false,
  videoResult: null,
  videoError: "",
  videoCatalog: null,
  videoRenderedModel: "",
  videoInputs: [],
  videoObjectUrl: "",
  guard: null,
  directory: { path: ".", revision: "", entries: [] },
  file: null,
  mutation: null,
  conflictAction: null,
  chatNotices: new Map(),
  chatUnscopedNotices: [],
  agentNotices: new Map(),
  agentUnscopedNotices: [],
  surfaceNotices: new Map(),
  nextNoticeId: 1,
};

const byId = (id) => document.getElementById(id);
const picker = createSelector(document);
const pickerButtons = new Map();
const threadSettingsSaves = new Map();
const threadSettingsSnapshots = new Map();
const chatSendAborts = new Map();
let threadLoadSequence = 0;
let workspaceSavePending = false;
let workspaceModelPickerQueued = "";
let chatRenderFrame = null;
let pendingChatStream = null;
let agentRenderFrame = null;
let editorInsertController = null;

class ApiError extends Error {
  constructor(status, code, message, details = {}) {
    super(message || `Request failed with HTTP ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.code = code || "request_failed";
    this.details = details && typeof details === "object" ? details : {};
  }
}

function element(tag, className = "", text = "") {
  const result = document.createElement(tag);
  if (className) result.className = className;
  if (text !== "") result.textContent = String(text);
  return result;
}

function clear(node) {
  node.replaceChildren();
  node.classList.remove("empty-state");
}

function setEmpty(node, message) {
  node.replaceChildren();
  node.classList.add("empty-state");
  node.textContent = message;
}

function openDialog(dialog) {
  if (!dialog.open) dialog.showModal();
}

function closeDialog(dialog) {
  if (dialog.open) dialog.close();
}

let confirmResolver = null;

function finishConfirm(accepted) {
  const resolve = confirmResolver;
  confirmResolver = null;
  closeDialog(byId("confirm-dialog"));
  if (resolve) resolve(!!accepted);
}

function askConfirm({ title, message, confirmLabel = "OK", danger = false } = {}) {
  return new Promise((resolve) => {
    if (confirmResolver) finishConfirm(false);
    confirmResolver = resolve;
    byId("confirm-title").textContent = title || "Confirm";
    byId("confirm-message").textContent = message || "";
    const submit = byId("confirm-submit");
    submit.textContent = confirmLabel || "OK";
    submit.classList.toggle("danger", danger === true);
    openDialog(byId("confirm-dialog"));
  });
}

function storageGet(key) {
  try {
    return localStorage.getItem(key) || "";
  } catch (_) {
    return "";
  }
}

function storageSet(key, value) {
  try {
    if (value) localStorage.setItem(key, value);
    else localStorage.removeItem(key);
  } catch (_) {
    // Private browsing policies may disable local storage; memory mode still works.
  }
}

function applyTheme(theme) {
  if (!["auto", "dark", "light"].includes(theme)) return false;
  document.documentElement.dataset.theme = theme;
  byId("theme-select").value = theme;
  storageSet(THEME_STORAGE_KEY, theme);
  return true;
}

function handleThemeCommand(text) {
  if (!/^\/theme(?:\s|$)/i.test(text)) return false;
  const match = text.match(/^\/theme(?:\s+(auto|light|dark))?\s*$/i);
  if (!match) {
    targetNotice(activeNoticeTarget(), "Usage: /theme light or /theme dark", "error");
    return true;
  }
  if (!match[1]) {
    return true;
  }
  applyTheme(match[1].toLowerCase());
  return true;
}

function handleChatSlashCommand(text) {
  if (handleThemeCommand(text)) return true;
  if (activePanelId() === "chat-panel") {
    const fetchMatch = text.match(/^\/fetch(?:\s+(\S+))?\s*$/i);
    if (fetchMatch) {
      if (!state.thread) {
        chatNotice("Select a chat thread first", "error", null);
        return true;
      }
      if (!fetchMatch[1]) {
        chatNotice("Usage: /fetch URL", "error", state.thread.id);
        return true;
      }
      void fetchChatUrl(fetchMatch[1]);
      return true;
    }
  }
  const pdf = text.match(/^\/(chat-to-pdf|last-to-pdf)\s*$/i);
  const docx = text.match(/^\/(chat-to-docx|last-to-docx)\s*$/i);
  if (!pdf && !docx) return false;
  if (!state.thread) {
    chatNotice("Select a chat thread first", "error", null);
    return true;
  }
  const kind = docx ? "docx" : "pdf";
  const capability = kind === "docx" ? "chat_docx" : "chat_pdf";
  if (!supports(capability)) {
    chatNotice(kind === "docx" ? "This server does not export chat DOCX files" : "This server does not export chat PDFs", "error", state.thread.id);
    return true;
  }
  const command = (docx || pdf)[1].toLowerCase();
  void downloadChatDocument(kind, command.startsWith("last-to-") ? "last" : "thread");
  return true;
}

function chatExportAvailable() {
  return supports("chat_pdf") || supports("chat_docx") || supports("chat_md") || supports("chat_json");
}

function chatExportRoute(kind) {
  if (kind === "docx") {
    return {
      path: "docx",
      accept: "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
      extension: "docx",
      fallback: "Could not export the chat DOCX",
    };
  }
  if (kind === "json") {
    return {
      path: "json",
      accept: "application/json",
      extension: "json",
      fallback: "Could not export the chat JSON",
    };
  }
  if (kind === "md") {
    return {
      path: "md",
      accept: "text/markdown",
      extension: "md",
      fallback: "Could not export the chat Markdown",
    };
  }
  return {
    path: "pdf",
    accept: "application/pdf",
    extension: "pdf",
    fallback: "Could not export the chat PDF",
  };
}

async function downloadChatDocument(kind, scope, thread = state.thread, errorNode = null) {
  const threadId = thread?.id ?? null;
  const report = (message) => {
    if (errorNode) errorNode.textContent = message;
    chatNotice(message, "error", threadId);
  };
  if (!thread) {
    report("Select a chat thread first");
    return false;
  }
  const route = chatExportRoute(kind);
  try {
    const response = await controlFetch(
      `${API_ROOT}/chat/threads/${encodeURIComponent(thread.id)}/${route.path}`, {
        method: "POST",
        headers: {
          Accept: route.accept,
          "Content-Type": "application/json",
        },
        body: JSON.stringify({ revision: thread.revision, scope }),
        credentials: "omit",
        cache: "no-store",
        referrerPolicy: "no-referrer",
      });
    if (!response.ok) {
      let message = route.fallback;
      try {
        const payload = await response.json();
        if (payload?.error?.message) message = payload.error.message;
      } catch (_) {}
      if (response.status === 401) invalidateAuthentication();
      throw new Error(message);
    }
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `${scope === "last" ? "last" : "chat"}.${route.extension}`;
    document.body.append(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
    return true;
  } catch (error) {
    report(errorMessage(error));
    return false;
  }
}

function openChatExport(thread, scope) {
  if (!thread || !chatExportAvailable()) return;
  state.chatExport = { thread, scope };
  byId("export-description").textContent = scope === "last"
    ? "Export this message." : "Export this thread.";
  byId("export-error").textContent = "";
  byId("export-json").hidden = !supports("chat_json");
  byId("export-pdf").hidden = !supports("chat_pdf");
  byId("export-docx").hidden = !supports("chat_docx");
  byId("export-md").hidden = !supports("chat_md");
  openDialog(byId("export-dialog"));
}

async function importChatFile(file) {
  if (!supports("chat_threads")) {
    chatNotice("This server does not accept chat import", "error", state.thread?.id ?? null);
    return;
  }
  try {
    const created = await api(`${API_ROOT}/chat/import`, {
      method: "POST",
      rawBody: await file.text(),
      contentType: "application/json",
    });
    await loadThreads();
    if (created?.thread?.id) await loadThread(created.thread.id);
  } catch (error) {
    chatNotice(errorMessage(error), "error", state.thread?.id ?? null);
  }
}

async function exportChatTable(markdown, filename) {
  if (!supports("chat_xlsx")) {
    chatNotice("This server does not export tables to XLSX", "error", state.thread?.id ?? null);
    return;
  }
  try {
    const response = await controlFetch(`${API_ROOT}/chat/tables/xlsx`, {
      method: "POST",
      headers: {
        Accept: "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ markdown }),
      credentials: "omit",
      cache: "no-store",
      referrerPolicy: "no-referrer",
    });
    if (!response.ok) {
      let message = "Could not export the table";
      try {
        const payload = await response.json();
        if (payload?.error?.message) message = payload.error.message;
      } catch (_) {}
      if (response.status === 401) invalidateAuthentication();
      throw new Error(message);
    }
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = filename || "table.xlsx";
    document.body.append(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  } catch (error) {
    chatNotice(errorMessage(error), "error", state.thread?.id ?? null);
  }
}

function errorMessage(error) {
  if (error instanceof ApiError) return `${error.message} (${error.code})`;
  return error instanceof Error ? error.message : "Unexpected controller error";
}

function activePanelId() {
  const panel = document.querySelector(".panel.active");
  return panel ? panel.id : "";
}

function noticeSeverity(value) {
  return value === "warning" || value === "error" ? value : "message";
}

function noticeRecord(message, severity, fields = {}) {
  return {
    id: state.nextNoticeId++,
    message: String(message || ""),
    severity: noticeSeverity(severity),
    ...fields,
  };
}

function appendBoundedNotice(records, record) {
  records.push(record);
  if (records.length > MAX_CONVERSATION_NOTICES) {
    records.splice(0, records.length - MAX_CONVERSATION_NOTICES);
  }
}

function scopedNotice(recordsByScope, unscoped, scopeId, message, severity, fields = {}) {
  const record = noticeRecord(message, severity, fields);
  if (scopeId == null || scopeId === "") {
    appendBoundedNotice(unscoped, record);
  } else {
    const records = recordsByScope.get(scopeId) || [];
    appendBoundedNotice(records, record);
    recordsByScope.set(scopeId, records);
  }
  return record;
}

function nextChatOrdinal(threadId) {
  const thread = state.thread?.id === threadId
    ? state.thread : state.threads.find((item) => item.id === threadId);
  if (!thread) return null;
  const count = Number(thread.message_count);
  if (Number.isSafeInteger(count) && count >= 0) return count;
  const messages = Array.isArray(thread.messages) ? thread.messages : [];
  const last = messages.length ? messages[messages.length - 1] : null;
  const ordinal = Number(last?.ordinal);
  return Number.isSafeInteger(ordinal) && ordinal >= 0 ? ordinal + 1 : messages.length;
}

function chatNotice(message, severity = "message", threadId = state.thread?.id ?? null,
                    beforeOrdinal = null) {
  const nextOrdinal = beforeOrdinal === null ? nextChatOrdinal(threadId) : beforeOrdinal;
  scopedNotice(state.chatNotices, state.chatUnscopedNotices, threadId, message, severity,
    { nextOrdinal: Number.isSafeInteger(Number(nextOrdinal)) ? Number(nextOrdinal) : null });
  if ((threadId == null && !state.thread) || state.thread?.id === threadId) renderChat();
}

function agentNotice(message, severity = "message", sessionId = state.session?.id ?? null) {
  scopedNotice(state.agentNotices, state.agentUnscopedNotices, sessionId, message, severity);
  if ((sessionId == null && !state.session) || state.session?.id === sessionId) renderAgent();
}

function pruneChatNoticesFrom(threadId, firstRemovedOrdinal) {
  const records = state.chatNotices.get(threadId);
  if (!records) return;
  const boundary = Number(firstRemovedOrdinal);
  if (!Number.isSafeInteger(boundary) || boundary < 0) return;
  const retained = records.filter((record) =>
    record.nextOrdinal === null || record.nextOrdinal < boundary);
  if (retained.length) state.chatNotices.set(threadId, retained);
  else state.chatNotices.delete(threadId);
}

function appendConversationNotice(container, record, cardClass) {
  const prefix = record.severity === "message" ? "💬 message" :
    record.severity === "warning" ? "⚠️ warning" : "⚠️ error";
  const card = element("article", `${cardClass} browser-notice`);
  card.dataset.severity = record.severity;
  card.append(element("div", cardClass === "message" ? "role" : "event-type", prefix),
    element("p", "browser-notice-text", record.message));
  container.append(card);
}

function appendChatTimeline(container, transcript, notices, allowPrint = false) {
  const ordered = [...notices].sort((left, right) => {
    const leftBoundary = Number.isSafeInteger(left.nextOrdinal)
      ? left.nextOrdinal : Number.MAX_SAFE_INTEGER;
    const rightBoundary = Number.isSafeInteger(right.nextOrdinal)
      ? right.nextOrdinal : Number.MAX_SAFE_INTEGER;
    return leftBoundary - rightBoundary || left.id - right.id;
  });
  const last = transcript[transcript.length - 1];
  const printOrdinal = allowPrint && last && last.role === "assistant" ? last.ordinal : null;
  let noticeIndex = 0;
  for (const message of transcript) {
    const ordinal = Number(message.ordinal);
    while (noticeIndex < ordered.length && Number.isSafeInteger(ordinal) &&
           Number.isSafeInteger(ordered[noticeIndex].nextOrdinal) &&
           ordered[noticeIndex].nextOrdinal <= ordinal) {
      appendConversationNotice(container, ordered[noticeIndex++], "message");
    }
    appendChatMessage(container, message.role, message.content, false, message.ordinal,
      message.attachments, Number(message.ordinal) === Number(printOrdinal));
  }
  while (noticeIndex < ordered.length) {
    appendConversationNotice(container, ordered[noticeIndex++], "message");
  }
}

function renderSurfaceNotice(panelId) {
  const areaId = SURFACE_NOTICE_IDS.get(panelId);
  if (!areaId) return;
  const area = byId(areaId);
  const record = state.surfaceNotices.get(panelId);
  area.replaceChildren();
  area.hidden = !record;
  if (!record) return;
  const prefix = record.severity === "message" ? "💬 message" :
    record.severity === "warning" ? "⚠️ warning" : "⚠️ error";
  const text = element("span", "inline-notice-text");
  text.append(element("strong", "inline-notice-prefix", prefix),
    document.createTextNode(` ${record.message}`));
  const dismiss = element("button", "ghost inline-notice-dismiss", "Dismiss");
  dismiss.type = "button";
  dismiss.addEventListener("click", () => {
    if (state.surfaceNotices.get(panelId)?.id === record.id) {
      state.surfaceNotices.delete(panelId);
      renderSurfaceNotice(panelId);
    }
  });
  area.className = `inline-notice-area ${record.severity}`;
  area.append(text, dismiss);
}

function surfaceNotice(panelId, message, severity = "message") {
  if (!SURFACE_NOTICE_IDS.has(panelId)) panelId = "settings-panel";
  state.surfaceNotices.set(panelId, noticeRecord(message, severity));
  renderSurfaceNotice(panelId);
}

function activeNoticeTarget() {
  const panelId = activePanelId();
  if (panelId === "chat-panel") return { panelId, scopeId: state.thread?.id ?? null };
  if (panelId === "agent-panel") return { panelId, scopeId: state.session?.id ?? null };
  return { panelId: SURFACE_NOTICE_IDS.has(panelId) ? panelId : "settings-panel", scopeId: null };
}

function targetNotice(target, message, severity = "message") {
  if (target.panelId === "chat-panel") chatNotice(message, severity, target.scopeId);
  else if (target.panelId === "agent-panel") agentNotice(message, severity, target.scopeId);
  else surfaceNotice(target.panelId, message, severity);
}

function jobNotice(job, message, severity = "message") {
  if (job?._context?.type === "chat") {
    chatNotice(message, severity, job._context.threadId);
    return;
  }
  if (job?.operation === "image") surfaceNotice("image-panel", message, severity);
  else if (job?.operation === "video") surfaceNotice("video-panel", message, severity);
  else if (job?.operation === "editor-assist" || job?._context?.type === "assist") {
    surfaceNotice("workspace-panel", message, severity);
  } else {
    surfaceNotice("jobs-panel", message, severity);
  }
}

function clearTransientNotices() {
  state.chatNotices.clear();
  state.chatUnscopedNotices.length = 0;
  state.agentNotices.clear();
  state.agentUnscopedNotices.length = 0;
  state.surfaceNotices.clear();
  for (const panelId of SURFACE_NOTICE_IDS.keys()) renderSurfaceNotice(panelId);
}

function appendDisplaySegment(segments, kind, text) {
  if (!text) return;
  const previous = segments[segments.length - 1];
  if (previous && previous.kind === kind) previous.text += text;
  else segments.push({ kind, text });
}

function chatDisplaySegments(content) {
  const text = String(content || "");
  const lower = text.toLowerCase();
  const openTag = "<think>";
  const closeTag = "</think>";
  const segments = [];
  let position = 0;
  while (position < text.length) {
    const open = lower.indexOf(openTag, position);
    if (open === -1) {
      let remainder = text.slice(position);
      if (!state.showThinkingTraces) {
        const lowerRemainder = lower.slice(position);
        for (let length = Math.min(openTag.length - 1, lowerRemainder.length); length > 0; --length) {
          if (lowerRemainder.endsWith(openTag.slice(0, length))) {
            remainder = remainder.slice(0, -length);
            break;
          }
        }
      }
      appendDisplaySegment(segments, "markdown", remainder);
      break;
    }
    appendDisplaySegment(segments, "markdown", text.slice(position, open));
    const close = lower.indexOf(closeTag, open + openTag.length);
    if (close === -1) {
      if (state.showThinkingTraces) appendDisplaySegment(segments, "thinking", text.slice(open));
      break;
    }
    if (state.showThinkingTraces) {
      appendDisplaySegment(segments, "thinking", text.slice(open, close + closeTag.length));
    }
    position = close + closeTag.length;
  }
  if (!state.showThinkingTraces) {
    while (segments.length && !segments[0].text.replace(/[\r\n]/g, "")) segments.shift();
    while (segments.length && !segments[segments.length - 1].text.replace(/[\r\n]/g, "")) segments.pop();
    if (segments.length) {
      segments[0].text = segments[0].text.replace(/^[\r\n]+/, "");
      segments[segments.length - 1].text = segments[segments.length - 1].text.replace(/[\r\n]+$/, "");
    }
  }
  return segments;
}

function appendMarkdown(parent, text, interactive = false) {
  try {
    parent.append(renderMarkdown(text, document, interactive ? {
      blocks: true,
      onNotice: (message) => chatNotice(message, "error", state.thread?.id ?? null),
      onExportTable: (markdown, filename) => void exportChatTable(markdown, filename),
    } : null));
  } catch (_) {
    parent.append(element("pre", "", text));
  }
}

function appendHighlightedSource(parent, text, language) {
  const code = element("code");
  appendHighlightedCode(document, code, text, language);
  parent.append(code);
}

function renderChatContent(output, role, content, streaming) {
  output.replaceChildren();
  if (role === "assistant") {
    const segments = chatDisplaySegments(content);
    for (const segment of segments) {
      if (segment.kind === "thinking") output.append(element("pre", "thinking-trace", segment.text));
      else appendMarkdown(output, segment.text, !streaming);
    }
    if (!output.textContent.trim() && streaming) output.append(element("pre", "", "Thinking…"));
    return;
  }
  if (role === "user") {
    appendMarkdown(output, String(content || ""), true);
    return;
  }
  output.append(element("pre", "", String(content || "")));
}

function formatBytes(value) {
  const bytes = Number(value);
  if (!Number.isFinite(bytes) || bytes < 0) return "—";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
}

function formatDate(value) {
  if (!value) return "Date unavailable";
  const date = new Date(value);
  if (!Number.isFinite(date.getTime())) return String(value);
  try {
    return new Intl.DateTimeFormat(undefined, {
      dateStyle: "medium",
      timeStyle: "short",
    }).format(date);
  } catch (_) {
    return date.toLocaleString();
  }
}

function formatTokenCount(value, estimated = false) {
  if (value === null || value === undefined || value === "") return "";
  const count = Number(value);
  if (!Number.isFinite(count) || count < 0) return "";
  return `${estimated ? "~" : ""}${new Intl.NumberFormat().format(Math.round(count))}`;
}

function formatCompactTokenCount(value) {
  if (typeof value !== "number" || !Number.isFinite(value) || value < 0) return "";
  const count = value;
  const compact = (scaled) => scaled.toFixed(1).replace(/\.0$/, "");
  if (count >= 1000000) return `${compact(count / 1000000)}M`;
  if (count >= 1000) return `${compact(count / 1000)}k`;
  return String(Math.round(count));
}

function formatDuration(value) {
  if (value === null || value === undefined || value === "") return "";
  const milliseconds = Number(value);
  if (!Number.isFinite(milliseconds) || milliseconds < 0) return "";
  return `${new Intl.NumberFormat().format(Math.round(milliseconds))} ms`;
}

function metricText(metrics, context = null, activeElapsed = null, includeContext = true) {
  const source = metrics && typeof metrics === "object" ? metrics : {};
  const pieces = [];
  const used = context && context.used_tokens !== undefined
    ? context.used_tokens : source.context_used_tokens;
  const windowTokens = context && context.window_tokens !== undefined
    ? context.window_tokens : source.context_window_tokens;
  const usedText = formatTokenCount(used, true);
  const windowText = formatTokenCount(windowTokens);
  if (includeContext && usedText) {
    pieces.push(`Context ${usedText}${windowText ? ` / ${windowText}` : ""} tok`);
  }
  const input = formatTokenCount(source.input_tokens, source.input_tokens_estimated === true);
  const output = formatTokenCount(source.output_tokens, source.output_tokens_estimated === true);
  if (input) pieces.push(`In ${input}`);
  if (output) pieces.push(`Out ${output}`);
  const cache = Number(source.cache_read_tokens);
  if (Number.isFinite(cache) && cache > 0) pieces.push(`Cache ${formatTokenCount(cache)}`);
  const elapsed = formatDuration(activeElapsed === null ? source.elapsed_ms : activeElapsed);
  if (elapsed) pieces.push(`Elapsed ${elapsed}`);
  const ttft = formatDuration(source.ttft_ms);
  if (ttft) pieces.push(`TTFT ${ttft}`);
  const rate = source.output_tokens_per_second === null ||
      source.output_tokens_per_second === undefined
    ? Number.NaN : Number(source.output_tokens_per_second);
  if (Number.isFinite(rate) && rate >= 0) pieces.push(`${rate.toFixed(1)} tok/s`);
  return pieces.join(" · ");
}

function appendMetrics(container, metrics) {
  const text = metricText(metrics);
  if (text) container.append(element("small", "metrics-strip", text));
}

function formatValue(value) {
  if (value === null || value === undefined || value === "") return "—";
  if (Array.isArray(value)) return value.join(", ");
  if (typeof value === "object") return JSON.stringify(value);
  return String(value);
}

function setDetails(target, rows) {
  clear(target);
  for (const [name, value] of rows) {
    target.append(element("dt", "", name), element("dd", "", formatValue(value)));
  }
}

function optionalPayload(fields) {
  const output = {};
  for (const [key, value] of Object.entries(fields)) {
    if (value !== "" && value !== null && value !== undefined) output[key] = value;
  }
  return output;
}

function wirePath(path) {
  return String(path).split("/").map((part) => encodeURIComponent(part)).join("/");
}

function mutationMethod(method) {
  return ["POST", "PUT", "PATCH", "DELETE"].includes(String(method || "GET").toUpperCase());
}

function controlHeaders(method, initial = {}, token = state.token, csrfToken = state.csrfToken) {
  if (!token) throw new ApiError(401, "not_connected", "Connect with a controller token first");
  const headers = new Headers(initial);
  headers.set("Authorization", `Bearer ${token}`);
  if (mutationMethod(method)) {
    if (!csrfToken) throw new ApiError(403, "csrf_unavailable", "Reconnect to refresh CSRF protection");
    headers.set(CSRF_HEADER, csrfToken);
  }
  return headers;
}

async function controlFetch(path, options = {}, retryCsrf = true) {
  const method = String(options.method || "GET").toUpperCase();
  const response = await fetch(path, {
    ...options,
    method,
    headers: controlHeaders(method, options.headers || {}),
  });
  if (retryCsrf && mutationMethod(method) && response.status === 403) {
    let failure = {};
    try { failure = (await response.clone().json()).error || {}; } catch (_) { /* handled by caller */ }
    if (failure.code === "csrf_validation_failed") {
      state.csrfToken = "";
      await refreshCsrfToken();
      return controlFetch(path, options, false);
    }
  }
  return response;
}

async function api(path, options = {}) {
  const method = String(options.method || "GET").toUpperCase();
  const headers = new Headers(options.headers || {});
  headers.set("Accept", "application/json");
  let body;
  if (options.rawBody !== undefined) {
    headers.set("Content-Type", options.contentType || "application/octet-stream");
    body = options.rawBody;
  } else if (options.body !== undefined) {
    headers.set("Content-Type", "application/json");
    body = JSON.stringify(options.body);
  }
  let response;
  try {
    response = await controlFetch(path, {
      method,
      headers,
      body,
      signal: options.signal,
      credentials: "omit",
      cache: "no-store",
      referrerPolicy: "no-referrer",
    });
  } catch (error) {
    if (error && error.name === "AbortError") throw error;
    if (state.authenticated) markConnectionLost();
    throw error;
  }
  const text = await response.text();
  let payload = null;
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch (_) {
      throw new ApiError(response.status, "invalid_response", "Server returned invalid JSON");
    }
  }
  if (!response.ok) {
    const failure = payload && payload.error ? payload.error : {};
    const error = new ApiError(response.status, failure.code, failure.message, failure.details);
    if (response.status === 401) invalidateAuthentication();
    throw error;
  }
  return payload;
}

async function refreshCsrfToken() {
  const payload = await api(`${API_ROOT}/csrf`);
  if (!payload || typeof payload.token !== "string" || !payload.token) {
    throw new ApiError(502, "invalid_response", "Server returned an invalid CSRF token");
  }
  state.csrfToken = payload.token;
}

async function readSse(response, onEvent, signal) {
  if (!response.body) throw new ApiError(502, "stream_unavailable", "The browser did not expose the event stream");
  const reader = response.body.getReader();
  const decoder = new TextDecoder();
  let buffer = "";
  let eventName = "message";
  let eventId = "";
  let dataLines = [];

  const dispatch = () => {
    if (dataLines.length === 0) {
      eventName = "message";
      eventId = "";
      return;
    }
    const raw = dataLines.join("\n");
    let data;
    try {
      data = JSON.parse(raw);
    } catch (_) {
      throw new ApiError(502, "invalid_sse", "Server sent malformed event JSON");
    }
    onEvent({ event: eventName, id: eventId, data });
    eventName = "message";
    eventId = "";
    dataLines = [];
  };

  const processLine = (input) => {
    const line = input.endsWith("\r") ? input.slice(0, -1) : input;
    if (line === "") {
      dispatch();
      return;
    }
    if (line.startsWith(":")) return;
    const colon = line.indexOf(":");
    const field = colon === -1 ? line : line.slice(0, colon);
    let value = colon === -1 ? "" : line.slice(colon + 1);
    if (value.startsWith(" ")) value = value.slice(1);
    if (field === "event") eventName = value;
    else if (field === "id" && !value.includes("\0")) eventId = value;
    else if (field === "data") dataLines.push(value);
  };

  try {
    while (!signal.aborted) {
      const chunk = await reader.read();
      if (chunk.done) break;
      buffer += decoder.decode(chunk.value, { stream: true });
      let newline = buffer.indexOf("\n");
      while (newline !== -1) {
        const line = buffer.slice(0, newline);
        buffer = buffer.slice(newline + 1);
        processLine(line);
        newline = buffer.indexOf("\n");
      }
    }
    buffer += decoder.decode();
    if (buffer) processLine(buffer);
    if (dataLines.length) dispatch();
  } finally {
    reader.releaseLock();
  }
}

function stopStream(key) {
  const controller = state.streams.get(key);
  if (controller) controller.abort();
  state.streams.delete(key);
}

function stopAllStreams() {
  for (const controller of state.streams.values()) controller.abort();
  state.streams.clear();
}

function startStream(key, path, onMessage, onExpired, isDone, after = 0) {
  stopStream(key);
  const controller = new AbortController();
  state.streams.set(key, controller);
  void (async () => {
    let cursor = after;
    while (state.connected && !controller.signal.aborted && !isDone()) {
      try {
        const headers = controlHeaders("GET", {
          Accept: "text/event-stream",
        });
        if (cursor > 0) headers.set("Last-Event-ID", String(cursor));
        const response = await fetch(path, {
          headers,
          signal: controller.signal,
          credentials: "omit",
          cache: "no-store",
          referrerPolicy: "no-referrer",
        });
        if (response.status === 410) {
          const resumed = await onExpired();
          cursor = Number.isSafeInteger(resumed) && resumed >= 0 ? resumed : 0;
          if (isDone()) return;
          continue;
        }
        if (!response.ok) {
          let failure = {};
          try { failure = (await response.json()).error || {}; } catch (_) { /* handled below */ }
          throw new ApiError(response.status, failure.code, failure.message, failure.details);
        }
        await readSse(response, (message) => {
          const eventId = Number(message.id || (message.data && message.data.id));
          if (Number.isSafeInteger(eventId) && eventId > cursor) cursor = eventId;
          onMessage(message.data, message.event);
        }, controller.signal);
        if (isDone()) return;
      } catch (error) {
        if (controller.signal.aborted) return;
        if (error instanceof ApiError && error.status === 401) {
          invalidateAuthentication();
          return;
        }
        markConnectionLost();
        return;
      }
      if (!isDone()) {
        markConnectionLost();
        return;
      }
    }
  })().finally(() => {
    if (state.streams.get(key) === controller) state.streams.delete(key);
  });
}

function supports(operation) {
  return Boolean(state.connected && state.capabilities && Array.isArray(state.capabilities.operations) &&
    state.capabilities.operations.includes(operation));
}

function chatTurnBusy(threadId = state.thread && state.thread.id) {
  return threadId != null && state.chatBusy.has(threadId);
}

function beginChatTurn(threadId) {
  state.chatBusy.add(threadId);
}

function endChatTurn(threadId, abort = false) {
  if (threadId == null) {
    if (abort) {
      for (const controller of chatSendAborts.values()) controller.abort();
    }
    chatSendAborts.clear();
    state.chatBusy.clear();
    state.chatPendingJobByThread.clear();
    state.chatPendingJobId = "";
    return;
  }
  if (abort) {
    const controller = chatSendAborts.get(threadId);
    if (controller) controller.abort();
  }
  chatSendAborts.delete(threadId);
  state.chatBusy.delete(threadId);
  state.chatPendingJobByThread.delete(threadId);
  const currentId = state.thread && state.thread.id;
  state.chatPendingJobId = currentId != null
    ? (state.chatPendingJobByThread.get(currentId) || "") : "";
}

function syncChatSendButton() {
  const send = byId("chat-send");
  if (!send) return;
  const blocked = !state.thread || state.thread.read_only === true || !supports("chat");
  const busy = !blocked && chatTurnBusy(state.thread.id);
  send.disabled = blocked;
  send.textContent = busy ? "Cancel" : "Send";
}

function headerFileName(name) {
  const value = String(name || "");
  try {
    const headers = new Headers();
    headers.set("X-Ainiux-Filename", value);
    return value;
  } catch (_) {
    return encodeURIComponent(value);
  }
}

function modelControls() {
  return [
    { providerId: "chat-provider", modelId: "chat-model", listId: "chat-model-list", statusId: "chat-model-status", reasoningId: "chat-reasoning" },
    { providerId: "goal-provider", modelId: "goal-model", listId: "goal-model-list", statusId: "goal-model-status" },
    { providerId: "thread-provider", modelId: "thread-model", listId: "thread-model-list", statusId: "thread-model-status" },
    { providerId: "agent-provider", modelId: "agent-model", listId: "agent-model-list", statusId: "agent-model-status", reasoningId: "agent-reasoning" },
    { providerId: "workspace-provider", modelId: "workspace-model", listId: "workspace-model-list", statusId: "workspace-model-status" },
  ];
}

const FALLBACK_REASONING_OPTIONS = [
  ["auto", "Auto"], ["off", "Off"], ["minimal", "Minimal"], ["low", "Low"],
  ["medium", "Medium"], ["high", "High"], ["xhigh", "XHigh"], ["max", "Max"],
];

function renderReasoningControl(control, catalog) {
  if (!control.reasoningId) return;
  const select = byId(control.reasoningId);
  const previous = select.value;
  const model = byId(control.modelId).value.trim();
  const matched = catalog && catalog.reasoningOptions instanceof Map
    ? catalog.reasoningOptions.get(model) : null;
  const choices = Array.isArray(matched) && matched.length
    ? matched.map((choice) => [choice.value, choice.label])
    : FALLBACK_REASONING_OPTIONS;
  clear(select);
  const defaultOption = element("option", "", "Server default");
  defaultOption.value = "";
  select.append(defaultOption);
  for (const [value, label] of choices) {
    if (typeof value !== "string" || !value || typeof label !== "string") continue;
    const option = element("option", "", label);
    option.value = value;
    select.append(option);
  }
  select.value = [...select.options].some((option) => option.value === previous) ? previous : "";
  select.title = matched
    ? "Reasoning choices for the selected model"
    : "Common reasoning choices; the provider validates support";
  if (control.reasoningId === "chat-reasoning") renderChatToolbar();
}

function modelCatalogKey(control) {
  return byId(control.providerId).value;
}

function renderModelControl(control) {
  const key = modelCatalogKey(control);
  const catalog = state.modelCatalogs.get(key);
  const list = byId(control.listId);
  clear(list);
  const models = catalog && Array.isArray(catalog.models) ? catalog.models : [];
  for (const model of models) {
    const option = element("option");
    option.value = model;
    list.append(option);
  }
  const status = byId(control.statusId);
  if (!supports("models")) status.textContent = state.connected ? "Enter a model manually." : "";
  else if (!catalog || catalog.state === "loading") status.textContent = "Loading models…";
  else if (catalog.state === "ready") status.textContent = "";
  else status.textContent = "Model list unavailable; enter one manually.";
  const input = byId(control.modelId);
  if (["goal-model", "thread-model"].includes(control.modelId) && catalog && catalog.state === "ready" && models.length === 1 && !input.value.trim()) {
    input.value = models[0];
  }
  renderReasoningControl(control, catalog);
  picker.update(`${control.modelId}:${key}`, models, status.textContent);
  syncPickerButtons();
}

function openModelPicker(control, kind) {
  const source = byId(kind === "provider" ? control.providerId : control.modelId);
  if (source.disabled || (control.providerId === "agent-provider" && workspaceSavePending)) return;
  const catalogKey = modelCatalogKey(control);
  const providerItems = [...byId(control.providerId).options]
    .filter((item) => item.value && item.value !== "none")
    .map((item) => ({ value: item.value, label: item.textContent }));
  picker.open({
    key: kind === "provider" ? control.providerId : `${control.modelId}:${catalogKey}`,
    title: kind === "provider" ? "Choose provider" : `Choose a ${catalogKey || "provider"} model`,
    items: kind === "provider" ? providerItems : state.modelCatalogs.get(catalogKey)?.models || [],
    value: source.value, manual: kind === "model", autoSelectOnly: kind === "model",
    status: byId(control.statusId).textContent,
    onReload: kind === "model" ? () => void requestModelCatalog(catalogKey, control) : null,
    onSelect: (value) => {
      if (source.disabled || (kind === "model" && modelCatalogKey(control) !== catalogKey)) return;
      if (kind === "provider") {
        byId(control.modelId).value = "";
        if (["workspace-provider", "agent-provider"].includes(control.providerId)) {
          workspaceModelPickerQueued = control.providerId;
        }
      }
      source.value = value;
      source.dispatchEvent(new Event("change", { bubbles: true }));
      syncPickerButtons();
      if (kind === "provider" &&
          !["workspace-provider", "agent-provider"].includes(control.providerId)) {
        window.setTimeout(() => openModelPicker(control, "model"), 0);
      }
    },
  });
}

function syncPickerButtons() {
  for (const [id, button] of pickerButtons) {
    const source = byId(id);
    button.disabled = source.disabled;
    button.textContent = source.tagName === "SELECT"
      ? source.selectedOptions[0]?.textContent || source.value || "Server default"
      : source.value.trim() || "Default model";
  }
  for (const scope of ["chat", "agent", "workspace"]) {
    const configuration = scope === "chat" ? state.thread :
      scope === "agent" ? state.session : state.workspaceSettings;
    const control = modelControls().find((item) => item.providerId === `${scope}-provider`);
    for (const kind of ["provider", "model"]) {
      const link = byId(`${scope}-${kind}-link`);
      const source = byId(kind === "provider" ? control.providerId : control.modelId);
      const fallback = state.startingNewChat && scope === "chat" ? "Starting new chat…" : "—";
      const value = configuration && typeof configuration[kind] === "string"
        ? configuration[kind] || (kind === "provider" ? "none" : "Default model") : fallback;
      const label = kind === "provider" ? "Provider" : "Model";
      link.textContent = `${label}: ${value}`;
      link.setAttribute("aria-label", `Choose ${kind}: ${value}`);
      link.setAttribute("aria-disabled", String(source.disabled ||
        (scope === "agent" && workspaceSavePending)));
      link.setAttribute("aria-haspopup", "dialog");
      link.setAttribute("aria-keyshortcuts", kind === "provider" ? "Alt+P" : "Alt+M");
      link.title = `${label}: ${value} · Choose ${kind} · Alt+${kind === "provider" ? "P" : "M"}`;
    }
  }
}

function installPickerControls() {
  for (const control of modelControls()) {
    if (control.providerId === "agent-provider") continue;
    for (const kind of ["provider", "model"]) {
      const id = kind === "provider" ? control.providerId : control.modelId;
      const source = byId(id);
      const button = element("button", "picker-trigger", kind === "model" ? "Choose…" : "Server default");
      button.type = "button"; button.setAttribute("aria-label", `Choose ${kind}`);
      button.setAttribute("aria-haspopup", "dialog");
      button.setAttribute("aria-keyshortcuts", kind === "provider" ? "Alt+P" : "Alt+M");
      button.title = `Choose ${kind} · Alt+${kind === "provider" ? "P" : "M"}`;
      source.after(button); pickerButtons.set(id, button);
      source.hidden = true;
      if (kind === "model") source.removeAttribute("list");
      button.addEventListener("click", () => openModelPicker(control, kind));
      source.addEventListener("keydown", (event) => {
        if (event.key === "ArrowDown" && event.altKey) { event.preventDefault(); openModelPicker(control, kind); }
      });
    }
  }
  syncPickerButtons();
}

function setupModelSettings() {
  const grid = byId("settings-panel").querySelector(".settings-grid");
  const appearance = byId("appearance-settings-card");
  for (const [scope, title] of [["chat", "Current chat"], ["workspace", "Workspace agent & editor"]]) {
    const section = element("section", "control-pane stack settings-card model-settings-card");
    section.dataset.settingsScope = scope;
    section.addEventListener("focusin", () => { modelSettingsScope = scope; });
    section.append(element("h2", "", title));
    const targets = element("div", "form-grid settings-routing-controls");
    if (scope === "workspace") {
      let modelLabel = null;
      for (const kind of ["provider", "model"]) {
        const label = element("label", "", kind === "provider" ? "Provider " : "Model ");
        const input = element(kind === "provider" ? "select" : "input"); input.id = `workspace-${kind}`;
        if (kind === "provider") input.className = "provider-select";
        else { input.maxLength = 512; input.autocomplete = "off"; modelLabel = label; }
        label.append(input); targets.append(label);
      }
      const list = element("datalist"); list.id = "workspace-model-list";
      const status = element("small", "field-hint"); status.id = "workspace-model-status";
      modelLabel.append(list, status);
    } else {
      targets.append(...byId("chat-model-controls").children);
      byId("chat-model-controls").remove();
    }
    const summary = element("p", "field-hint"); summary.id = `${scope}-settings-summary`;
    const fields = element("div", "form-grid model-settings-fields"); fields.id = `${scope}-settings-fields`;
    const status = element("p", "field-hint"); status.id = `${scope}-settings-save-status`; status.setAttribute("role", "status");
    section.append(targets, summary, fields, status);
    grid.insertBefore(section, appearance);
  }
  for (const scope of ["chat", "agent", "workspace"]) {
    const control = modelControls().find((item) => item.providerId === `${scope}-provider`);
    for (const kind of ["provider", "model"]) {
      byId(`${scope}-${kind}-link`).addEventListener("click", (event) => {
        event.preventDefault();
        openModelPicker(control, kind);
      });
    }
  }
  for (const [container, scope] of [["assist-dialog", "workspace"]]) {
    const button = element("button", "ghost", "Model settings"); button.type = "button";
    button.addEventListener("click", () => {
      if (container === "assist-dialog") closeDialog(byId(container));
      showModelSettings(scope);
    });
    const target = container === "agent-panel" ? byId(container).querySelector(".agent-toolbar")
      : byId(container).querySelector(".dialog-actions");
    target.append(button);
  }
}

let modelSettingsScope = "chat";
function showModelSettings(scope, kind = "provider") {
  switchPanel("settings-panel");
  modelSettingsScope = scope;
  const target = pickerButtons.get(`${scope}-${kind}`);
  byId(`${scope}-settings-summary`).scrollIntoView({ block: "nearest" });
  if (target && !target.disabled) target.focus();
}

function modelShortcutControl(modal) {
  if (document.querySelector(".model-picker[open]")) return null;
  if (modal) {
    if (modal.id === "new-thread-dialog") return modelControls().find((control) => control.providerId === "thread-provider");
    return null;
  }
  const panel = activePanelId();
  let scope;
  if (panel === "chat-panel") scope = "chat";
  else if (panel === "agent-panel") scope = "agent";
  else if (panel === "workspace-panel") scope = "workspace";
  else if (panel === "jobs-panel") scope = "goal";
  else if (panel === "settings-panel") scope = document.activeElement.closest("[data-settings-scope]")?.dataset.settingsScope || modelSettingsScope;
  return modelControls().find((control) => control.providerId === `${scope}-provider`);
}

function renderModelSettings(scope) {
  const configuration = scope === "chat" ? state.thread : state.workspaceSettings;
  const fields = byId(`${scope}-settings-fields`);
  byId(`${scope}-settings-summary`).textContent = configuration
    ? `${scope === "chat" ? configuration.name || "Chat" : "Shared workspace settings"} · ${configuration.provider || "Server default"} / ${configuration.model || "default"}`
    : scope === "chat" ? "Select a chat thread." : "Connect to load workspace settings.";
  const signature = JSON.stringify([configuration?.settings, configuration?.settings_fields]);
  if (fields.dataset.signature === signature) return;
  fields.dataset.signature = signature; fields.replaceChildren();
  for (const field of configuration?.settings_fields || []) {
    const label = element("label", "", `${field.id.replaceAll("_", " ")} `);
    const choices = field.choices || [];
    const input = element(choices.length ? "select" : "input");
    if (choices.length) {
      if (field.optional) { const option = element("option", "", "Default"); option.value = ""; input.append(option); }
      for (const value of choices) { const option = element("option", "", value); option.value = value; input.append(option); }
      const value = configuration.settings[field.id] || "";
      if (value && ![...input.options].some((option) => option.value === value)) {
        const option = element("option", "", value); option.value = value; input.append(option);
      }
    } else { input.maxLength = 128; input.autocomplete = "off"; input.placeholder = field.optional ? "Default" : ""; }
    input.value = configuration.settings[field.id] || "";
    input.addEventListener("change", () => {
      const save = scope === "chat" ? saveChatSettings : saveWorkspaceSettings;
      void save({ settings: { [field.id]: input.value } });
    });
    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && !event.isComposing) { event.preventDefault(); input.blur(); }
    });
    label.append(input, element("small", "field-hint", field.hint || "")); fields.append(label);
  }
}

async function saveChatSettings(patch) {
  const selected = state.thread;
  if (!selected || selected.read_only || state.chatStreams.has(selected.id)) return;
  const id = selected.id;
  if (!threadSettingsSnapshots.has(id)) threadSettingsSnapshots.set(id, selected);
  const save = (threadSettingsSaves.get(id) || Promise.resolve()).catch(() => {}).then(async () => {
    const snapshot = threadSettingsSnapshots.get(id);
    byId("chat-settings-save-status").textContent = "Saving…";
    const response = await api(`${API_ROOT}/chat/threads/${encodeURIComponent(id)}/settings`, {
      method: "POST", body: { revision: snapshot.revision, ...patch },
    });
    const updated = { ...snapshot, ...response.thread };
    threadSettingsSnapshots.set(id, updated);
    if (state.thread?.id === id) {
      state.thread = { ...state.thread, ...response.thread }; applyThreadModelSettings(state.thread);
      byId("chat-settings-save-status").textContent = "Saved";
    }
    await loadThreads();
  });
  threadSettingsSaves.set(id, save);
  try { await save; }
  catch (error) {
    if (state.thread?.id === id) {
      byId("chat-settings-save-status").textContent = errorMessage(error);
      if (patch.provider !== undefined || patch.model !== undefined) applyThreadModelSettings(state.thread);
    }
    chatNotice(errorMessage(error), "error", id);
  } finally {
    if (threadSettingsSaves.get(id) === save) threadSettingsSaves.delete(id);
  }
}

async function refreshWorkspaceSettings() {
  state.workspaceSettings = await api(`${API_ROOT}/workspace/settings`);
  applyWorkspaceSettings();
}

function applyWorkspaceSettings() {
  const saved = state.workspaceSettings;
  if (!saved) return;
  byId("workspace-provider").value = saved.provider || "";
  byId("workspace-model").value = saved.model || "";
  refreshModelControl(modelControls().find((control) => control.providerId === "workspace-provider"));
  renderModelSettings("workspace");
}

async function saveWorkspaceSettings(patch, noticeTarget = activeNoticeTarget()) {
  if (workspaceSavePending || !state.workspaceSettings) return;
  workspaceSavePending = true; updateSettingsAvailability();
  byId("workspace-settings-save-status").textContent = "Saving…";
  let saved = false;
  try {
    state.workspaceSettings = await api(`${API_ROOT}/workspace/settings`, {
      method: "POST", body: { revision: state.workspaceSettings.revision, ...patch },
    });
    applyWorkspaceSettings();
    saved = true;
    if (state.session) await refreshSelectedSession();
    byId("workspace-settings-save-status").textContent = "Saved";
  } catch (error) {
    byId("workspace-settings-save-status").textContent = errorMessage(error);
    targetNotice(noticeTarget, errorMessage(error), "error");
    if (patch.provider !== undefined || patch.model !== undefined) applyWorkspaceSettings();
  } finally {
    workspaceSavePending = false; updateSettingsAvailability();
    if (workspaceModelPickerQueued) {
      const providerId = workspaceModelPickerQueued;
      workspaceModelPickerQueued = "";
      if (saved) {
        const control = modelControls().find((item) => item.providerId === providerId);
        window.setTimeout(() => openModelPicker(control, "model"), 0);
      }
    }
  }
}

function updateSettingsAvailability() {
  const chatDisabled = !state.connected || !state.thread || state.thread.read_only || chatTurnBusy();
  for (const id of ["chat-provider", "chat-model", "chat-reasoning"]) byId(id).disabled = chatDisabled;
  for (const input of document.querySelectorAll("#chat-settings-fields input, #chat-settings-fields select, [data-chat-setting]")) input.disabled = chatDisabled;
  const workspaceDisabled = !state.connected || workspaceSavePending || Boolean(state.session && state.session.status !== "ready");
  for (const id of ["workspace-provider", "workspace-model"]) byId(id).disabled = workspaceDisabled;
  for (const input of document.querySelectorAll("#workspace-settings-fields input, #workspace-settings-fields select")) input.disabled = workspaceDisabled;
  syncPickerButtons();
}

function renderMatchingModelControls(key) {
  for (const control of modelControls()) {
    if (modelCatalogKey(control) === key) renderModelControl(control);
  }
}

async function finishModelCatalog(key, catalog) {
  if (catalog.finishing || state.modelCatalogs.get(key) !== catalog) return;
  catalog.finishing = true;
  try {
    const snapshot = await api(`${API_ROOT}/jobs/${encodeURIComponent(catalog.jobId)}`);
    if (state.modelCatalogs.get(key) !== catalog) return;
    catalog.state = snapshot.state;
    catalog.done = TERMINAL_STATES.has(snapshot.state);
    if (snapshot.state === "succeeded" && snapshot.result && Array.isArray(snapshot.result.models)) {
      catalog.models = [...new Set(snapshot.result.models.filter((model) => typeof model === "string" && model))];
      const reasoningOptions = Array.isArray(snapshot.result.reasoning_options)
        ? snapshot.result.reasoning_options : [];
      catalog.reasoningOptions = new Map(reasoningOptions
        .filter((entry) => entry && typeof entry.model === "string" && Array.isArray(entry.options))
        .map((entry) => [entry.model, entry.options]));
      catalog.state = "ready";
    } else if (catalog.done) {
      catalog.state = "failed";
    } else {
      catalog.state = "loading";
    }
  } catch (_) {
    if (state.modelCatalogs.get(key) === catalog) catalog.state = "failed";
  } finally {
    catalog.finishing = false;
    catalog.done = catalog.state === "ready" || catalog.state === "failed";
    if (catalog.done) stopStream(`models:${catalog.jobId}`);
    renderMatchingModelControls(key);
  }
}

async function requestModelCatalog(key, control) {
  const catalog = { state: "loading", models: [], jobId: "", done: false, finishing: false };
  state.modelCatalogs.set(key, catalog);
  renderMatchingModelControls(key);
  try {
    const response = await api(`${API_ROOT}/jobs/models`, {
      method: "POST",
      body: optionalPayload({
        provider: byId(control.providerId).value,
      }),
    });
    if (state.modelCatalogs.get(key) !== catalog) {
      void api(`${API_ROOT}/jobs/${encodeURIComponent(response.job.id)}/cancel`, { method: "POST" }).catch(() => {});
      return;
    }
    catalog.jobId = response.job.id;
    startStream(`models:${catalog.jobId}`,
      `${API_ROOT}/jobs/${encodeURIComponent(catalog.jobId)}/events`,
      (event) => {
        if (event && TERMINAL_STATES.has(event.type)) {
          // The server closes a completed job's SSE stream immediately. Mark
          // it done before the follow-up snapshot fetch so the generic stream
          // watcher cannot mistake that clean EOF for a lost server.
          catalog.done = true;
          void finishModelCatalog(key, catalog);
        }
      },
      () => finishModelCatalog(key, catalog),
      () => catalog.done || state.modelCatalogs.get(key) !== catalog);
    if (TERMINAL_STATES.has(response.job.state)) {
      catalog.done = true;
      void finishModelCatalog(key, catalog);
    }
  } catch (_) {
    if (state.modelCatalogs.get(key) === catalog) {
      catalog.state = "failed";
      catalog.done = true;
      renderMatchingModelControls(key);
    }
  }
}

function cancelUnusedModelCatalogs() {
  const activeKeys = new Set(modelControls().map(modelCatalogKey));
  for (const [key, catalog] of state.modelCatalogs) {
    if (catalog.state !== "loading" || activeKeys.has(key)) continue;
    state.modelCatalogs.delete(key);
    catalog.done = true;
    if (catalog.jobId) {
      stopStream(`models:${catalog.jobId}`);
      void api(`${API_ROOT}/jobs/${encodeURIComponent(catalog.jobId)}/cancel`, { method: "POST" }).catch(() => {});
    }
  }
}

function refreshModelControl(control) {
  const key = modelCatalogKey(control);
  renderModelControl(control);
  if (supports("models") && !state.modelCatalogs.has(key)) void requestModelCatalog(key, control);
  cancelUnusedModelCatalogs();
}

function refreshModelControls() {
  for (const control of modelControls()) refreshModelControl(control);
}

function resetLoadingModelCatalogs() {
  for (const [key, catalog] of state.modelCatalogs) {
    if (catalog.state === "loading") state.modelCatalogs.delete(key);
  }
}

function populateProviders() {
  const providers = state.capabilities && Array.isArray(state.capabilities.providers)
    ? state.capabilities.providers : [];
  for (const select of document.querySelectorAll(".provider-select")) {
    const previous = select.value;
    clear(select);
    const automatic = element("option", "", "Server default");
    automatic.value = "";
    if (["goal-provider", "thread-provider"].includes(select.id)) select.append(automatic);
    for (const provider of providers) {
      const option = element("option", "", provider);
      option.value = provider;
      select.append(option);
    }
    if ([...select.options].some((option) => option.value === previous)) select.value = previous;
  }
}

function fillImageSelect(id, values, placeholder, previous = "") {
  const select = byId(id);
  clear(select);
  const automatic = element("option", "", placeholder);
  automatic.value = "";
  select.append(automatic);
  for (const value of values) {
    const option = element("option", "", value);
    option.value = value;
    select.append(option);
  }
  if ([...select.options].some((option) => option.value === previous)) select.value = previous;
}

function selectedImageModel() {
  if (!state.imageCatalog) return null;
  return selectImageModel(state.imageCatalog, byId("image-provider").value,
    byId("image-model").value);
}

function imageReferenceError(model) {
  if (!state.imageCatalog) return "Image catalog is unavailable.";
  const basic = imageFileError(state.imageInputs.map((input) => input.file), state.imageCatalog.limits);
  if (basic) return basic;
  if (state.imageInputs.length && (!model || !model.edits)) {
    return "The selected model does not support reference images.";
  }
  if (model && state.imageInputs.length > Number(model.max_input_images || 0)) {
    return `The selected model accepts at most ${model.max_input_images} reference images.`;
  }
  return "";
}

function renderImageInputList() {
  const list = byId("image-input-list");
  clear(list);
  if (!state.imageInputs.length) {
    list.classList.add("empty-state");
    list.textContent = "No reference images selected.";
    return;
  }
  for (const input of state.imageInputs) {
    const row = element("div", "image-input-row");
    const preview = element("img");
    preview.src = input.previewUrl;
    preview.alt = "";
    const details = element("span", "", `${input.file.name} · ${formatBytes(input.file.size)}`);
    const remove = element("button", "ghost", "Remove");
    remove.type = "button";
    remove.addEventListener("click", () => void removeImageInput(input));
    row.append(preview, details, remove);
    list.append(row);
  }
}

function renderImageOptions(resetModel = false) {
  const catalog = state.imageCatalog;
  const status = byId("image-options-status");
  if (!catalog) {
    status.textContent = "Connect to load image models from images.conf.";
    byId("image-generate-button").disabled = true;
    return;
  }
  const provider = byId("image-provider");
  const previousProvider = provider.value;
  fillImageSelect("image-provider", catalog.providers,
    `Server default (${catalog.default_provider || "provider"})`, previousProvider);
  if (previousProvider && catalog.providers.includes(previousProvider)) provider.value = previousProvider;
  const requestedModel = resetModel ? "" : byId("image-model").value;
  const effectiveProvider = provider.value || catalog.default_provider;
  const models = catalog.models.filter((item) => item.provider === effectiveProvider || item.provider === "any");
  fillImageSelect("image-model", models.map((item) => item.model), "Catalog default", requestedModel);
  const model = selectImageModel(catalog, provider.value, byId("image-model").value);
  if (model && !byId("image-model").value) byId("image-model").value = model.model;
  const previous = {
    size: byId("image-size").value,
    aspect: byId("image-aspect").value,
    quality: byId("image-quality").value,
    format: byId("image-format").value,
  };
  const sizes = model && Array.isArray(model.sizes) ? [...model.sizes] : [];
  if (model && model.custom_size && model.custom_size.enabled) sizes.push("Custom…");
  fillImageSelect("image-size", sizes, "Model default", previous.size);
  fillImageSelect("image-aspect", model && model.aspect_ratios || [], "Model default", previous.aspect);
  fillImageSelect("image-quality", model && model.qualities || [], "Model default", previous.quality);
  fillImageSelect("image-format", model && model.formats || [],
    model && model.format_default ? `Model default (${model.format_default})` : "Model default",
    previous.format);
  const custom = byId("image-size").value === "Custom…";
  byId("image-custom-size").hidden = !custom;
  const multiple = model && model.custom_size ? Number(model.custom_size.multiple) || 1 : 1;
  for (const id of ["image-width", "image-height"]) {
    byId(id).step = String(multiple);
    byId(id).min = String(multiple);
    if (model && model.custom_size && model.custom_size.max_edge) {
      byId(id).max = String(model.custom_size.max_edge);
    } else {
      byId(id).removeAttribute("max");
    }
  }
  const dimensionError = custom
    ? customDimensionError(model, byId("image-width").value, byId("image-height").value) : "";
  const referenceError = imageReferenceError(model);
  status.textContent = referenceError || dimensionError ||
    (model ? `${model.edits ? `Up to ${model.max_input_images} reference images` : "Text-to-image only"}.` :
      "No configured image model is available for this provider.");
  status.classList.toggle("error-text", Boolean(referenceError || dimensionError || !model));
  byId("image-input-files").disabled = !model || !model.edits;
  const activeJob = state.imageJobId ? state.jobs.get(state.imageJobId) : null;
  byId("image-generate-button").disabled = !supports("image") || !model ||
    state.imageSubmitting || Boolean(activeJob && !TERMINAL_STATES.has(activeJob.state)) ||
    Boolean(referenceError || dimensionError);
}

async function removeImageInput(input) {
  state.imageInputs = state.imageInputs.filter((item) => item !== input);
  URL.revokeObjectURL(input.previewUrl);
  if (input.uploadId && state.token) {
    await api(`${API_ROOT}/images/inputs/${encodeURIComponent(input.uploadId)}`, {
      method: "DELETE",
    }).catch(() => {});
  }
  renderImageInputList();
  renderImageOptions();
}

function releaseAllImageInputs() {
  const token = state.token;
  const csrfToken = state.csrfToken;
  for (const input of state.imageInputs) {
    URL.revokeObjectURL(input.previewUrl);
    if (input.uploadId && token && csrfToken) {
      void fetch(`${API_ROOT}/images/inputs/${encodeURIComponent(input.uploadId)}`, {
        method: "DELETE", headers: controlHeaders("DELETE", {}, token, csrfToken),
        credentials: "omit", cache: "no-store", referrerPolicy: "no-referrer",
      }).catch(() => {});
    }
  }
  state.imageInputs = [];
  renderImageInputList();
}

function resetImageForm() {
  const values = resetImageFormValues(byId("image-provider").value, byId("image-model").value);
  byId("image-provider").value = values.provider;
  byId("image-model").value = values.model;
  byId("image-prompt").value = values.prompt;
  byId("image-input-files").value = "";
  byId("image-size").value = values.size;
  byId("image-aspect").value = values.aspect;
  byId("image-quality").value = values.quality;
  byId("image-format").value = values.format;
  byId("image-width").value = values.width;
  byId("image-height").value = values.height;
  releaseAllImageInputs();
  renderImageOptions();
  byId("image-prompt").focus();
}

function selectedVideoModel() {
  if (!state.videoCatalog) return null;
  const provider = byId("video-provider").value || state.videoCatalog.default_provider;
  const requested = byId("video-model").value;
  return selectVideoModel(state.videoCatalog, provider, requested);
}

function renderVideoInputList() {
  const list = byId("video-input-list");
  clear(list);
  if (!state.videoInputs.length) {
    list.classList.add("empty-state");
    list.textContent = "No input media selected.";
    return;
  }
  for (const input of state.videoInputs) {
    const row = element("div", "video-input-row");
    const details = element("span", "",
      `${cropVideoFileName(input.file.name)} · ${formatBytes(input.file.size)}`);
    details.title = input.file.name;
    const remove = element("button", "ghost", "Remove");
    remove.type = "button";
    remove.addEventListener("click", () => void removeVideoInput(input));
    row.append(details, remove);
    list.append(row);
  }
}

function videoInputError(model) {
  return videoFileError(model, state.videoInputs, state.videoCatalog.limits);
}

function renderVideoOptions(resetModel = false) {
  const catalog = state.videoCatalog; const status = byId("video-options-status");
  if (!catalog) { status.textContent = "Connect to load video models from videos.conf."; byId("video-generate-button").disabled = true; return; }
  const provider = byId("video-provider"); const previousProvider = provider.value;
  fillImageSelect("video-provider", catalog.providers, `Server default (${catalog.default_provider || "fal"})`, previousProvider);
  const effectiveProvider = provider.value || catalog.default_provider;
  const models = catalog.models.filter((item) => item.provider === effectiveProvider || item.provider === "any");
  const priorModel = state.videoRenderedModel || "";
  const previousModel = resetModel ? "" : byId("video-model").value;
  const oldValues = new Map();
  if (!resetModel) for (const control of byId("video-settings").querySelectorAll("[data-video-setting]")) {
    oldValues.set(control.dataset.videoSetting, control.type === "checkbox" ? control.checked : control.value);
  }
  fillImageSelect("video-model", models.map((item) => item.model), "Catalog default", previousModel);
  const model = selectedVideoModel(); if (model && !byId("video-model").value) byId("video-model").value = model.model;
  const settings = byId("video-settings"); settings.replaceChildren();
  for (const descriptor of model && Array.isArray(model.settings) ? model.settings : []) {
    const label = element("label", "", descriptor.label || descriptor.name); let control;
    if (descriptor.type === "boolean") {
      control = document.createElement("input"); control.type = "checkbox"; control.checked = descriptor.default === true;
    } else if (descriptor.type === "enum") {
      control = document.createElement("select");
      for (const value of descriptor.options || []) { const option = element("option", "", value); option.value = value; control.append(option); }
      if (descriptor.default !== undefined) control.value = String(descriptor.default);
    } else {
      control = document.createElement("input"); control.type = descriptor.type === "string" ? "text" : "number";
      if (descriptor.min !== undefined) control.min = String(descriptor.min);
      if (descriptor.max !== undefined) control.max = String(descriptor.max);
      control.step = String(descriptor.step !== undefined ? descriptor.step : descriptor.type === "integer" ? 1 : "any");
      if (descriptor.default !== undefined) control.value = String(descriptor.default);
    }
    if (priorModel === (model && model.model) && oldValues.has(descriptor.name)) {
      if (control.type === "checkbox") control.checked = oldValues.get(descriptor.name) === true;
      else control.value = String(oldValues.get(descriptor.name));
    }
    control.dataset.videoSetting = descriptor.name; control.dataset.videoType = descriptor.type; label.append(control); settings.append(label);
  }
  state.videoRenderedModel = model ? model.model : "";
  const validation = videoInputError(model);
  status.textContent = validation || videoInputStatus(model, state.videoInputs);
  status.classList.toggle("error-text", Boolean(validation)); byId("video-input-files").disabled = !model || model.input_mode === "text";
  const active = state.videoJobId ? state.jobs.get(state.videoJobId) : null;
  byId("video-generate-button").disabled = !supports("video") || !model || state.videoSubmitting || Boolean(active && !TERMINAL_STATES.has(active.state)) || Boolean(validation);
}

async function removeVideoInput(input) {
  state.videoInputs = state.videoInputs.filter((item) => item !== input);
  if (input.uploadId && state.token) await api(`${API_ROOT}/videos/inputs/${encodeURIComponent(input.uploadId)}`, { method: "DELETE" }).catch(() => {});
  renderVideoInputList(); renderVideoOptions();
}

function releaseAllVideoInputs() {
  const token = state.token;
  const csrfToken = state.csrfToken;
  for (const input of state.videoInputs) if (input.uploadId && token && csrfToken) void fetch(`${API_ROOT}/videos/inputs/${encodeURIComponent(input.uploadId)}`, { method: "DELETE", headers: controlHeaders("DELETE", {}, token, csrfToken), credentials: "omit", cache: "no-store", referrerPolicy: "no-referrer" }).catch(() => {});
  state.videoInputs = []; renderVideoInputList();
}

async function uploadVideoInputs() {
  for (const input of state.videoInputs) if (!input.uploadId) {
    const stored = await api(`${API_ROOT}/videos/inputs`, { method: "POST", rawBody: input.file, contentType: input.file.type }); input.uploadId = stored.id;
  }
  return state.videoInputs.map((input) => input.uploadId);
}

async function uploadImageInputs() {
  for (const input of state.imageInputs) {
    if (input.uploadId) continue;
    const stored = await api(`${API_ROOT}/images/inputs`, {
      method: "POST", rawBody: input.file, contentType: input.file.type,
    });
    input.uploadId = stored.id;
    input.expiresAt = stored.expires_at;
  }
  return state.imageInputs.map((input) => input.uploadId);
}

function applyCapabilities() {
  populateProviders();
  refreshModelControls();
  byId("new-thread-button").disabled = !supports("chat_threads");
  byId("import-thread-button").disabled = !supports("chat_threads");
  byId("thread-search").disabled = !supports("chat_threads");
  syncChatSendButton();
  const chatAttachBlocked = !state.thread || state.thread.read_only === true || !supports("chat_inputs");
  byId("chat-attach-button").disabled = chatAttachBlocked;
  byId("chat-fetch-button").disabled = chatAttachBlocked;
  for (const control of byId("agent-panel").querySelectorAll("select, input, textarea, button")) {
    control.disabled = !supports("sessions");
  }
  byId("workspace-review-button").disabled = !supports("review");
  byId("refresh-directory-button").disabled = !supports("dired");
  byId("create-file-button").disabled = !supports("files");
  byId("create-directory-button").disabled = !supports("workspace_mutations");
  for (const control of byId("goal-job-form").elements) control.disabled = !supports("run") && !supports("plan");
  for (const control of byId("image-job-form").elements) control.disabled = !supports("image");
  for (const control of byId("video-job-form").elements) control.disabled = !supports("video");
  renderImageOptions();
  renderVideoOptions();
  updateSettingsAvailability();
}

function renderSettings() {
  const status = state.status || {};
  setDetails(byId("status-details"), [
    ["API version", status.api_version],
    ["State", status.status],
    ["Transport", status.bind && status.bind.transport],
    ["Listener", status.bind ? `${status.bind.address}:${status.bind.port}` : "—"],
    ["Connections", status.connections ? `${status.connections.active} / ${status.connections.maximum}` : "—"],
    ["Jobs", status.jobs ? `${status.jobs.active} active, ${status.jobs.retained} retained / ${status.jobs.maximum}` : "—"],
    ["Sessions", status.sessions ? `${status.sessions.active} / ${status.sessions.maximum}` : "—"],
  ]);
  const caps = state.capabilities || {};
  setDetails(byId("capability-details"), [
    ["Operations", caps.operations],
    ["Providers", caps.providers],
    ["MCP adapter", caps.adapters && caps.adapters.mcp ? "Enabled" : "Unavailable"],
    ["Embedded UI", caps.adapters && caps.adapters.web_ui ? "Enabled" : "Compatibility mode"],
    ["OpenAI /v1 adapter", caps.adapters && caps.adapters.openai_v1 ? "Enabled" : "Not implemented"],
    ["MCP credential", caps.authentication && caps.authentication.mcp_configured ? "Configured" : "Not configured"],
  ]);
}

async function refreshSettings() {
  const [capabilities, status, imageCatalog, videoCatalog, workspaceSettings] = await Promise.all([
    api(`${API_ROOT}/capabilities`),
    api(`${API_ROOT}/status`),
    api(`${API_ROOT}/images/catalog`),
    api(`${API_ROOT}/videos/catalog`),
    api(`${API_ROOT}/workspace/settings`),
  ]);
  state.capabilities = capabilities;
  state.status = status;
  state.imageCatalog = normalizeImageCatalog(imageCatalog);
  state.videoCatalog = normalizeVideoCatalog(videoCatalog);
  state.workspaceSettings = workspaceSettings;
  renderSettings();
  applyCapabilities();
  applyWorkspaceSettings();
}

function setConnectionStatus(label, className) {
  const badge = byId("connection-badge");
  badge.className = `connection-dot ${className}`;
  badge.title = label;
  badge.setAttribute("aria-label", label);
  const text = badge.querySelector(".sr-only");
  if (text) text.textContent = label;
}

function cancelReconnect() {
  if (state.reconnectTimer !== null) window.clearTimeout(state.reconnectTimer);
  state.reconnectTimer = null;
}

function scheduleReconnect(immediate = false) {
  if (!state.authenticated || state.connected) return;
  cancelReconnect();
  const exponent = Math.min(state.reconnectAttempt, 5);
  const delay = immediate ? 0 : Math.min(10000, 500 * (2 ** exponent));
  state.reconnectAttempt += 1;
  state.reconnectTimer = window.setTimeout(() => void attemptReconnect(), delay);
}

function markConnectionLost() {
  if (!state.authenticated) return;
  const noticeTarget = activeNoticeTarget();
  const wasConnected = state.connected;
  state.connected = false;
  if (wasConnected) {
    stopAllStreams();
    resetLoadingModelCatalogs();
    endChatTurn(null, true);
  }
  setConnectionStatus("Reconnecting…", "reconnecting");
  byId("disconnect-button").hidden = false;
  applyCapabilities();
  if (wasConnected) targetNotice(noticeTarget,
    "Connection lost. Ainiux will reconnect automatically.", "error");
  if (state.reconnectTimer === null) scheduleReconnect();
}

async function restoreBrowserState() {
  const threadId = state.thread && state.thread.id;
  const sessionId = state.session && state.session.id;
  const directoryPath = state.directory.path || ".";
  const tasks = [];
  if (supports("chat_threads")) {
    tasks.push((async () => {
      await loadThreads();
      await cleanupEmptyThreads(threadId || 0);
      if (threadId) await loadThread(threadId);
      else if (!state.chatInitialized) await startNewChat();
    })());
  }
  if (supports("sessions")) {
    tasks.push((async () => {
      await loadSessions();
      if (sessionId && state.sessions.some((item) => item.id === sessionId)) {
        await selectSession(sessionId);
      } else {
        state.session = null;
        state.agentHistory.clear(); state.agentLogs.clear(); state.agentActivities.clear(); state.agentSeenEvents.clear();
        renderAgent();
        if (activePanelId() === "agent-panel") await ensureWorkspaceAgent();
      }
    })());
  }
  if (supports("dired")) tasks.push(loadDirectory(directoryPath));
  if (supports("review")) tasks.push(loadWorkspaceReview());
  tasks.push((async () => {
    await refreshKnownJobs();
    for (const job of state.jobs.values()) {
      if (!TERMINAL_STATES.has(job.state)) watchJob(job.id);
    }
  })());
  await Promise.allSettled(tasks);
}

async function markConnected(reconnected) {
  cancelReconnect();
  state.authenticated = true;
  state.connected = true;
  state.reconnectAttempt = 0;
  storageSet(TOKEN_STORAGE_KEY, state.token);
  byId("token-input").value = "";
  setConnectionStatus("Connected", "online");
  byId("disconnect-button").hidden = false;
  closeDialog(byId("auth-dialog"));
  applyCapabilities();
  if (reconnected) clearTransientNotices();
  await restoreBrowserState();
  state.chatInitialized = true;
  if (reconnected) {
    targetNotice(activeNoticeTarget(), "Reconnected to the Ainiux control server");
  }
}

async function attemptReconnect() {
  state.reconnectTimer = null;
  if (!state.authenticated || state.connected || !state.token) return;
  setConnectionStatus("Reconnecting…", "reconnecting");
  try {
    state.csrfToken = "";
    await refreshCsrfToken();
    await refreshSettings();
    await markConnected(true);
  } catch (error) {
    if (!state.authenticated || (error instanceof ApiError && error.status === 401)) return;
    if (state.reconnectTimer === null) scheduleReconnect();
  }
}

async function connect(token, previouslyValidated = false) {
  const cleaned = token.trim();
  if (!cleaned) throw new ApiError(401, "missing_token", "Enter a controller token");
  state.token = cleaned;
  state.csrfToken = "";
  state.authenticated = previouslyValidated;
  try {
    await refreshCsrfToken();
    await refreshSettings();
  } catch (error) {
    if (previouslyValidated && !(error instanceof ApiError && error.status === 401)) {
      state.authenticated = true;
      byId("disconnect-button").hidden = false;
      closeDialog(byId("auth-dialog"));
      markConnectionLost();
      return false;
    }
    state.token = "";
    state.csrfToken = "";
    state.authenticated = false;
    throw error;
  }
  await markConnected(false);
  return true;
}

function forgetAuthentication(message = "") {
  picker.close();
  state.workspaceSettings = null;
  state.agentHistory.clear();
  threadSettingsSnapshots.clear();
  cancelReconnect();
  stopAllStreams();
  stopAgentClock();
  releaseAllImageInputs();
  releaseAllVideoInputs();
  releaseChatInputs();
  if (state.videoObjectUrl) URL.revokeObjectURL(state.videoObjectUrl);
  state.videoObjectUrl = "";
  state.token = "";
  state.csrfToken = "";
  state.authenticated = false;
  state.connected = false;
  state.reconnectAttempt = 0;
  state.capabilities = null;
  state.status = null;
  state.imageCatalog = null;
  state.videoCatalog = null;
  state.thread = null;
  state.chatInitialized = false;
  state.startingNewChat = false;
  state.chatEdit = null;
  endChatTurn(null, true);
  state.chatMetrics.clear();
  state.session = null;
  state.modelCatalogs.clear();
  state.file = null;
  state.guard = null;
  clearTransientNotices();
  storageSet(TOKEN_STORAGE_KEY, "");
  byId("token-input").value = "";
  setConnectionStatus("Offline", "offline");
  byId("disconnect-button").hidden = true;
  setEmpty(byId("thread-list"), "Connect to load threads.");
  setEmpty(byId("chat-messages"), "Choose or create a thread.");
  setEmpty(byId("agent-events"), "Open Agent to initialize this workspace.");
  setEmpty(byId("directory-list"), "Connect to browse the workspace.");
  clearEditor();
  byId("chat-input").disabled = true;
  byId("agent-turn-input").disabled = true;
  closeDialog(byId("guard-dialog"));
  byId("auth-error").textContent = message;
  openDialog(byId("auth-dialog"));
  renderImageOptions();
  renderVideoOptions();
}

function invalidateAuthentication() {
  forgetAuthentication("Invalid authentication");
}

function switchPanel(panelId) {
  if (panelId === "settings-panel") {
    const previous = activePanelId();
    if (previous === "chat-panel") modelSettingsScope = "chat";
    else if (previous === "agent-panel" || previous === "workspace-panel") modelSettingsScope = "workspace";
  }
  for (const panel of document.querySelectorAll("main > .panel")) {
    const active = panel.id === panelId;
    panel.hidden = !active;
    panel.classList.toggle("active", active);
  }
  for (const button of document.querySelectorAll(".primary-nav button")) {
    if (button.dataset.panel === panelId) button.setAttribute("aria-current", "page");
    else button.removeAttribute("aria-current");
  }
  byId("main").focus({ preventScroll: true });
  if (panelId === "agent-panel" && state.connected) void ensureWorkspaceAgent();
  if (panelId === "image-panel") renderImage();
  if (panelId === "video-panel") renderVideo();
}

function jobLabel(job) {
  return `${job.operation || "job"} · ${job.id}`;
}

function renderJobResult(container, job) {
  const result = job.result && typeof job.result === "object" ? job.result : {};
  if (job.state === "failed" && job.error) {
    container.append(element("pre", "danger-text", job.error.message || "Job failed"));
    return;
  }
  if (!TERMINAL_STATES.has(job.state) && job._streamText) {
    container.append(element("pre", "streaming-output", job._streamText));
    return;
  }
  if (job.operation === "image" && result.data_base64) {
    const format = String(result.format || "png").toLowerCase();
    const mime = format === "jpg" || format === "jpeg" ? "image/jpeg" :
      format === "webp" ? "image/webp" : "image/png";
    const image = element("img");
    image.alt = "Image generated by Ainiux";
    image.loading = "lazy";
    image.src = `data:${mime};base64,${result.data_base64}`;
    container.append(image, element("small", "muted", `${result.model || "model"} · ${result.size || "generated size"}`));
    return;
  }
  if (typeof result.content === "string") {
    container.append(element("pre", "", result.content));
    appendMetrics(container, result.metrics);
    return;
  }
  if (job.operation === "editor-assist" && result.edit) {
    container.append(element("pre", "", result.edit.replacement || "Empty replacement"));
    appendMetrics(container, result.metrics);
    return;
  }
  if (TERMINAL_STATES.has(job.state) && Object.keys(result).length) {
    const safeResult = { ...result };
    delete safeResult.data_base64;
    container.append(element("pre", "", JSON.stringify(safeResult, null, 2)));
  }
}

function imageMimeType(format) {
  const normalized = String(format || "png").toLowerCase();
  if (normalized === "jpg" || normalized === "jpeg") return "image/jpeg";
  if (normalized === "webp") return "image/webp";
  return "image/png";
}

function renderImage() {
  const output = byId("image-output");
  const cancel = byId("image-cancel-button");
  const download = byId("image-download-button");
  const job = state.imageJobId ? state.jobs.get(state.imageJobId) : null;
  cancel.hidden = !job || TERMINAL_STATES.has(job.state);
  download.hidden = !(state.imageResult && state.imageResult.data_base64);
  if (job && !TERMINAL_STATES.has(job.state)) {
    clear(output);
    output.classList.add("image-progress");
    const latest = Array.isArray(job._events) ? job._events[job._events.length - 1] : null;
    output.append(element("div", "spinner", ""),
      element("strong", "", job.state === "queued" ? "Queued" : "Generating…"),
      element("p", "muted", latest && latest.data && latest.data.text
        ? latest.data.text : "Waiting for the image provider."));
    return;
  }
  output.classList.remove("image-progress");
  if (state.imageError) {
    setEmpty(output, state.imageError);
    output.classList.add("danger-text");
    return;
  }
  output.classList.remove("danger-text");
  const result = state.imageResult;
  if (!result || !result.data_base64) {
    setEmpty(output, "Your generated image will appear here.");
    return;
  }
  clear(output);
  const image = element("img");
  image.alt = "Image generated by Ainiux";
  image.src = `data:${imageMimeType(result.format)};base64,${result.data_base64}`;
  const details = [result.model || "model", result.size || "generated size"];
  if (result.server_path) details.push(`saved as ${result.server_path}`);
  if (Number.isFinite(Number(result.total_ms)) && Number(result.total_ms) >= 0) {
    details.push(formatDuration(result.total_ms));
  }
  output.append(image, element("p", "image-meta", details.join(" · ")));
}

function downloadGeneratedImage() {
  const result = state.imageResult;
  if (!result || !result.data_base64) return;
  try {
    const binary = window.atob(result.data_base64);
    const bytes = new Uint8Array(binary.length);
    for (let index = 0; index < binary.length; ++index) bytes[index] = binary.charCodeAt(index);
    const url = URL.createObjectURL(new Blob([bytes], { type: imageMimeType(result.format) }));
    const link = element("a");
    link.href = url;
    link.download = result.server_path || `image.${String(result.format || "png").toLowerCase()}`;
    document.body.append(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 0);
  } catch (error) {
    surfaceNotice("image-panel", `Could not prepare the local image copy: ${errorMessage(error)}`, "error");
  }
}

async function loadVideoArtifact(jobId) {
  const response = await fetch(`${API_ROOT}/jobs/${encodeURIComponent(jobId)}/artifact`, {
    headers: controlHeaders("GET", { Accept: "video/mp4" }),
    credentials: "omit", cache: "no-store", referrerPolicy: "no-referrer",
  });
  if (!response.ok) {
    if (response.status === 401) invalidateAuthentication();
    throw new ApiError(response.status, "artifact_download_failed", "Could not load the generated video artifact");
  }
  const blob = await response.blob();
  if (state.videoObjectUrl) URL.revokeObjectURL(state.videoObjectUrl);
  state.videoObjectUrl = URL.createObjectURL(blob);
}

function renderVideo() {
  const output = byId("video-output"); const cancel = byId("video-cancel-button");
  const download = byId("video-download-button"); const job = state.videoJobId ? state.jobs.get(state.videoJobId) : null;
  cancel.hidden = !job || TERMINAL_STATES.has(job.state); download.hidden = !state.videoObjectUrl;
  if (job && !TERMINAL_STATES.has(job.state)) {
    clear(output); const latest = Array.isArray(job._events) ? job._events[job._events.length - 1] : null;
    output.append(element("div", "spinner", ""), element("strong", "", job.state === "queued" ? "Queued" : "Generating…"), element("p", "muted", latest && latest.data && latest.data.text ? latest.data.text : "Waiting for the video provider.")); return;
  }
  if (state.videoError) { setEmpty(output, state.videoError); output.classList.add("danger-text"); return; }
  output.classList.remove("danger-text");
  if (!state.videoResult || !state.videoObjectUrl) { setEmpty(output, state.videoResult ? "Loading generated video…" : "Your generated video will appear here."); return; }
  clear(output); const video = document.createElement("video"); video.controls = true; video.preload = "metadata"; video.src = state.videoObjectUrl;
  const details = [state.videoResult.model || "model"];
  if (state.videoResult.server_path) details.push(`saved as ${state.videoResult.server_path}`);
  if (state.videoResult.byte_size) details.push(formatBytes(state.videoResult.byte_size));
  output.append(video, element("p", "image-meta", details.join(" · ")));
}

function downloadGeneratedVideo() {
  if (!state.videoObjectUrl || !state.videoResult) return;
  const link = element("a"); link.href = state.videoObjectUrl; link.download = state.videoResult.server_path || "video.mp4";
  document.body.append(link); link.click(); link.remove();
}

function renderJobs() {
  const list = byId("job-list");
  const jobs = [...state.jobs.values()]
    .filter((job) => job.operation === "run" || job.operation === "plan").reverse();
  if (jobs.length === 0) {
    setEmpty(list, "No run or plan jobs.");
    return;
  }
  clear(list);
  for (const job of jobs) {
    const card = element("article", "job-card");
    const header = element("header");
    header.append(element("h4", "", jobLabel(job)), element("span", `job-state ${job.state}`, job.state));
    card.append(header);
    const latest = Array.isArray(job._events) ? job._events[job._events.length - 1] : null;
    if (latest && latest.data && latest.data.text) card.append(element("p", "muted", latest.data.text));
    renderJobResult(card, job);
    if (!TERMINAL_STATES.has(job.state)) {
      const cancel = element("button", "danger", "Cancel");
      cancel.type = "button";
      cancel.addEventListener("click", () => void cancelJob(job.id));
      card.append(cancel);
    }
    list.append(card);
  }
}

function updateJob(snapshot, context) {
  const previous = state.jobs.get(snapshot.id) || {};
  const merged = {
    ...previous,
    ...snapshot,
    _events: previous._events || [],
    _context: context || previous._context || null,
    _handled: previous._handled || false,
    _streamText: previous._streamText || "",
  };
  state.jobs.set(merged.id, merged);
  renderJobs();
  if (merged.operation === "image") renderImage();
  if (merged.operation === "video") renderVideo();
  return merged;
}

async function submitJob(operation, payload, context = null, signal) {
  const idempotency = typeof crypto.randomUUID === "function"
    ? crypto.randomUUID() : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const response = await api(`${API_ROOT}/jobs/${operation}`, {
    method: "POST",
    headers: { "Idempotency-Key": `wui-${idempotency}` },
    body: payload,
    signal,
  });
  if (context) context.jobId = response.job.id;
  const job = updateJob(response.job, context);
  watchJob(job.id);
  return job;
}

function watchJob(jobId) {
  const existing = state.jobs.get(jobId);
  if (existing && TERMINAL_STATES.has(existing.state)) {
    void refreshJob(jobId);
    return;
  }
  const key = `job:${jobId}`;
  startStream(key, `${API_ROOT}/jobs/${encodeURIComponent(jobId)}/events`,
    (event) => {
      const job = state.jobs.get(jobId);
      if (!job) return;
      if (event && event.id && job._events.some((entry) => entry.id === event.id)) return;
      job._events.push(event);
      if (job._events.length > 24) job._events.shift();
      if (event && event.type === "delta" && event.data && typeof event.data.text === "string") {
        job._streamText += event.data.text;
        if (job._context && job._context.type === "chat") {
          job._context.streamText += event.data.text;
          updateVisibleChatStream(job._context);
        }
      }
      if (event && TERMINAL_STATES.has(event.type)) job.state = event.type;
      if (job.operation === "image") renderImage();
      else if (job.operation === "video") renderVideo();
      else if (activePanelId() === "jobs-panel" || !event || event.type !== "delta") renderJobs();
      if (event && TERMINAL_STATES.has(event.type)) void refreshJob(jobId);
    },
    async () => {
      await refreshJob(jobId);
      jobNotice(state.jobs.get(jobId),
        `Event replay expired for ${jobId}; loaded its current snapshot.`, "warning");
    },
    () => {
      const job = state.jobs.get(jobId);
      return !job || TERMINAL_STATES.has(job.state);
    });
}

async function refreshJob(jobId) {
  try {
    const snapshot = await api(`${API_ROOT}/jobs/${encodeURIComponent(jobId)}`);
    const job = updateJob(snapshot);
    if (TERMINAL_STATES.has(job.state)) {
      stopStream(`job:${jobId}`);
      await handleJobCompletion(job);
    }
  } catch (error) {
    jobNotice(state.jobs.get(jobId), errorMessage(error), "error");
  }
}

async function handleJobCompletion(job) {
  if (job._handled) return;
  job._handled = true;
  if (job._context && job._context.type === "chat") await finishChatJob(job, job._context);
  if (job._context && job._context.type === "assist") finishAssistJob(job, job._context);
  if (job.operation === "image") {
    state.imageJobId = "";
    state.imageResult = job.state === "succeeded" ? job.result : null;
    state.imageError = job.state === "failed" && job.error
      ? (job.error.message || "Image generation failed")
      : job.state === "cancelled" ? "Image generation cancelled." : "";
    renderImage();
    renderImageOptions();
  }
  if (job.operation === "video") {
    state.videoJobId = "";
    state.videoResult = job.state === "succeeded" ? job.result : null;
    state.videoError = job.state === "failed" && job.error ? (job.error.message || "Video generation failed") : job.state === "cancelled" ? "Video generation cancelled." : "";
    if (job.state === "succeeded") {
      try { await loadVideoArtifact(job.id); }
      catch (error) { state.videoError = errorMessage(error); }
    }
    renderVideo(); renderVideoOptions();
  }
}

async function cancelJob(jobId) {
  const existing = state.jobs.get(jobId);
  try {
    const snapshot = await api(`${API_ROOT}/jobs/${encodeURIComponent(jobId)}/cancel`, { method: "POST" });
    updateJob(snapshot);
  } catch (error) {
    jobNotice(existing, errorMessage(error), "error");
  }
}

async function refreshKnownJobs() {
  await Promise.allSettled([...state.jobs.keys()].map((id) => refreshJob(id)));
}

function threadSearchQuery() {
  const field = byId("thread-search");
  return field ? field.value.trim() : "";
}

function renderThreadSearchStatus(truncated) {
  const status = byId("thread-search-status");
  const query = threadSearchQuery();
  if (query && truncated) {
    status.hidden = false;
    status.textContent = "Showing the 200 newest matches.";
  } else {
    status.hidden = true;
    status.textContent = "";
  }
}

function renderThreads() {
  const list = byId("thread-list");
  if (!state.threads.length) {
    const query = threadSearchQuery();
    setEmpty(list, query ? `No threads match "${query}".` : "No saved threads.");
    return;
  }
  clear(list);
  for (const thread of state.threads) {
    const selected = state.thread && state.thread.id === thread.id;
    const item = element("div", `thread-item ${selected ? "selected" : ""}`);
    const button = element("button", `list-button ${selected ? "selected" : ""}`);
    button.type = "button";
    const details = [formatDate(thread.modified_at), `${thread.message_count || 0} messages`];
    if (thread.provider || thread.model) details.push(`${thread.provider || "provider"} / ${thread.model || "default"}`);
    button.append(element("strong", "", thread.name || "New chat"),
      element("small", "", details.join(" · ")));
    button.addEventListener("click", () => void loadThread(thread.id));
    item.append(button);
    const actions = element("div", "thread-actions");
    if (chatExportAvailable()) {
      const exportButton = element("button", "thread-export", "Export");
      exportButton.type = "button";
      exportButton.addEventListener("click", (event) => {
        event.stopPropagation();
        openChatExport(thread, "thread");
      });
      actions.append(exportButton);
    }
    if (thread.read_only !== true) {
      const remove = element("button", "thread-delete", "Delete");
      remove.type = "button";
      remove.addEventListener("click", (event) => {
        event.stopPropagation();
        void deleteThread(thread);
      });
      actions.append(remove);
    }
    if (actions.childElementCount) item.append(actions);
    list.append(item);
  }
}

function appendAttachmentChips(parent, attachments, removable = false) {
  if (!Array.isArray(attachments) || !attachments.length) return;
  const row = element("div", "message-attachments");
  for (const attachment of attachments) {
    const chip = element("span", "chat-attach-chip");
    const label = [attachment.display_name || attachment.kind || "attachment"];
    if (attachment.byte_size) label.push(formatBytes(attachment.byte_size));
    chip.append(element("span", "", label.join(" · ")));
    if (removable) {
      const remove = element("button", "ghost", "Remove");
      remove.type = "button";
      remove.addEventListener("click", () => void removeChatInput(attachment));
      chip.append(remove);
    }
    row.append(chip);
  }
  parent.append(row);
}

function appendChatMessage(container, role, content, streaming = false, ordinal = null, attachments = [], printActions = false) {
  const card = element("article", `message ${role || "system"}${streaming ? " streaming" : ""}`);
  if (streaming) card.id = "chat-stream-message";
  const output = element("div", "message-content");
  const editing = !streaming && role === "assistant" && state.chatEdit &&
    state.chatEdit.ordinal === ordinal;
  if (editing) {
    const editor = element("div", "message-edit");
    const textarea = element("textarea");
    textarea.rows = 6;
    textarea.value = state.chatEdit.draft;
    textarea.addEventListener("input", () => {
      if (state.chatEdit && state.chatEdit.ordinal === ordinal) state.chatEdit.draft = textarea.value;
    });
    const actions = element("div", "message-actions");
    const save = element("button", "", "Save");
    save.type = "button";
    save.addEventListener("click", () => void saveChatMessageEdit(ordinal, textarea.value));
    const cancel = element("button", "ghost", "Cancel");
    cancel.type = "button";
    cancel.addEventListener("click", () => cancelChatMessageEdit());
    actions.append(save, cancel);
    editor.append(textarea, actions);
    card.append(element("div", "role", "assistant · editing"), editor);
    queueMicrotask(() => textarea.focus());
  } else {
    renderChatContent(output, role, content, streaming);
    appendAttachmentChips(output, attachments);
    card.append(element("div", "role", streaming ? "assistant · streaming" : role || "message"), output);
    const actions = element("div", "message-actions");
    if (printActions && chatExportAvailable()) {
      const exportButton = element("button", "", "Export");
      exportButton.type = "button";
      exportButton.addEventListener("click", () => openChatExport(state.thread, "last"));
      actions.append(exportButton);
    }
    const writable = state.thread && state.thread.read_only !== true && !chatTurnBusy();
    if (!streaming && (role === "assistant" || role === "user") && writable && ordinal != null) {
      if (role === "assistant") {
        const edit = element("button", "", "Edit");
        edit.type = "button";
        edit.addEventListener("click", () => startChatMessageEdit(ordinal, content));
        actions.append(edit);
      }
      const remove = element("button", "delete", "Delete");
      remove.type = "button";
      remove.addEventListener("click", () => void deleteChatMessage(ordinal));
      actions.append(remove);
    }
    if (actions.childElementCount) card.append(actions);
  }
  container.append(card);
}

function updateVisibleChatStream(context) {
  if (!state.thread || state.thread.id !== context.threadId) return;
  pendingChatStream = context;
  if (chatRenderFrame !== null) return;
  chatRenderFrame = window.requestAnimationFrame(() => {
    chatRenderFrame = null;
    const pending = pendingChatStream;
    pendingChatStream = null;
    if (!pending || !state.thread || state.thread.id !== pending.threadId) return;
    const card = byId("chat-stream-message");
    if (!card) {
      renderChat();
      return;
    }
    const output = card.querySelector(".message-content");
    if (output) renderChatContent(output, "assistant", pending.streamText, true);
    const messages = byId("chat-messages");
    messages.scrollTop = messages.scrollHeight;
  });
}

function renderChat() {
  updateSettingsAvailability();
  const messages = byId("chat-messages");
  if (!state.thread) {
    byId("conversation-heading").textContent = state.startingNewChat
      ? "Starting new chat…" : "Select a thread";
    byId("thread-meta").textContent = "";
    byId("chat-metrics").textContent = "";
    if (state.chatUnscopedNotices.length) {
      clear(messages);
      for (const notice of state.chatUnscopedNotices) {
        appendConversationNotice(messages, notice, "message");
      }
      messages.scrollTop = messages.scrollHeight;
    } else {
      setEmpty(messages, state.startingNewChat ? "Starting new chat…" : "Choose or create a thread.");
    }
    byId("chat-input").disabled = true;
    byId("chat-fetch-button").disabled = true;
    byId("chat-attach-button").disabled = true;
    syncChatSendButton();
    renderChatAttachList();
    renderChatToolbar();
    return;
  }
  byId("conversation-heading").textContent = state.thread.name || "New chat";
  byId("thread-meta").textContent = `${formatDate(state.thread.modified_at)} · ${state.thread.message_count || state.thread.messages.length} messages`;
  byId("chat-metrics").textContent = metricText(state.chatMetrics.get(state.thread.id));
  const transcript = Array.isArray(state.thread.messages) ? state.thread.messages : [];
  const stream = state.chatStreams.get(state.thread.id);
  const notices = state.chatNotices.get(state.thread.id) || [];
  if (!transcript.length && !stream && !notices.length) setEmpty(messages, "This thread is empty.");
  else {
    clear(messages);
    appendChatTimeline(messages, transcript, notices, !stream);
    if (stream) appendChatMessage(messages, "assistant", stream.streamText, true);
    messages.scrollTop = messages.scrollHeight;
  }
  const readOnly = state.thread.read_only === true;
  if (readOnly) byId("thread-meta").textContent += " · read-only";
  byId("chat-input").disabled = readOnly;
  const attachBlocked = readOnly || !supports("chat_inputs");
  byId("chat-attach-button").disabled = attachBlocked;
  byId("chat-fetch-button").disabled = attachBlocked;
  syncChatSendButton();
  renderChatAttachList();
  renderChatToolbar();
  renderThreads();
}

let threadListSequence = 0;
let threadSearchTimer = null;

async function loadThreads() {
  const query = threadSearchQuery();
  const sequence = ++threadListSequence;
  const path = query
    ? `${API_ROOT}/chat/threads?q=${encodeURIComponent(query)}`
    : `${API_ROOT}/chat/threads`;
  try {
    const response = await api(path);
    if (sequence !== threadListSequence) return;
    state.threads = Array.isArray(response.threads) ? response.threads : [];
    renderThreadSearchStatus(response.truncated === true);
    renderThreads();
  } catch (error) {
    if (sequence !== threadListSequence) return;
    renderThreadSearchStatus(false);
    setEmpty(byId("thread-list"), errorMessage(error));
  }
}

function scheduleThreadSearch() {
  if (threadSearchTimer !== null) window.clearTimeout(threadSearchTimer);
  threadSearchTimer = window.setTimeout(() => {
    threadSearchTimer = null;
    void loadThreads();
  }, 200);
}

async function loadThread(threadId) {
  const previous = state.thread;
  const sequence = ++threadLoadSequence;
  try {
    await threadSettingsSaves.get(threadId);
    const response = await api(`${API_ROOT}/chat/threads/${encodeURIComponent(threadId)}`);
    if (sequence !== threadLoadSequence) return;
    state.thread = response.thread;
    if (!previous || previous.id !== threadId) {
      state.chatEdit = null;
      releaseChatInputs();
    }
    threadSettingsSnapshots.set(threadId, state.thread);
    applyThreadModelSettings(state.thread);
    renderChat();
    if (previous && previous.id !== threadId) void abandonUnusedThread(previous);
  } catch (error) {
    chatNotice(errorMessage(error), "error", threadId);
  }
}

async function cleanupEmptyThreads(keepId = 0, noticeThreadId = state.thread?.id ?? null) {
  try {
    await api(`${API_ROOT}/chat/threads/cleanup-empty`, {
      method: "POST",
      body: keepId ? { keep_id: keepId } : {},
    });
    await loadThreads();
  } catch (error) {
    if (!(error instanceof ApiError) || ![404, 409].includes(error.status)) {
      chatNotice(`Could not clean up unused chats: ${errorMessage(error)}`, "error", noticeThreadId);
    }
  }
}

function threadHasConversation(thread) {
  if (!thread) return false;
  if (state.thread && state.thread.id === thread.id && Array.isArray(state.thread.messages)) {
    return state.thread.messages.some((message) => message.role === "user" || message.role === "assistant");
  }
  return (thread.message_count || 0) > 0;
}

async function deleteThread(thread) {
  if (!thread || thread.read_only === true) return;
  if (chatTurnBusy(thread.id)) {
    chatNotice("Wait for the current response to finish before deleting this chat", "error", thread.id);
    return;
  }
  if (threadHasConversation(thread) &&
      !await askConfirm({
        title: "Delete chat?",
        message: "Delete this chat thread? This cannot be undone.",
        confirmLabel: "Delete",
        danger: true,
      })) return;
  try {
    await threadSettingsSaves.get(thread.id);
    const snapshot = threadSettingsSnapshots.get(thread.id) || thread;
    await api(`${API_ROOT}/chat/threads/${encodeURIComponent(thread.id)}`, {
      method: "DELETE",
      body: { revision: snapshot.revision },
    });
    const wasCurrent = state.thread && state.thread.id === thread.id;
    if (wasCurrent) {
      state.thread = null;
      state.chatEdit = null;
      renderChat();
    }
    threadSettingsSnapshots.delete(thread.id);
    state.chatNotices.delete(thread.id);
    await loadThreads();
    if (!wasCurrent) return;
    if (state.threads.length) await loadThread(state.threads[0].id);
    else await startNewChat();
  } catch (error) {
    if (error instanceof ApiError && error.code === "revision_conflict") {
      chatNotice("The chat thread changed in another client. Reload it before deleting.", "error", thread.id);
      await loadThreads();
      if (state.thread && state.thread.id === thread.id) await loadThread(thread.id);
    } else chatNotice(errorMessage(error), "error", thread.id);
  }
}

function startChatMessageEdit(ordinal, content) {
  state.chatEdit = { ordinal, draft: content == null ? "" : String(content) };
  renderChat();
}

function cancelChatMessageEdit() {
  state.chatEdit = null;
  renderChat();
}

async function saveChatMessageEdit(ordinal, content) {
  if (!state.thread || state.thread.read_only === true) return;
  const threadId = state.thread.id;
  try {
    await threadSettingsSaves.get(threadId);
    if (state.thread?.id !== threadId) return;
    const response = await api(
      `${API_ROOT}/chat/threads/${encodeURIComponent(threadId)}/edit-message`, {
        method: "POST",
        body: { revision: state.thread.revision, ordinal, content },
      });
    state.chatEdit = null;
    if (response.thread && state.thread?.id === threadId) {
      state.thread = response.thread;
      threadSettingsSnapshots.set(threadId, state.thread);
      applyThreadModelSettings(state.thread);
      renderChat();
      await loadThreads();
    } else await loadThread(threadId);
  } catch (error) {
    if (error instanceof ApiError && error.code === "revision_conflict") {
      showConflict("The chat thread changed in another client. Reload it before editing again.",
        () => loadThread(threadId));
    } else chatNotice(errorMessage(error), "error", threadId);
  }
}

async function deleteChatMessage(ordinal) {
  if (!state.thread || state.thread.read_only === true) return;
  const transcript = Array.isArray(state.thread.messages) ? state.thread.messages : [];
  const index = transcript.findIndex((message) => message.ordinal === ordinal);
  const following = index >= 0 ? transcript.length - index - 1 : 0;
  if (!await askConfirm({
    title: "Delete message?",
    message: following > 0
      ? "Delete this message and every message after it? This cannot be undone."
      : "Delete this message? This cannot be undone.",
    confirmLabel: "Delete",
    danger: true,
  })) return;
  const threadId = state.thread.id;
  try {
    await threadSettingsSaves.get(threadId);
    if (state.thread?.id !== threadId) return;
    const response = await api(
      `${API_ROOT}/chat/threads/${encodeURIComponent(threadId)}/delete-message`, {
        method: "POST",
        body: { revision: state.thread.revision, ordinal },
      });
    state.chatEdit = null;
    pruneChatNoticesFrom(threadId, ordinal);
    if (response.thread && state.thread?.id === threadId) {
      state.thread = response.thread;
      threadSettingsSnapshots.set(threadId, state.thread);
      applyThreadModelSettings(state.thread);
      renderChat();
      await loadThreads();
    } else await loadThread(threadId);
  } catch (error) {
    if (error instanceof ApiError && error.code === "revision_conflict") {
      showConflict("The chat thread changed in another client. Reload it before deleting again.",
        () => loadThread(threadId));
    } else chatNotice(errorMessage(error), "error", threadId);
  }
}

async function abandonUnusedThread(thread) {
  if (!thread || thread.read_only === true) return;
  if (state.thread && state.thread.id === thread.id) return;
  try {
    await threadSettingsSaves.get(thread.id);
    const snapshot = threadSettingsSnapshots.get(thread.id) || thread;
    const result = await api(`${API_ROOT}/chat/threads/${encodeURIComponent(thread.id)}/abandon`, {
      method: "POST", body: { revision: snapshot.revision },
    });
    if (result.deleted) {
      threadSettingsSnapshots.delete(thread.id);
      state.chatNotices.delete(thread.id);
      await loadThreads();
    }
  } catch (error) {
    if (!(error instanceof ApiError) || ![404, 409].includes(error.status)) {
      chatNotice(`Could not clean up the unused chat: ${errorMessage(error)}`, "error", thread.id);
    }
  }
}

function lastChatRouting() {
  const usable = (thread) => thread && typeof thread.provider === "string" &&
    thread.provider && thread.provider !== "none";
  if (usable(state.thread)) {
    return { provider: state.thread.provider, model: state.thread.model || "" };
  }
  const listed = state.threads.find((thread) => usable(thread));
  if (listed) return { provider: listed.provider, model: listed.model || "" };
  return {};
}

async function startNewChat() {
  const routing = lastChatRouting();
  const values = routing.provider ? routing : { provider: "none" };
  return createNewChat(values, !routing.provider);
}

async function createNewChat(values = {}, promptForRouting = false) {
  const previous = state.thread;
  releaseChatInputs();
  state.startingNewChat = true;
  state.thread = null;
  renderChat();
  try {
    const response = await api(`${API_ROOT}/chat/threads`, {
      method: "POST",
      body: optionalPayload({ revision: 0, ...values }),
    });
    state.thread = response.thread;
    state.startingNewChat = false;
    state.chatEdit = null;
    threadSettingsSnapshots.set(state.thread.id, state.thread);
    applyThreadModelSettings(state.thread);
    await loadThreads();
    renderChat();
    if (previous) void abandonUnusedThread(previous);
    const provider = state.thread.provider || "";
    if (promptForRouting && (!provider || provider === "none")) {
      const control = modelControls().find((item) => item.providerId === "chat-provider");
      openModelPicker(control, "provider");
    }
    return state.thread;
  } catch (error) {
    state.startingNewChat = false;
    state.thread = previous;
    renderChat();
    throw error;
  }
}

function applyThreadModelSettings(thread) {
  if (!thread) return;
  const provider = byId("chat-provider");
  const storedProvider = typeof thread.provider === "string" ? thread.provider : "";
  if (storedProvider && ![...provider.options].some((option) => option.value === storedProvider)) {
    const option = element("option", "", storedProvider); option.value = storedProvider; provider.append(option);
  }
  provider.value = storedProvider;
  byId("chat-model").value = typeof thread.model === "string" ? thread.model : "";
  const control = modelControls().find((item) => item.providerId === "chat-provider");
  if (control) refreshModelControl(control);
  const reasoning = byId("chat-reasoning");
  const value = thread.settings?.reasoning || "auto";
  if (![...reasoning.options].some((option) => option.value === value)) {
    const option = element("option", "", value); option.value = value; reasoning.append(option);
  }
  reasoning.value = value;
  renderChatToolbar(); renderModelSettings("chat"); updateSettingsAvailability();
}

async function appendThreadMessages(threadId, revision, messages, metadata = null, signal) {
  const body = { revision, messages };
  if (metadata) {
    body.provider = metadata.provider;
    body.model = metadata.model;
  }
  return api(`${API_ROOT}/chat/threads/${encodeURIComponent(threadId)}/messages`, {
    method: "POST",
    body,
    signal,
  });
}

function chatInputName(input) {
  return input.file ? input.file.name : (input.name || "attachment");
}

function chatInputSize(input) {
  if (input.file) return input.file.size;
  const size = Number(input.size);
  return Number.isFinite(size) && size >= 0 ? size : 0;
}

async function fetchChatUrl(url, errorNode = null) {
  const target = String(url || "").trim();
  const thread = state.thread;
  const report = (message, severity = "error") => {
    if (errorNode) errorNode.textContent = message;
    else chatNotice(message, severity, thread?.id ?? null);
  };
  if (!thread) {
    report("Select a chat thread first");
    return false;
  }
  if (thread.read_only === true) {
    report("This thread is read-only");
    return false;
  }
  if (!supports("chat_inputs")) {
    report("This server does not accept chat attachments");
    return false;
  }
  if (!/^https?:\/\//i.test(target)) {
    report("Fetch requires an absolute URL");
    return false;
  }
  if (state.chatInputs.length >= 16) {
    report("Chat accepts at most 16 attachments");
    return false;
  }
  try {
    const stored = await api(`${API_ROOT}/chat/inputs/fetch`, {
      method: "POST",
      body: {
        url: target,
        provider: thread.provider || "",
        model: thread.model || "",
      },
    });
    if (state.thread?.id !== thread.id) {
      if (stored?.id) {
        await api(`${API_ROOT}/chat/inputs/${encodeURIComponent(stored.id)}`, { method: "DELETE" }).catch(() => {});
      }
      return false;
    }
    state.chatInputs.push({
      file: null,
      name: stored.display_name || target,
      size: Number(stored.byte_size) || 0,
      uploadId: stored.id,
      kind: stored.kind,
      stored,
    });
    renderChat();
    return true;
  } catch (error) {
    report(errorMessage(error));
    return false;
  }
}

function renderChatAttachList() {
  const list = byId("chat-attach-list");
  if (!list) return;
  clear(list);
  for (const input of state.chatInputs) {
    const chip = element("span", "chat-attach-chip");
    chip.append(element("span", "", `${chatInputName(input)} · ${formatBytes(chatInputSize(input))}`));
    const remove = element("button", "ghost", "Remove");
    remove.type = "button";
    remove.addEventListener("click", () => void removeChatInput(input));
    chip.append(remove);
    list.append(chip);
  }
}

async function removeChatInput(input) {
  state.chatInputs = state.chatInputs.filter((item) => item !== input);
  if (input.uploadId && state.token) {
    await api(`${API_ROOT}/chat/inputs/${encodeURIComponent(input.uploadId)}`, {
      method: "DELETE",
    }).catch(() => {});
  }
  renderChat();
}

function releaseChatInputs() {
  const token = state.token;
  const csrfToken = state.csrfToken;
  for (const input of state.chatInputs) {
    if (input.uploadId && token && csrfToken) {
      void fetch(`${API_ROOT}/chat/inputs/${encodeURIComponent(input.uploadId)}`, {
        method: "DELETE",
        headers: controlHeaders("DELETE", {}, token, csrfToken),
        credentials: "omit",
        cache: "no-store",
        referrerPolicy: "no-referrer",
      }).catch(() => {});
    }
  }
  state.chatInputs = [];
  renderChatAttachList();
}

function queueChatFiles(fileList) {
  const files = [...fileList];
  for (const file of files) {
    if (state.chatInputs.length >= 16) {
      chatNotice("Chat accepts at most 16 attachments", "error");
      break;
    }
    state.chatInputs.push({ file, uploadId: "" });
  }
  renderChat();
}

async function uploadChatInputs(signal, inputs = state.chatInputs) {
  for (const input of inputs) {
    if (!input.uploadId) {
      const stored = await api(`${API_ROOT}/chat/inputs`, {
        method: "POST",
        rawBody: input.file,
        contentType: input.file.type || "application/octet-stream",
        headers: { "X-Ainiux-Filename": headerFileName(input.file.name) },
        signal,
      });
      input.uploadId = stored.id;
      input.kind = stored.kind;
      input.converted = stored.converted;
      input.stored = stored;
    }
  }
  return {
    ids: inputs.map((input) => input.uploadId),
    stored: inputs.map((input) => input.stored).filter(Boolean),
  };
}

function formatConversionNotice(stored) {
  const elapsedUs = Number(stored.conversion_elapsed_us);
  const elapsed = Number.isFinite(elapsedUs) && elapsedUs >= 0
    ? (elapsedUs / 1000).toFixed(3) : "0.000";
  const resultBytes = Number(stored.byte_size);
  const result = Number.isFinite(resultBytes) && resultBytes >= 0
    ? new Intl.NumberFormat().format(Math.round(resultBytes)) : "0";
  return `Attached and converted ${stored.display_name || "document"} ` +
    `(${formatBytes(stored.source_byte_size)}) in ${elapsed} ms, resulting in ` +
    `${result} bytes of Markdown.`;
}

function showConflict(message, action) {
  state.conflictAction = action;
  byId("conflict-message").textContent = message;
  openDialog(byId("conflict-dialog"));
}

async function sendChatMessage(text) {
  if (!state.thread || chatTurnBusy(state.thread.id)) return;
  const requestedId = state.thread.id;
  try { await threadSettingsSaves.get(requestedId); }
  catch (error) {
    chatNotice(errorMessage(error), "error", requestedId);
    return;
  }
  if (state.thread?.id !== requestedId) return;
  const provider = state.thread.provider || "";
  if (!provider || provider === "none") {
    chatNotice("Choose a provider before sending", "error", requestedId);
    const control = modelControls().find((item) => item.providerId === "chat-provider");
    openModelPicker(control, "provider");
    return;
  }
  const threadId = state.thread.id;
  const controller = new AbortController();
  chatSendAborts.set(threadId, controller);
  beginChatTurn(threadId);
  renderChat();
  let sendingThread = state.thread;
  try {
    const selected = {
      provider: sendingThread.provider || "",
      model: sendingThread.model || "",
    };
    const pendingInputs = [...state.chatInputs];
    const attachedName = pendingInputs[0] ? chatInputName(pendingInputs[0]) : "";
    const uploaded = pendingInputs.length
      ? await uploadChatInputs(controller.signal, pendingInputs)
      : { ids: [], stored: [] };
    const inputIds = uploaded.ids;
    const userMessage = { role: "user", content: text };
    if (inputIds.length) userMessage.input_ids = inputIds;
    const appended = await appendThreadMessages(threadId, sendingThread.revision,
      [userMessage], selected, controller.signal);
    const ordinalValue = Number(appended.thread?.first_ordinal);
    const ordinal = Number.isSafeInteger(ordinalValue) && ordinalValue >= 0
      ? ordinalValue : Math.max(0, Number(appended.thread?.message_count || 1) - 1);
    const attachments = uploaded.stored.map((stored) => ({
      kind: stored.kind,
      mime_type: stored.mime_type,
      display_name: stored.display_name,
      byte_size: stored.byte_size,
    }));
    sendingThread = { ...sendingThread, ...appended.thread,
      messages: [...sendingThread.messages, {
        ordinal, role: "user", content: text, attachments,
      }] };
    if (!sendingThread.name || sendingThread.name === "New chat") {
      const firstLine = text.split(/\r?\n/, 1)[0].trim();
      sendingThread.name = [...firstLine].slice(0, 40).join("") ||
        attachedName || "New chat";
    }
    if (state.thread?.id === threadId) state.thread = sendingThread;
    threadSettingsSnapshots.set(threadId, sendingThread);
    for (const stored of uploaded.stored) {
      if (stored.converted) {
        chatNotice(formatConversionNotice(stored), "message", threadId, ordinal);
      }
    }
    for (const stored of uploaded.stored) {
      for (const warning of stored.warnings || []) {
        chatNotice(warning, "warning", threadId, ordinal);
      }
    }
    const payload = optionalPayload({
      ...selected,
      settings: sendingThread.settings || {},
      input_ids: inputIds,
      thread_id: threadId,
      search_query: state.chatWebSearch ? text : "",
    });
    const context = {
      type: "chat",
      threadId,
      revision: sendingThread.revision,
      streamText: "",
      jobId: "",
    };
    state.chatStreams.set(threadId, context);
    renderChat();
    const job = await submitJob("chat", payload, context, controller.signal);
    state.chatPendingJobByThread.set(threadId, job.id);
    if (state.thread?.id === threadId) state.chatPendingJobId = job.id;
    if (state.thread?.id === threadId) {
      byId("chat-input").value = "";
      releaseChatInputs();
    }
    renderChat();
  } catch (error) {
    endChatTurn(threadId, false);
    state.chatStreams.delete(threadId);
    renderChat();
    if (error && error.name === "AbortError") return;
    if (error instanceof ApiError && error.code === "revision_conflict") {
      showConflict("The chat thread changed in another client. Reload it before sending again.",
        () => loadThread(threadId));
    } else chatNotice(errorMessage(error), "error", threadId);
  } finally {
    if (chatSendAborts.get(threadId) === controller) chatSendAborts.delete(threadId);
  }
}

async function regenerateChat() {
  if (!state.thread || state.thread.read_only === true) {
    chatNotice("Select a writable chat thread before regenerating", "error");
    return;
  }
  const threadId = state.thread.id;
  if (chatTurnBusy(threadId)) {
    const jobId = state.chatPendingJobByThread.get(threadId) || state.chatPendingJobId;
    if (!jobId) return;
    state.chatRegenerateQueued = true;
    await cancelJob(jobId);
    return;
  }
  try {
    await threadSettingsSaves.get(threadId);
    if (state.thread?.id !== threadId) return;
    const original = state.thread;
    const response = await api(
      `${API_ROOT}/chat/threads/${encodeURIComponent(threadId)}/regenerate`, {
        method: "POST",
        body: { revision: state.thread.revision },
      });
    if (state.thread?.id !== threadId) return;
    const messages = Array.isArray(original.messages) ? original.messages : [];
    let userIndex = messages.length - 1;
    while (userIndex >= 0 && messages[userIndex].role !== "user") userIndex -= 1;
    if (userIndex < 0) throw new Error("This thread has no user prompt to regenerate");
    const promptOrdinal = Number(messages[userIndex].ordinal);
    const firstRemovedOrdinal = Number.isSafeInteger(promptOrdinal) && promptOrdinal >= 0
      ? promptOrdinal + 1 : Number(response.thread?.message_count);
    pruneChatNoticesFrom(threadId, firstRemovedOrdinal);
    state.thread = {
      ...state.thread,
      ...response.thread,
      messages: messages.slice(0, userIndex + 1),
    };
    threadSettingsSnapshots.set(threadId, state.thread);
    renderChat();
    await sendChatMessageFromTranscript(response.prompt);
  } catch (error) {
    if (error instanceof ApiError && error.code === "revision_conflict") {
      showConflict("The chat thread changed in another client. Reload it before regenerating.",
        () => loadThread(threadId));
    } else chatNotice(errorMessage(error), "error", threadId);
  }
}

async function sendChatMessageFromTranscript(prompt) {
  if (!state.thread || chatTurnBusy(state.thread.id)) return;
  const threadId = state.thread.id;
  beginChatTurn(threadId);
  const payload = optionalPayload({
    provider: byId("chat-provider").value,
    model: byId("chat-model").value.trim(),
    reasoning: byId("chat-reasoning").value,
    settings: state.thread.settings || {},
    thread_id: threadId,
  });
  const context = {
    type: "chat",
    threadId,
    revision: state.thread.revision,
    streamText: "",
    jobId: "",
  };
  state.chatStreams.set(threadId, context);
  renderChat();
  try {
    const job = await submitJob("chat", payload, context);
    state.chatPendingJobByThread.set(threadId, job.id);
    if (state.thread?.id === threadId) state.chatPendingJobId = job.id;
  } catch (error) {
    endChatTurn(threadId, false);
    state.chatStreams.delete(threadId);
    renderChat();
    throw error;
  }
  if (prompt) byId("chat-input").value = "";
}

async function finishChatJob(job, context) {
  const regenerate = state.chatRegenerateQueued;
  state.chatRegenerateQueued = false;
  endChatTurn(context.threadId, false);
  if (job.state !== "succeeded" || !job.result || typeof job.result.content !== "string") {
    state.chatStreams.delete(context.threadId);
    renderChat();
    if (regenerate) void regenerateChat();
    return;
  }
  context.streamText = job.result.content;
  updateVisibleChatStream(context);
  try {
    state.chatMetrics.set(context.threadId, job.result.metrics || null);
    await appendThreadMessages(context.threadId, context.revision,
      [{ role: "assistant", content: job.result.content }], {
        provider: typeof job.result.provider === "string" ? job.result.provider : "",
        model: typeof job.result.model === "string" ? job.result.model : "",
      });
    state.chatStreams.delete(context.threadId);
    if (state.thread && state.thread.id === context.threadId) await loadThread(context.threadId);
    await loadThreads();
  } catch (error) {
    if (error instanceof ApiError && error.code === "revision_conflict") {
      showConflict("The model response completed, but the thread changed before it could be appended. The response remains visible in Jobs; reload the thread to continue.",
        () => loadThread(context.threadId));
    } else chatNotice(errorMessage(error), "error", context.threadId);
  } finally {
    renderChat();
    if (regenerate) void regenerateChat();
  }
}

function stopAgentClock() {
  if (state.agentClockTimer !== null) window.clearInterval(state.agentClockTimer);
  state.agentClockTimer = null;
  state.agentClock = null;
}

function syncAgentClock() {
  const session = state.session;
  if (!session || !session.turn_id) {
    stopAgentClock();
    return;
  }
  if (!state.agentClock || state.agentClock.sessionId !== session.id ||
      state.agentClock.turnId !== session.turn_id) {
    state.agentClock = {
      sessionId: session.id,
      turnId: session.turn_id,
      baseMs: Number.isFinite(Number(session.active_elapsed_ms)) ? Number(session.active_elapsed_ms) : 0,
      startedAt: performance.now(),
    };
  }
  if (state.agentClockTimer === null) {
    state.agentClockTimer = window.setInterval(renderAgentMetrics, 250);
  }
}

function renderAgentMetrics() {
  const target = byId("agent-metrics");
  if (!state.session) {
    target.textContent = "";
    return;
  }
  let activeElapsed = null;
  let metrics = state.session.last_turn_metrics;
  if (state.session.turn_id && state.agentClock) {
    activeElapsed = state.agentClock.baseMs + performance.now() - state.agentClock.startedAt;
    metrics = null;
  }
  target.textContent = metricText(metrics, null, activeElapsed, false);
}

function renderAgentContext() {
  const target = byId("agent-context");
  const context = state.session && state.session.context;
  const used = context && typeof context === "object"
    ? context.used_tokens : Number.NaN;
  const usedText = formatCompactTokenCount(
    context && typeof context === "object" ? context.used_tokens : null);
  if (!Number.isFinite(used) || used < 0 || !usedText) {
    target.hidden = true;
    if (target.textContent) target.textContent = "";
    target.removeAttribute("aria-label");
    target.removeAttribute("title");
    return;
  }
  const windowTokens = context.window_tokens;
  let visible = usedText;
  let accessible = `Estimated context usage: ${usedText} tokens`;
  const percentValue = (used / windowTokens) * 100;
  if (typeof windowTokens === "number" && Number.isFinite(windowTokens) &&
      windowTokens > 0 && Number.isFinite(percentValue)) {
    const percent = percentValue.toFixed(1).replace(/\.0$/, "");
    visible += ` (${percent}%)`;
    accessible += `, ${percent}% of the context window`;
  }
  if (target.textContent !== visible) target.textContent = visible;
  if (target.getAttribute("aria-label") !== accessible) {
    target.setAttribute("aria-label", accessible);
  }
  if (target.title !== accessible) target.title = accessible;
  target.hidden = false;
}

function agentEventDisplay(entry, live) {
  const type = entry.type || "event";
  const data = entry.data || {};
  let label = type;
  let kind = type;
  if (type === "turn_started") { label = "you"; kind = "user"; }
  else if (type === "turn_completed") { label = "assistant"; kind = "assistant"; }
  else if (type === "turn_failed") { label = "error"; kind = "error"; }
  else if (type === "session_error") { label = "error"; kind = "error"; }
  else if (type === "approval_required") { label = "guard"; kind = "notice"; }
  else if (type === "thinking") label = "thinking";
  else if (type === "tool") label = "tool";
  else if (type === "assistant" || type === "response") kind = "assistant";
  const text = type === "approval_required" ? (data.message || "Approval required") :
    typeof data.content === "string" ? data.content :
    typeof data.message === "string" ? data.message :
      typeof data.text === "string" ? data.text : JSON.stringify(data, null, 2);
  const taskElapsedMs = type === "turn_completed" ? data.metrics?.elapsed_ms
    : type === "assistant" ? data.task_elapsed_ms : null;
  return { label: live ? `${label} · streaming` : label, kind, text,
    metrics: data.metrics, taskElapsedMs };
}

function formatTaskComplete(value) {
  if (value === null || value === undefined || value === "") return "";
  const elapsedMs = Number(value);
  if (!Number.isFinite(elapsedMs) || elapsedMs < 0) return "";
  if (elapsedMs < 60000) {
    return `Task complete in ${(elapsedMs / 1000).toFixed(2)} seconds.`;
  }
  const totalSeconds = Math.floor(elapsedMs / 1000);
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  let text = `Task complete in ${minutes} ${minutes === 1 ? "minute" : "minutes"}`;
  if (seconds) text += ` and ${seconds} ${seconds === 1 ? "second" : "seconds"}`;
  return `${text}.`;
}

function appendAgentEvent(container, entry, live = false) {
  const display = agentEventDisplay(entry, live);
  const failedTool = display.kind === "tool" && /\b(?:error|failed|denied)\b/i.test(display.text);
  const card = element("article", `event-card ${display.kind}${failedTool ? " failed" : ""}${live ? " streaming" : ""}`);
  card.append(element("div", "event-type", display.label));
  const text = display.text;
  if (text && text !== "{}") {
    if (["user", "assistant", "response"].includes(display.kind)) {
      const output = element("div", "message-content");
      appendMarkdown(output, text);
      card.append(output);
    } else if (/```/.test(text)) {
      // Thinking/tool notices can contain an explicitly fenced source excerpt;
      // use the same Markdown lexer and language highlighter as assistant text.
      const output = element("div", "message-content");
      appendMarkdown(output, text);
      card.append(output);
    } else {
      card.append(element("pre", "", text));
    }
  }
  appendMetrics(card, display.metrics);
  const taskComplete = formatTaskComplete(display.taskElapsedMs);
  if (taskComplete) card.append(element("small", "metrics-strip task-complete", taskComplete));
  container.append(card);
}

function agentEventVisible(event) {
  return event && ["turn_started", "turn_completed", "turn_failed",
    "approval_required", "session_error"].includes(event.type);
}

function applyAgentActivity(sessionId, event, logs) {
  let activities = state.agentActivities.get(sessionId);
  if (!activities) {
    activities = new Map();
    state.agentActivities.set(sessionId, activities);
  }
  const data = event.data || {};
  const key = `${event.turn_id || "turn"}:${data.kind || "activity"}:${data.round_id}:${data.tool_id}`;
  if (data.action === "discard") {
    activities.delete(key);
    return;
  }
  const entry = {
    id: event.id,
    turn_id: event.turn_id,
    type: data.kind || "activity",
    data: { text: data.text || "" },
  };
  if (data.action === "append") {
    const previous = activities.get(key);
    if (previous && previous.data) entry.data.text = `${previous.data.text || ""}${entry.data.text}`;
    activities.set(key, entry);
    return;
  }
  if (data.action === "commit") {
    activities.delete(key);
    logs.push(entry);
    if (logs.length > 150) logs.shift();
  } else {
    activities.set(key, entry);
  }
}

function renderAgent() {
  updateSettingsAvailability();
  renderAgentContext();
  const events = byId("agent-events");
  const followTail = events.classList.contains("empty-state") ||
    events.scrollHeight - events.scrollTop - events.clientHeight <= 40;
  const previousScrollTop = events.scrollTop;
  if (!state.session) {
    byId("agent-meta").textContent = state.agentInitializing
      ? "Preparing the workspace agent…" : "Open Agent to initialize this workspace.";
    byId("agent-metrics").textContent = "";
    if (state.agentUnscopedNotices.length) {
      clear(events);
      for (const notice of state.agentUnscopedNotices) {
        appendConversationNotice(events, notice, "event-card");
      }
      events.scrollTop = events.scrollHeight;
    } else {
      setEmpty(events, state.agentInitializing
        ? "Loading the project context and tools…" : "Open Agent to initialize this workspace.");
    }
    byId("agent-turn-input").disabled = true;
    byId("agent-turn-submit").disabled = true;
    byId("cancel-turn-button").hidden = true;
    byId("agent-cycle-reasoning-button").textContent = "Reasoning: auto";
    byId("agent-cycle-reasoning-button").disabled = true;
    for (const id of ["agent-provider", "agent-model", "agent-reasoning",
      "agent-task-mode", "agent-permission"]) byId(id).disabled = true;
    syncPickerButtons();
    stopAgentClock();
    return;
  }
  const session = state.session;
  byId("agent-meta").textContent = session.status;
  const provider = byId("agent-provider");
  if ([...provider.options].some((option) => option.value === session.provider)) {
    provider.value = session.provider;
  }
  byId("agent-model").value = session.model || "";
  const reasoning = byId("agent-reasoning");
  const reasoningChoices = Array.isArray(session.reasoning_options)
    ? session.reasoning_options : [];
  clear(reasoning);
  for (const choice of reasoningChoices) {
    if (!choice || typeof choice.value !== "string" || !choice.value) continue;
    const option = element("option", "", choice.label || choice.value);
    option.value = choice.value;
    reasoning.append(option);
  }
  if (![...reasoning.options].some((option) => option.value === session.reasoning)) {
    const option = element("option", "", session.reasoning || "Auto");
    option.value = session.reasoning || "auto";
    reasoning.prepend(option);
  }
  reasoning.value = session.reasoning || "auto";
  byId("agent-cycle-reasoning-button").textContent =
    `Reasoning: ${session.reasoning || "auto"}`;
  byId("agent-task-mode").value = session.task_mode || "act";
  byId("agent-permission").value = session.permission_mode || "smart";
  const logs = state.agentLogs.get(session.id) || [];
  const history = state.agentHistory.get(session.id);
  const activities = state.agentActivities.get(session.id);
  const notices = state.agentNotices.get(session.id) || [];
  if (!history?.messages?.length && !logs.length && (!activities || activities.size === 0) &&
      !notices.length) {
    setEmpty(events, "Waiting for session events…");
  }
  else {
    clear(events);
    if (history?.before) {
      const older = element("button", "ghost", "Load older messages"); older.type = "button";
      older.addEventListener("click", () => void loadAgentHistory(session.id, history.before)); events.append(older);
    }
    for (const message of history?.messages || []) appendAgentEvent(events, {
      type: message.role,
      data: { content: message.content, task_elapsed_ms: message.task_elapsed_ms },
    });
    for (const entry of logs) {
      appendAgentEvent(events, entry);
    }
    if (activities) for (const entry of activities.values()) appendAgentEvent(events, entry, true);
    for (const notice of notices) appendConversationNotice(events, notice, "event-card");
    events.scrollTop = followTail ? events.scrollHeight : previousScrollTop;
  }
  const ready = session.status === "ready" && !session.turn_id;
  byId("agent-turn-input").disabled = !ready;
  byId("agent-turn-submit").disabled = !ready;
  byId("cancel-turn-button").hidden = !session.turn_id;
  for (const id of ["agent-provider", "agent-model", "agent-reasoning",
    "agent-task-mode", "agent-permission"]) {
    byId(id).disabled = !ready || state.agentSettingsPending;
  }
  byId("agent-cycle-reasoning-button").disabled = !ready || state.agentSettingsPending;
  syncAgentClock();
  renderAgentMetrics();
  syncPickerButtons();
}

async function loadAgentHistory(sessionId, before = 0) {
  try {
    const response = await api(`${API_ROOT}/sessions/${encodeURIComponent(sessionId)}/history${before ? `?before=${before}` : ""}`);
    if (state.session?.id !== sessionId || (state.session.turn_id || "") !== response.turn_id) return;
    const previous = state.agentHistory.get(sessionId);
    const rows = before ? [...response.messages, ...(previous?.messages || [])] : response.messages;
    const bySeq = new Map(rows.map((row) => [row.seq, row]));
    state.agentHistory.set(sessionId, {
      messages: [...bySeq.values()].sort((a, b) => a.seq - b.seq), before: response.before,
    });
    if (!before) {
      const live = (state.agentLogs.get(sessionId) || []).filter((event) => response.turn_id && event.turn_id === response.turn_id);
      state.agentLogs.set(sessionId, live);
      if (!response.turn_id) state.agentActivities.set(sessionId, new Map());
    }
    renderAgent();
  } catch (error) {
    if (error.status !== 409) agentNotice(errorMessage(error), "error", sessionId);
  }
}

function scheduleAgentRender() {
  if (agentRenderFrame !== null) return;
  agentRenderFrame = window.requestAnimationFrame(() => {
    agentRenderFrame = null;
    renderAgent();
  });
}

async function loadSessions() {
  const noticeSessionId = state.session?.id ?? null;
  try {
    const sessions = await api(`${API_ROOT}/sessions`);
    state.sessions = Array.isArray(sessions) ? sessions : [];
    if (state.session) {
      const updated = state.sessions.find((item) => item.id === state.session.id);
      if (updated) state.session = { ...state.session, ...updated };
    }
    renderAgent();
  } catch (error) { agentNotice(errorMessage(error), "error", noticeSessionId); }
}

async function ensureWorkspaceAgent() {
  if (state.session || state.agentInitializing || !supports("sessions")) return;
  state.agentInitializing = true;
  renderAgent();
  try {
    await loadSessions();
    if (state.sessions.length) {
      const newest = [...state.sessions].sort((left, right) =>
        new Date(right.updated_at || 0).getTime() - new Date(left.updated_at || 0).getTime())[0];
      await selectSession(newest.id);
    } else {
      const response = await api(`${API_ROOT}/sessions/agent`, {
        method: "POST",
        body: { kind: "agent" },
      });
      state.sessions = [response.session];
      await selectSession(response.session.id);
    }
  } catch (error) {
    agentNotice(errorMessage(error), "error", null);
  } finally {
    state.agentInitializing = false;
    renderAgent();
  }
}

function showGuard(sessionId, approval, turnId = null) {
  state.guard = { sessionId, approval, turnId };
  setDetails(byId("guard-details"), [
    ["Tool", approval.tool],
    ["Rule", approval.rule_id],
    ["Message", approval.message],
    ["Command", approval.command_preview],
    ["Review file", approval.review_file],
  ]);
  byId("guard-review").hidden = true;
  byId("guard-review").textContent = "";
  byId("guard-review-button").hidden = !approval.review_file;
  openDialog(byId("guard-dialog"));
}

async function selectSession(sessionId) {
  try {
    if (state.session && state.session.id !== sessionId) {
      stopStream(`session:${state.session.id}`);
      stopAgentClock();
    }
    state.session = await api(`${API_ROOT}/sessions/${encodeURIComponent(sessionId)}`);
    if (!state.agentLogs.has(sessionId)) state.agentLogs.set(sessionId, []);
    if (!state.agentActivities.has(sessionId)) state.agentActivities.set(sessionId, new Map());
    if (!state.agentSeenEvents.has(sessionId)) state.agentSeenEvents.set(sessionId, new Set());
    renderAgent();
    if (state.session.status !== "preparing") await loadAgentHistory(sessionId);
    if (state.session.approval) showGuard(sessionId, {
      ...state.session.approval,
      review_file: state.session.approval.review_file,
    }, state.session.turn_id);
    watchSession(sessionId);
  } catch (error) {
    agentNotice(errorMessage(error), "error", sessionId);
  }
}

function watchSession(sessionId) {
  const key = `session:${sessionId}`;
  startStream(key, `${API_ROOT}/sessions/${encodeURIComponent(sessionId)}/events`,
    (event) => {
      const logs = state.agentLogs.get(sessionId) || [];
      let seen = state.agentSeenEvents.get(sessionId);
      if (!seen) {
        seen = new Set();
        state.agentSeenEvents.set(sessionId, seen);
      }
      if (event && event.id && seen.has(event.id)) return;
      if (event && event.id) {
        seen.add(event.id);
        if (seen.size > 512) seen.delete(seen.values().next().value);
      }
      if (event.type === "activity") applyAgentActivity(sessionId, event, logs);
      else if (agentEventVisible(event)) {
        logs.push(event);
        if (logs.length > 150) logs.shift();
      }
      state.agentLogs.set(sessionId, logs);
      if (event.type === "approval_required") showGuard(sessionId, {
        id: event.data.approval_id,
        ...event.data,
      }, event.turn_id);
      if (state.session && state.session.id === sessionId) {
        if (event.type === "turn_started") {
          state.agentActivities.set(sessionId, new Map());
          state.session.turn_id = event.turn_id;
          state.session.status = "running";
          state.session.active_elapsed_ms = 0;
        }
        if (event.type === "approval_required") state.session.status = "waiting_guard";
        if (event.type === "session_error") state.session.status = "error";
        if (event.type === "ready" || event.type === "session_created" ||
            event.type === "session_closed" || event.type === "reasoning_changed" ||
            event.type === "settings_changed") {
          state.session = event.data;
          if (["ready", "settings_changed", "reasoning_changed"].includes(event.type)) {
            void loadAgentHistory(sessionId);
            void refreshWorkspaceSettings().catch((error) =>
              agentNotice(errorMessage(error), "error", sessionId));
          }
        }
        if (["turn_completed", "turn_failed", "approval_resolved"].includes(event.type)) {
          if (["turn_completed", "turn_failed"].includes(event.type)) {
            state.agentActivities.set(sessionId, new Map());
            state.session.turn_id = null;
            state.session.status = "ready";
            state.session.active_elapsed_ms = null;
            if (event.data && event.data.metrics) state.session.last_turn_metrics = event.data.metrics;
          }
          void refreshSelectedSession();
        }
        if (event.type === "activity" && event.data && event.data.kind === "response" &&
            ["append", "upsert"].includes(event.data.action)) scheduleAgentRender();
        else renderAgent();
      }
    },
    async () => {
      if (state.session && state.session.id === sessionId) await refreshSelectedSession();
      agentNotice("Agent event replay expired; loaded the current session state.",
        "warning", sessionId);
      return state.session?.id === sessionId ? state.session.event_cursor || 0 : 0;
    },
    () => !state.connected || !state.sessions.some((session) => session.id === sessionId),
    state.session?.id === sessionId ? state.session.event_cursor || 0 : 0);
}

async function refreshSelectedSession() {
  if (!state.session) return;
  const sessionId = state.session.id;
  try {
    const refreshed = await api(`${API_ROOT}/sessions/${encodeURIComponent(sessionId)}`);
    if (state.session?.id === sessionId) state.session = refreshed;
    await loadSessions();
    await loadAgentHistory(sessionId);
    renderAgent();
  } catch (error) {
    agentNotice(errorMessage(error), "error", sessionId);
  }
}

function cycleSelect(select) {
  const choices = [...select.options].filter((option) => option.value);
  if (!choices.length) return "";
  const current = choices.findIndex((option) => option.value === select.value);
  const next = choices[(current + 1) % choices.length];
  select.value = next.value;
  select.dispatchEvent(new Event("change", { bubbles: true }));
  return next.textContent || next.value;
}

function cycleChatReasoning() {
  cycleSelect(byId("chat-reasoning"));
  renderChatToolbar();
}

function toggleChatThinking() {
  state.showThinkingTraces = !state.showThinkingTraces;
  storageSet(THINKING_STORAGE_KEY, state.showThinkingTraces ? "show" : "hide");
  renderChat();
}

function toggleChatWebSearch() {
  state.chatWebSearch = !state.chatWebSearch;
  renderChatToolbar();
}

function renderChatToolbar() {
  const regenerate = byId("chat-regenerate-button");
  regenerate.disabled = !state.thread || state.thread.read_only === true;
  const selected = byId("chat-reasoning");
  byId("chat-cycle-reasoning-button").textContent =
    `Reasoning: ${selected.value || "auto"}`;
  const thinking = byId("chat-thinking-button");
  thinking.textContent = `Thinking ${state.showThinkingTraces ? "shown" : "hidden"}`;
  thinking.setAttribute("aria-pressed", state.showThinkingTraces ? "true" : "false");
  const search = byId("chat-web-search-button");
  search.disabled = !state.thread || state.thread.read_only === true;
  search.textContent = `Web search ${state.chatWebSearch ? "on" : "off"}`;
  search.setAttribute("aria-pressed", state.chatWebSearch ? "true" : "false");
}

async function cycleAgentReasoning() {
  if (!state.session || state.session.turn_id) {
    agentNotice("Select an idle agent session before changing reasoning", "error");
    return;
  }
  const configured = Array.isArray(state.session.reasoning_options)
    ? state.session.reasoning_options : [];
  const choices = configured.length
    ? configured.filter((choice) => choice && typeof choice.value === "string" && choice.value)
    : FALLBACK_REASONING_OPTIONS.map(([value, label]) => ({ value, label }));
  if (!choices.length) return;
  const current = choices.findIndex((choice) => choice.value === state.session.reasoning);
  const next = choices[(current + 1) % choices.length];
  await setAgentReasoning(next.value, next.label || next.value);
}

async function setAgentReasoning(value, label = value) {
  if (!state.session || state.session.turn_id || state.agentSettingsPending || !value) return;
  const sessionId = state.session.id;
  state.agentSettingsPending = true;
  renderAgent();
  try {
    const response = await api(
      `${API_ROOT}/sessions/${encodeURIComponent(sessionId)}/reasoning`, {
        method: "POST",
        body: { reasoning: value },
      });
    const index = state.sessions.findIndex((item) => item.id === sessionId);
    if (index >= 0) state.sessions[index] = response;
    if (state.session?.id === sessionId) {
      state.session = response;
      renderAgent();
    }
  } catch (error) {
    agentNotice(errorMessage(error), "error", sessionId);
  } finally {
    state.agentSettingsPending = false;
    renderAgent();
  }
}

async function setAgentSetting(field, value) {
  if (!state.session || state.session.turn_id || state.agentSettingsPending || !value) return;
  const sessionId = state.session.id;
  state.agentSettingsPending = true;
  renderAgent();
  try {
    const response = await api(
      `${API_ROOT}/sessions/${encodeURIComponent(sessionId)}/settings`, {
        method: "POST",
        body: { [field]: value },
      });
    const index = state.sessions.findIndex((item) => item.id === sessionId);
    if (index >= 0) state.sessions[index] = response;
    if (state.session?.id === sessionId) state.session = response;
    if (field === "provider" && state.session?.id === sessionId) {
      const control = modelControls().find((item) => item.providerId === "agent-provider");
      if (control) refreshModelControl(control);
    }
  } catch (error) {
    agentNotice(errorMessage(error), "error", sessionId);
    if (state.session?.id === sessionId) await refreshSelectedSession();
  } finally {
    state.agentSettingsPending = false;
    renderAgent();
  }
}

async function cancelActiveAgentTurn() {
  if (!state.session || !state.session.turn_id) return false;
  const sessionId = state.session.id;
  const turnId = state.session.turn_id;
  try {
    await api(`${API_ROOT}/sessions/${encodeURIComponent(sessionId)}/turns/${encodeURIComponent(turnId)}/cancel`, { method: "POST" });
    if (state.guard?.sessionId === sessionId) {
      state.guard = null;
      closeDialog(byId("guard-dialog"));
    }
    return true;
  } catch (error) {
    agentNotice(errorMessage(error), "error", sessionId);
    return false;
  }
}

async function interruptCurrentTask() {
  const threadId = state.thread && state.thread.id;
  if (activePanelId() === "chat-panel" && threadId != null && chatTurnBusy(threadId)) {
    state.chatRegenerateQueued = false;
    const jobId = state.chatPendingJobByThread.get(threadId) || state.chatPendingJobId;
    if (jobId) await cancelJob(jobId);
    else {
      endChatTurn(threadId, true);
      renderChat();
    }
    return;
  }
  if (activePanelId() === "agent-panel" && await cancelActiveAgentTurn()) return;
  const running = [...state.jobs.values()].reverse()
    .find((job) => !TERMINAL_STATES.has(job.state));
  if (running) await cancelJob(running.id);
}

async function resolveGuard(decision) {
  if (!state.guard) return;
  const guard = state.guard;
  try {
    await api(`${API_ROOT}/sessions/${encodeURIComponent(guard.sessionId)}/approvals/${encodeURIComponent(guard.approval.id)}`, {
      method: "POST",
      body: { decision },
    });
    state.guard = null;
    closeDialog(byId("guard-dialog"));
    await refreshSelectedSession();
  } catch (error) {
    closeDialog(byId("guard-dialog"));
    state.guard = null;
    if (error instanceof ApiError && [404, 409].includes(error.status)) {
      agentNotice("That approval is no longer pending; session state was refreshed.",
        "error", guard.sessionId);
      await refreshSelectedSession();
    } else agentNotice(errorMessage(error), "error", guard.sessionId);
  }
}

async function reviewGuardFile() {
  if (!state.guard) return;
  const guard = state.guard;
  const sessionId = guard.sessionId;
  try {
    const response = await api(`${API_ROOT}/sessions/${encodeURIComponent(sessionId)}/approvals/${encodeURIComponent(guard.approval.id)}/review-file`);
    if (state.guard !== guard) return;
    byId("guard-review").textContent = response.content || "";
    byId("guard-review").hidden = false;
  } catch (error) {
    agentNotice(errorMessage(error), "error", sessionId);
  }
}

function directoryPrefix() {
  return state.directory.path === "." ? "" : `${state.directory.path}/`;
}

function parentDirectory(path) {
  const slash = path.lastIndexOf("/");
  return slash === -1 ? "." : path.slice(0, slash);
}

function openFileCoveredBy(entry) {
  if (!state.file || !entry) return false;
  return state.file.path === entry.path ||
    (entry.type === "directory" && state.file.path.startsWith(`${entry.path}/`));
}

function editorSnapshot() {
  const editor = byId("file-editor");
  return {
    value: editor.value,
    selectionStart: editor.selectionStart,
    selectionEnd: editor.selectionEnd,
    selectionDirection: editor.selectionDirection,
  };
}

function setEditorSnapshot(value) {
  const editor = byId("file-editor");
  editor.value = value.value;
  editor.setSelectionRange(value.selectionStart, value.selectionEnd, value.selectionDirection);
}

function setEditorTabSize(tabWidth) {
  const value = String(Math.max(1, Math.min(32, Number(tabWidth) || 4)));
  byId("file-editor").style.tabSize = value;
  byId("file-edit-highlight").style.tabSize = value;
  byId("file-highlight").style.tabSize = value;
}

function updateEditorIndentControls() {
  const editor = byId("file-editor");
  const loaded = Boolean(state.file);
  const editing = loaded && !editor.disabled;
  const selected = editing && editor.selectionStart !== editor.selectionEnd;
  byId("editor-indent-width").disabled = !loaded;
  byId("editor-indent-style").disabled = !loaded;
  byId("editor-reformat-button").textContent = selected ? "Reformat selection" : "Reformat file";
  byId("editor-reformat-button").disabled = !editing ||
    languageForPath(state.file?.path || "") === "text";
  byId("editor-reformat-button").title = loaded && languageForPath(state.file.path) === "text"
    ? "Reformat is unavailable for plain text; Tab and Shift+Tab still work" : "";
}

function updateEditorHistoryButtons() {
  const editing = Boolean(state.file) && !byId("file-editor").disabled;
  const history = state.file && state.file.history;
  byId("undo-file-button").hidden = !editing;
  byId("redo-file-button").hidden = !editing;
  byId("undo-file-button").disabled = !editing || !history || !history.undo.length;
  byId("redo-file-button").disabled = !editing || !history || !history.redo.length;
  byId("insert-file-button").disabled = !editing || !supports("files") ||
    editorInsertController !== null;
  updateEditorIndentControls();
}

function refreshEditorDraft() {
  if (!state.file) return;
  state.file.dirty = byId("file-editor").value !== state.file.content;
  renderEditHighlight();
  updateEditorMeta();
}

function applyEditorHistory(direction) {
  if (!state.file || !state.file.history || byId("file-editor").disabled) return false;
  byId("file-editor").focus({ preventScroll: true });
  const changed = direction === "undo"
    ? undoEditorChange(state.file.history, editorSnapshot())
    : redoEditorChange(state.file.history, editorSnapshot());
  if (!changed) {
    updateEditorHistoryButtons();
    return false;
  }
  state.file.pendingEditorInput = null;
  setEditorSnapshot(changed);
  refreshEditorDraft();
  return true;
}

function replaceEditorDraft(value) {
  if (!state.file) return;
  const before = editorSnapshot();
  const next = {
    value,
    selectionStart: value.length,
    selectionEnd: value.length,
    selectionDirection: "none",
  };
  setEditorSnapshot(next);
  recordEditorChange(state.file.history, before, next);
  state.file.pendingEditorInput = null;
  refreshEditorDraft();
}

function applyEditorTransformation(next) {
  if (!state.file || !state.file.history || byId("file-editor").disabled) return false;
  const before = editorSnapshot();
  setEditorSnapshot(next);
  if (!recordEditorChange(state.file.history, before, next)) {
    updateEditorHistorySelection(state.file.history, next);
  }
  state.file.pendingEditorInput = null;
  refreshEditorDraft();
  byId("file-editor").focus({ preventScroll: true });
  return before.value !== next.value;
}

function applyEditorIndentation(outdent) {
  if (!state.file) return;
  const next = outdent
    ? outdentEditorSnapshot(editorSnapshot(), state.file.tabWidth)
    : indentEditorSnapshot(editorSnapshot(), state.file.tabWidth, state.file.tabStyle);
  applyEditorTransformation(next);
}

function reformatEditorDraft() {
  if (!state.file || byId("file-editor").disabled) return;
  const language = languageForPath(state.file.path);
  if (language === "text") return;
  if (new TextEncoder().encode(byId("file-editor").value).length > MAX_EDITOR_BYTES) {
    surfaceNotice("workspace-panel", "This draft is too large to reformat safely (1 MiB limit).", "error");
    return;
  }
  const next = reformatEditorSnapshot(editorSnapshot(), language,
    state.file.tabWidth, state.file.tabStyle);
  applyEditorTransformation(next);
  if (next.warning) surfaceNotice("workspace-panel", next.warning, "warning");
}

function setInsertFilePending(pending) {
  byId("insert-file-path").disabled = pending;
  byId("insert-file-submit").disabled = pending;
  byId("insert-file-status").textContent = pending ? "Reading workspace file…" : "";
  updateEditorHistoryButtons();
}

function openInsertFileDialog() {
  if (byId("insert-file-button").disabled) return;
  byId("insert-file-path").value = "";
  byId("insert-file-error").textContent = "";
  byId("insert-file-status").textContent = "";
  openDialog(byId("insert-file-dialog"));
  byId("insert-file-path").focus();
}

function cancelInsertWorkspaceFile() {
  if (editorInsertController) editorInsertController.abort();
  closeDialog(byId("insert-file-dialog"));
}

async function insertWorkspaceFile(source) {
  const editor = byId("file-editor");
  if (!state.file || editor.disabled || editorInsertController) return;
  const target = state.file;
  const revision = target.revision;
  const before = editorSnapshot();
  const position = before.selectionDirection === "backward"
    ? before.selectionStart : before.selectionEnd;
  const controller = new AbortController();
  editorInsertController = controller;
  setInsertFilePending(true);
  try {
    const response = await api(`${API_ROOT}/files?path=${wirePath(source)}`, {
      signal: controller.signal,
    });
    if (state.file !== target || target.revision !== revision || editor.disabled ||
        editor.value !== before.value) {
      closeDialog(byId("insert-file-dialog"));
      surfaceNotice("workspace-panel",
        "Insertion discarded because the target editor changed while the file was loading.",
        "error");
      return;
    }
    const inserted = typeof response.content === "string" ? response.content : "";
    const value = before.value.slice(0, position) + inserted + before.value.slice(position);
    if (new TextEncoder().encode(value).length > MAX_EDITOR_BYTES) {
      throw new ApiError(413, "insert_too_large",
        "The inserted file would exceed the 1 MiB remote editing limit");
    }
    const cursor = position + inserted.length;
    editorInsertController = null;
    setInsertFilePending(false);
    closeDialog(byId("insert-file-dialog"));
    applyEditorTransformation({ value, selectionStart: cursor, selectionEnd: cursor,
      selectionDirection: "none" });
    const converted = response.converted_from ? " as Markdown" : "";
    surfaceNotice("workspace-panel",
      `Inserted ${response.path || source} at cursor${converted}`);
    for (const warning of response.warnings || []) {
      surfaceNotice("workspace-panel", warning, "warning");
    }
  } catch (error) {
    if (!error || error.name !== "AbortError") {
      byId("insert-file-error").textContent = errorMessage(error);
      openDialog(byId("insert-file-dialog"));
    }
  } finally {
    if (editorInsertController === controller) editorInsertController = null;
    setInsertFilePending(false);
  }
}

function clearEditor() {
  state.file = null;
  byId("file-editor").value = "";
  byId("file-editor").disabled = true;
  byId("file-edit-layer").hidden = true;
  byId("file-edit-highlight").replaceChildren();
  byId("file-highlight").hidden = true;
  byId("file-highlight").replaceChildren();
  byId("edit-file-button").hidden = true;
  byId("editor-heading").textContent = "Editor";
  byId("save-file-button").disabled = true;
  byId("editor-assist-button").disabled = true;
  byId("editor-indent-width").value = "4";
  byId("editor-indent-style").value = "spaces";
  setEditorTabSize(4);
  updateEditorHistoryButtons();
  updateEditorMeta();
}

function renderBreadcrumbs() {
  const nav = byId("breadcrumbs");
  clear(nav);
  const root = element("button", "", "workspace");
  root.type = "button";
  root.addEventListener("click", () => void loadDirectory("."));
  nav.append(root);
  if (state.directory.path === ".") return;
  const parts = state.directory.path.split("/");
  let current = "";
  for (const part of parts) {
    nav.append(element("span", "", "/"));
    current = current ? `${current}/${part}` : part;
    const target = current;
    const button = element("button", "", part);
    button.type = "button";
    button.addEventListener("click", () => void loadDirectory(target));
    nav.append(button);
  }
}

function renderDirectory() {
  renderBreadcrumbs();
  const list = byId("directory-list");
  const entries = Array.isArray(state.directory.entries) ? state.directory.entries : [];
  if (!entries.length) {
    setEmpty(list, "This directory is empty.");
    return;
  }
  clear(list);
  for (const entry of entries) {
    const row = element("div", `file-row ${state.file && state.file.path === entry.path ? "selected" : ""}`);
    const main = element("div", "file-main");
    const entryClass = entry.type === "directory" ? "directory-entry" :
      entry.executable ? "executable-entry" : "file-entry";
    const open = element("button", entryClass, `${entry.type === "directory" ? "▸" : "·"} ${entry.name}`);
    open.type = "button";
    open.addEventListener("click", () => entry.type === "directory" ? void loadDirectory(entry.path) : void loadFile(entry.path));
    main.append(open, element("small", "", entry.type === "directory" ? "Directory" : `${formatBytes(entry.size)} · ${entry.modified_at || ""}`));
    const actions = element("div", "file-actions");
    for (const [label, type, className] of [["Rename", "rename", ""], ["Copy", "copy", ""], ["Delete", "delete", "delete"]]) {
      const button = element("button", className, label);
      button.type = "button";
      button.disabled = entry.mutable === false || !supports("workspace_mutations");
      button.addEventListener("click", () => openMutation(type, entry));
      actions.append(button);
    }
    row.append(main, actions);
    list.append(row);
  }
}

async function loadDirectory(path) {
  try {
    const response = await api(`${API_ROOT}/dired?path=${wirePath(path)}`);
    state.directory = response;
    renderDirectory();
  } catch (error) {
    setEmpty(byId("directory-list"), errorMessage(error));
  }
}

async function loadWorkspaceReview() {
  try {
    const response = await api(`${API_ROOT}/workspace/review`);
    const summary = byId("workspace-summary");
    clear(summary);
    const values = response.summary || {};
    summary.append(element("span", "summary-chip", `${values.files || 0} files`),
      element("span", "summary-chip", `${values.directories || 0} directories`),
      element("span", "summary-chip", formatBytes(values.bytes || 0)),
      element("span", "summary-chip", response.truncated ? "Review truncated" : "Complete bounded review"));
  } catch (error) {
    byId("workspace-summary").textContent = errorMessage(error);
  }
}

function updateEditorMeta() {
  if (!state.file) {
    byId("editor-meta").textContent = "Open a text file.";
    return;
  }
  byId("editor-meta").textContent = `${state.file.path} · ${formatBytes(new TextEncoder().encode(byId("file-editor").value).length)}${state.file.dirty ? " · unsaved" : ""}`;
  byId("save-file-button").disabled = !state.file.dirty;
  byId("edit-file-button").hidden = !byId("file-edit-layer").hidden;
  byId("edit-file-button").disabled = !state.file;
  byId("editor-assist-button").disabled = state.file.dirty || !supports("editor_assist");
  updateEditorHistoryButtons();
}

function renderFileHighlight() {
  const viewer = byId("file-highlight");
  viewer.replaceChildren();
  if (!state.file) {
    viewer.hidden = true;
    return;
  }
  appendHighlightedSource(viewer, state.file.content || "", languageForPath(state.file.path));
  setEditorTabSize(state.file.tabWidth);
  viewer.hidden = false;
}

function syncEditorHighlightScroll() {
  const editor = byId("file-editor");
  const highlight = byId("file-edit-highlight");
  highlight.scrollTop = editor.scrollTop;
  highlight.scrollLeft = editor.scrollLeft;
}

function renderEditHighlight() {
  const highlight = byId("file-edit-highlight");
  highlight.replaceChildren();
  if (!state.file) return;
  appendHighlightedSource(highlight, byId("file-editor").value, languageForPath(state.file.path));
  setEditorTabSize(state.file.tabWidth);
  syncEditorHighlightScroll();
}

function beginFileEdit() {
  if (!state.file) return;
  const editor = byId("file-editor");
  byId("file-highlight").hidden = true;
  byId("file-edit-layer").hidden = false;
  editor.disabled = false;
  editor.focus({ preventScroll: true });
  if (state.file.initialEditorPositionPending) {
    editor.setSelectionRange(0, 0, "none");
    editor.scrollTop = 0;
    editor.scrollLeft = 0;
    state.file.initialEditorPositionPending = false;
    updateEditorHistorySelection(state.file.history, editorSnapshot());
  }
  renderEditHighlight();
  byId("edit-file-button").hidden = true;
  updateEditorHistoryButtons();
}

function showFileViewer() {
  if (!state.file || state.file.dirty) return;
  byId("file-edit-layer").hidden = true;
  byId("file-editor").disabled = true;
  renderFileHighlight();
  byId("edit-file-button").hidden = false;
  updateEditorHistoryButtons();
}

async function loadFile(path) {
  if (state.file && state.file.dirty && state.file.path !== path &&
      !await askConfirm({
        title: "Discard draft?",
        message: "Discard the unsaved editor draft and open another file?",
        confirmLabel: "Discard",
        danger: true,
      })) return;
  try {
    const response = await api(`${API_ROOT}/files?path=${wirePath(path)}`);
    const converted = Boolean(response.converted_from);
    for (const warning of response.warnings || []) {
      surfaceNotice("workspace-panel", warning, "warning");
    }
    const openPath = converted && response.suggested_path ? response.suggested_path : response.path;
    let revision = response.revision;
    if (converted && response.suggested_path) {
      try {
        const sibling = await api(`${API_ROOT}/files?path=${wirePath(response.suggested_path)}`);
        revision = sibling.revision;
      } catch (_) {
        revision = "";
      }
    }
    const indentation = detectIndentation(response.content || "", 4, "spaces");
    state.file = { ...response, path: openPath, dirty: converted, history: null, pendingEditorInput: null,
      initialEditorPositionPending: true, tabWidth: indentation.tabWidth,
      tabStyle: indentation.tabStyle, revision };
    const editor = byId("file-editor");
    editor.value = response.content || "";
    editor.setSelectionRange(0, 0, "none");
    editor.scrollTop = 0;
    editor.scrollLeft = 0;
    state.file.history = createEditorHistory(editorSnapshot());
    byId("editor-indent-width").value = String(state.file.tabWidth);
    byId("editor-indent-style").value = state.file.tabStyle;
    setEditorTabSize(state.file.tabWidth);
    editor.disabled = true;
    byId("file-edit-layer").hidden = true;
    renderFileHighlight();
    byId("editor-heading").textContent = openPath;
    updateEditorMeta();
    renderDirectory();
    if (converted) {
      const from = String(response.converted_from || "");
      const label = from.includes("presentationml") ? "PPTX"
        : from.includes("spreadsheetml") ? "XLSX"
        : from.includes("wordprocessingml") ? "DOCX"
        : from.includes("pdf") ? "PDF"
        : "document";
      surfaceNotice("workspace-panel",
        `Converted ${label} to Markdown; Save writes the sibling .md file`);
    }
  } catch (error) {
    surfaceNotice("workspace-panel", errorMessage(error), "error");
  }
}

async function saveFile() {
  if (!state.file || !state.file.dirty) return;
  const path = state.file.path;
  try {
    const content = byId("file-editor").value;
    const response = state.file.revision
      ? await api(`${API_ROOT}/files?path=${wirePath(path)}`, {
          method: "PUT",
          body: { revision: state.file.revision, content },
        })
      : await api(`${API_ROOT}/files`, {
          method: "POST",
          body: { path, content, parent_revision: state.directory.revision },
        });
    state.file.revision = response.file.revision;
    state.file.content = content;
    state.file.dirty = false;
    updateEditorMeta();
    showFileViewer();
    await loadDirectory(state.directory.path);
  } catch (error) {
    if (error instanceof ApiError && error.code === "revision_conflict") {
      showConflict(`The server copy of ${path} changed. Your draft is still in the editor.`, () => loadFile(path));
    } else surfaceNotice("workspace-panel", errorMessage(error), "error");
  }
}

async function openMutation(type, entry = null) {
  if ((type === "rename" || type === "delete") && openFileCoveredBy(entry) &&
      state.file.dirty && !await askConfirm({
        title: "Discard draft?",
        message: "Discard the unsaved editor draft before changing this target?",
        confirmLabel: "Discard",
        danger: true,
      })) {
    return;
  }
  state.mutation = { type, entry };
  byId("mutation-error").textContent = "";
  const title = byId("mutation-title");
  const description = byId("mutation-description");
  const path = byId("mutation-path");
  const confirmRow = byId("mutation-confirm-row");
  const recursiveRow = byId("mutation-recursive-row");
  path.readOnly = type === "delete";
  confirmRow.hidden = type !== "delete";
  recursiveRow.hidden = type !== "delete" || !entry || entry.type !== "directory";
  byId("mutation-confirm").value = "";
  byId("mutation-recursive").checked = false;
  if (type === "create-file") {
    title.textContent = "Create file";
    description.textContent = "Create an empty UTF-8 text file in the reviewed directory.";
    path.value = `${directoryPrefix()}new-file.txt`;
  } else if (type === "mkdir") {
    title.textContent = "Create folder";
    description.textContent = "Create a directory only if the reviewed parent is still current.";
    path.value = `${directoryPrefix()}new-folder`;
  } else if (type === "rename" || type === "copy") {
    title.textContent = type === "rename" ? "Rename or move" : "Copy";
    description.textContent = `Choose a new path inside ${state.directory.path}. Existing targets are never overwritten.`;
    path.value = type === "copy" ? `${directoryPrefix()}${entry.name}-copy` : entry.path;
  } else {
    title.textContent = "Delete target";
    description.textContent = `Deletion requires the exact confirmation: delete ${entry.path}`;
    path.value = entry.path;
    byId("mutation-confirm").placeholder = `delete ${entry.path}`;
  }
  openDialog(byId("mutation-dialog"));
  path.focus();
  path.select();
}

async function applyMutation() {
  if (!state.mutation) return;
  const { type, entry } = state.mutation;
  const path = byId("mutation-path").value.trim();
  if (type !== "delete" && parentDirectory(path) !== state.directory.path) {
    throw new Error(`Choose a destination directly inside ${state.directory.path}.`);
  }
  const openPath = openFileCoveredBy(entry) ? state.file.path : "";
  let response;
  if (type === "create-file") {
    response = await api(`${API_ROOT}/files`, {
      method: "POST",
      body: { path, content: "", parent_revision: state.directory.revision },
    });
  } else {
    let operation;
    if (type === "mkdir") operation = { operation: "mkdir", path, parent_revision: state.directory.revision };
    else if (type === "rename" || type === "copy") operation = {
      operation: type === "rename" ? "move" : "copy",
      path: entry.path,
      revision: entry.revision,
      destination: path,
      destination_parent_revision: state.directory.revision,
    };
    else operation = {
      operation: "delete",
      path: entry.path,
      revision: entry.revision,
      recursive: byId("mutation-recursive").checked,
      confirmation: byId("mutation-confirm").value,
    };
    response = await api(`${API_ROOT}/dired/mutations`, { method: "POST", body: { operations: [operation] } });
    const result = response.results && response.results[0];
    if (!result || !result.ok) {
      const failure = result && result.error ? result.error : {};
      throw new ApiError(409, failure.code, failure.message, failure.details);
    }
  }
  closeDialog(byId("mutation-dialog"));
  state.mutation = null;
  await loadDirectory(state.directory.path);
  await loadWorkspaceReview();
  if (type === "create-file" && response.file) {
    await loadFile(response.file.path);
    beginFileEdit();
  }
  else if (type === "delete" && openPath) clearEditor();
  else if (type === "rename" && openPath) {
    const suffix = openPath === entry.path ? "" : openPath.slice(entry.path.length);
    state.file.dirty = false;
    await loadFile(`${path}${suffix}`);
  }
}

function byteOffset(text, codeUnitOffset) {
  return new TextEncoder().encode(text.slice(0, codeUnitOffset)).length;
}

function applyByteEdit(text, edit) {
  const source = new TextEncoder().encode(text);
  const replacement = new TextEncoder().encode(edit.replacement || "");
  const start = Number(edit.start);
  const length = Number(edit.length);
  if (!Number.isSafeInteger(start) || !Number.isSafeInteger(length) || start < 0 || length < 0 || start + length > source.length) {
    throw new Error("Editor proposal contains an invalid byte range");
  }
  const combined = new Uint8Array(source.length - length + replacement.length);
  combined.set(source.slice(0, start), 0);
  combined.set(replacement, start);
  combined.set(source.slice(start + length), start + replacement.length);
  return new TextDecoder("utf-8", { fatal: true }).decode(combined);
}

async function requestAssist(instruction, selectionOnly) {
  if (!state.file) return;
  if (state.file.dirty) throw new Error("Save the current draft before requesting AI assist");
  const editor = byId("file-editor");
  const payload = optionalPayload({
    path: state.file.path,
    revision: state.file.revision,
    instruction,
  });
  if (selectionOnly) {
    if (editor.selectionStart === editor.selectionEnd) throw new Error("Select a non-empty editor range first");
    payload.selection_start = byteOffset(editor.value, editor.selectionStart);
    payload.selection_end = byteOffset(editor.value, editor.selectionEnd);
  }
  await submitJob("editor-assist", payload, {
    type: "assist",
    path: state.file.path,
    revision: state.file.revision,
    draft: editor.value,
  });
  closeDialog(byId("assist-dialog"));
  switchPanel("jobs-panel");
}

function finishAssistJob(job, context) {
  if (job.state !== "succeeded" || !job.result || !job.result.edit) return;
  if (!state.file || state.file.path !== context.path || state.file.revision !== context.revision ||
      byId("file-editor").value !== context.draft) {
    surfaceNotice("workspace-panel",
      "The assist proposal is ready in Jobs, but the editor changed, so it was not applied.",
      "error");
    return;
  }
  try {
    beginFileEdit();
    replaceEditorDraft(applyByteEdit(byId("file-editor").value, job.result.edit));
    switchPanel("workspace-panel");
    byId("file-editor").focus();
  } catch (error) {
    surfaceNotice("workspace-panel", errorMessage(error), "error");
  }
}

function bindEvents() {
  setupModelSettings();
  installPickerControls();
  window.addEventListener("keydown", (event) => {
    if (event.isComposing) return;
    const modal = document.querySelector("dialog[open]");
    // Use physical keys as well as key values for Option layouts; never catch AltGr.
    const pickerKind = event.code === "KeyP" || event.key.toLowerCase() === "p" ? "provider" :
      event.code === "KeyM" || event.key.toLowerCase() === "m" ? "model" : null;
    if (pickerKind && event.altKey && !event.ctrlKey && !event.metaKey && !event.shiftKey &&
        !event.getModifierState("AltGraph")) {
      const target = modelShortcutControl(modal);
      if (target) {
        event.preventDefault();
        if (!event.repeat) openModelPicker(target, pickerKind);
      }
      return;
    }
    if (modal && event.key !== "Escape") return;
    const key = event.key.toLowerCase();
    const control = event.ctrlKey && !event.altKey && !event.metaKey;
    if (control && key === "r" && activePanelId() === "chat-panel") {
      event.preventDefault();
      void regenerateChat();
      return;
    }
    if ((control || event.altKey) && key === "t") {
      if (activePanelId() === "chat-panel") {
        event.preventDefault();
        cycleChatReasoning();
      } else if (activePanelId() === "agent-panel") {
        event.preventDefault();
        void cycleAgentReasoning();
      }
      return;
    }
    if ((control || event.altKey) && key === "w" && activePanelId() === "chat-panel") {
      event.preventDefault();
      toggleChatThinking();
      return;
    }
    if (event.altKey && !event.ctrlKey && !event.metaKey && key === "s" &&
        activePanelId() === "chat-panel") {
      event.preventDefault();
      toggleChatWebSearch();
      return;
    }
    if (event.key === "Escape") {
      if (modal && modal.id !== "guard-dialog") return;
      if ((chatTurnBusy() || state.chatPendingJobId || (state.session && state.session.turn_id) ||
          [...state.jobs.values()].some((job) => !TERMINAL_STATES.has(job.state)))) {
        event.preventDefault();
        void interruptCurrentTask();
      }
    }
  });
  for (const button of document.querySelectorAll(".primary-nav button")) {
    button.addEventListener("click", () => switchPanel(button.dataset.panel));
  }
  for (const button of document.querySelectorAll(".dialog-cancel")) {
    button.addEventListener("click", () => closeDialog(button.closest("dialog")));
  }
  byId("confirm-submit").addEventListener("click", () => finishConfirm(true));
  byId("confirm-dialog").addEventListener("close", () => {
    if (confirmResolver) finishConfirm(false);
  });
  for (const control of modelControls()) {
    byId(control.providerId).addEventListener("change", () => refreshModelControl(control));
    if (control.reasoningId) {
      byId(control.modelId).addEventListener("input", () => {
        renderReasoningControl(control, state.modelCatalogs.get(modelCatalogKey(control)));
      });
    }
  }
  byId("chat-provider").addEventListener("change", () => void saveChatSettings({
    provider: byId("chat-provider").value, model: byId("chat-model").value.trim(),
  }));
  byId("chat-model").addEventListener("change", () => void saveChatSettings({ model: byId("chat-model").value.trim() }));
  byId("chat-reasoning").addEventListener("change", () => {
    renderChatToolbar(); void saveChatSettings({ settings: { reasoning: byId("chat-reasoning").value || "auto" } });
  });
  byId("workspace-provider").addEventListener("change", () => void saveWorkspaceSettings({
    provider: byId("workspace-provider").value, model: byId("workspace-model").value.trim(),
  }));
  byId("workspace-model").addEventListener("change", () => void saveWorkspaceSettings({ model: byId("workspace-model").value.trim() }));

  byId("theme-select").addEventListener("change", (event) => {
    applyTheme(event.target.value);
  });
  byId("disconnect-button").addEventListener("click", () => forgetAuthentication());
  byId("auth-dialog").addEventListener("cancel", (event) => {
    if (!state.connected) event.preventDefault();
  });
  byId("guard-dialog").addEventListener("cancel", (event) => event.preventDefault());
  byId("auth-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    byId("auth-error").textContent = "";
    try {
      await connect(byId("token-input").value);
    } catch (error) {
      if (error instanceof ApiError && error.status === 401) {
        byId("token-input").value = "";
        byId("auth-error").textContent = "Invalid authentication";
      } else {
        byId("auth-error").textContent = `Could not reach the server: ${errorMessage(error)}`;
      }
      byId("token-input").focus();
    }
  });
  window.addEventListener("online", () => {
    if (state.authenticated && !state.connected) scheduleReconnect(true);
  });

  byId("refresh-settings-button").addEventListener("click", () => {
    const noticeTarget = activeNoticeTarget();
    void refreshSettings().catch((error) => targetNotice(noticeTarget, errorMessage(error), "error"));
    if (state.thread) void loadThread(state.thread.id);
  });
  byId("refresh-jobs-button").addEventListener("click", () => void refreshKnownJobs());
  byId("clear-finished-button").addEventListener("click", () => {
    for (const [id, job] of state.jobs) {
      if ((job.operation === "run" || job.operation === "plan") &&
          TERMINAL_STATES.has(job.state)) state.jobs.delete(id);
    }
    renderJobs();
  });

  byId("goal-job-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const operation = byId("goal-operation").value;
    if (!supports(operation)) {
      surfaceNotice("jobs-panel", `${operation} is not supported by this server`, "error");
      return;
    }
    try {
      await submitJob(operation, optionalPayload({
        goal: byId("goal-input").value.trim(),
        provider: byId("goal-provider").value,
        model: byId("goal-model").value.trim(),
      }));
      byId("goal-input").value = "";
    } catch (error) { surfaceNotice("jobs-panel", errorMessage(error), "error"); }
  });

  byId("image-job-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    state.imageSubmitting = true;
    renderImageOptions();
    try {
      state.imageError = "";
      state.imageResult = null;
      const model = selectedImageModel();
      const validation = imageReferenceError(model);
      if (validation) throw new ApiError(400, "invalid_image_input", validation);
      const custom = byId("image-size").value === "Custom…";
      const makePayload = async () => optionalPayload({
        prompt: byId("image-prompt").value.trim(),
        provider: byId("image-provider").value,
        model: model ? model.model : "",
        size: custom ? `${byId("image-width").value}x${byId("image-height").value}` :
          byId("image-size").value,
        aspect: custom && model && model.size_mode === "aspect" ? "custom" :
          byId("image-aspect").value,
        quality: byId("image-quality").value,
        format: byId("image-format").value,
        input_image_ids: await uploadImageInputs(),
      });
      let job;
      try {
        job = await submitJob("image", await makePayload(), { type: "image" });
      } catch (error) {
        if (!(error instanceof ApiError) || error.code !== "not_found") throw error;
        await Promise.all(state.imageInputs.map(async (input) => {
          if (input.uploadId) {
            await api(`${API_ROOT}/images/inputs/${encodeURIComponent(input.uploadId)}`, {
              method: "DELETE",
            }).catch(() => {});
          }
          input.uploadId = "";
        }));
        job = await submitJob("image", await makePayload(), { type: "image" });
      }
      state.imageJobId = job.id;
      renderImage();
    } catch (error) { surfaceNotice("image-panel", errorMessage(error), "error"); }
    finally {
      state.imageSubmitting = false;
      renderImageOptions();
    }
  });
  byId("image-provider").addEventListener("change", () => renderImageOptions(true));
  byId("image-model").addEventListener("change", () => renderImageOptions());
  for (const id of ["image-size", "image-aspect", "image-quality", "image-format",
    "image-width", "image-height"]) {
    byId(id).addEventListener("change", () => renderImageOptions());
    if (id === "image-width" || id === "image-height") {
      byId(id).addEventListener("input", () => renderImageOptions());
    }
  }
  byId("image-input-files").addEventListener("change", (event) => {
    const additions = Array.from(event.target.files || []);
    const combined = [...state.imageInputs.map((input) => input.file), ...additions];
    const validation = state.imageCatalog
      ? imageFileError(combined, state.imageCatalog.limits) : "Image catalog is unavailable.";
    if (validation) surfaceNotice("image-panel", validation, "error");
    else {
      for (const file of additions) {
        state.imageInputs.push({ file, previewUrl: URL.createObjectURL(file), uploadId: "" });
      }
    }
    event.target.value = "";
    renderImageInputList();
    renderImageOptions();
  });
  byId("image-cancel-button").addEventListener("click", () => {
    if (state.imageJobId) void cancelJob(state.imageJobId);
  });
  byId("image-reset-button").addEventListener("click", resetImageForm);
  byId("image-download-button").addEventListener("click", downloadGeneratedImage);
  byId("video-job-form").addEventListener("submit", async (event) => {
    event.preventDefault(); state.videoSubmitting = true; renderVideoOptions();
    try {
      state.videoError = ""; state.videoResult = null;
      if (state.videoObjectUrl) URL.revokeObjectURL(state.videoObjectUrl);
      state.videoObjectUrl = "";
      const model = selectedVideoModel(); const validation = videoInputError(model);
      if (validation) throw new ApiError(400, "invalid_video_input", validation);
      const settings = {};
      for (const control of byId("video-settings").querySelectorAll("[data-video-setting]")) {
        const type = control.dataset.videoType; let value;
        if (type === "boolean") value = control.checked;
        else if (type === "integer" || type === "number") { if (control.value === "") continue; value = Number(control.value); }
        else { if (control.value === "") continue; value = control.value; }
        settings[control.dataset.videoSetting] = value;
      }
      const job = await submitJob("video", optionalPayload({ prompt: byId("video-prompt").value.trim(), provider: byId("video-provider").value, model: model ? model.model : "", settings, input_media_ids: await uploadVideoInputs() }), { type: "video" });
      state.videoJobId = job.id; renderVideo();
    } catch (error) { surfaceNotice("video-panel", errorMessage(error), "error"); }
    finally { state.videoSubmitting = false; renderVideoOptions(); }
  });
  byId("video-provider").addEventListener("change", () => renderVideoOptions(true));
  byId("video-model").addEventListener("change", () => renderVideoOptions());
  byId("video-input-files").addEventListener("change", (event) => {
    const additions = Array.from(event.target.files || []); state.videoInputs.push(...additions.map((file) => ({ file, uploadId: "" })));
    event.target.value = ""; const validation = videoInputError(selectedVideoModel());
    if (validation) {
      state.videoInputs.splice(state.videoInputs.length - additions.length, additions.length);
      surfaceNotice("video-panel", validation, "error");
    }
    renderVideoInputList(); renderVideoOptions();
  });
  byId("video-cancel-button").addEventListener("click", () => { if (state.videoJobId) void cancelJob(state.videoJobId); });
  byId("video-reset-button").addEventListener("click", () => {
    byId("video-prompt").value = ""; byId("video-input-files").value = ""; releaseAllVideoInputs();
    state.videoResult = null;
    state.videoError = "";
    if (state.videoObjectUrl) URL.revokeObjectURL(state.videoObjectUrl);
    state.videoObjectUrl = "";
    renderVideoOptions(); renderVideo(); byId("video-prompt").focus();
  });
  byId("video-download-button").addEventListener("click", downloadGeneratedVideo);
  byId("edit-file-button").addEventListener("click", beginFileEdit);

  byId("new-thread-button").addEventListener("click", () => {
    byId("new-thread-error").textContent = "";
    void startNewChat().catch((error) => {
      byId("new-thread-error").textContent = errorMessage(error);
      openDialog(byId("new-thread-dialog"));
    });
  });
  byId("chat-regenerate-button").addEventListener("click", () => void regenerateChat());
  byId("chat-cycle-reasoning-button").addEventListener("click", cycleChatReasoning);
  byId("chat-thinking-button").addEventListener("click", toggleChatThinking);
  byId("chat-web-search-button").addEventListener("click", toggleChatWebSearch);
  byId("refresh-threads-button").addEventListener("click", () => void loadThreads());
  byId("thread-search").addEventListener("input", scheduleThreadSearch);
  byId("thread-search").addEventListener("search", () => {
    if (threadSearchTimer !== null) {
      window.clearTimeout(threadSearchTimer);
      threadSearchTimer = null;
    }
    void loadThreads();
  });
  byId("thread-search").addEventListener("keydown", (event) => {
    if (event.key !== "Enter") return;
    event.preventDefault();
    if (threadSearchTimer !== null) {
      window.clearTimeout(threadSearchTimer);
      threadSearchTimer = null;
    }
    void loadThreads();
  });
  byId("new-thread-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    byId("new-thread-error").textContent = "";
    closeDialog(byId("new-thread-dialog"));
    try {
      const routing = lastChatRouting();
      await createNewChat(optionalPayload({
        name: byId("thread-name-input").value.trim(),
        provider: byId("thread-provider").value || routing.provider || "none",
        model: byId("thread-model").value.trim() || routing.model || "",
      }), !(byId("thread-provider").value || routing.provider));
      byId("thread-name-input").value = "";
    } catch (error) {
      byId("new-thread-error").textContent = errorMessage(error);
      openDialog(byId("new-thread-dialog"));
    }
  });
  byId("chat-form").addEventListener("submit", (event) => {
    event.preventDefault();
    if (chatTurnBusy()) {
      void interruptCurrentTask();
      return;
    }
    const text = byId("chat-input").value.trim();
    if (handleChatSlashCommand(text)) {
      byId("chat-input").value = "";
      return;
    }
    if (text || state.chatInputs.length) void sendChatMessage(text);
  });
  byId("chat-attach-button").addEventListener("click", () => byId("chat-attach-files").click());
  for (const kind of ["json", "pdf", "docx", "md"]) {
    byId(`export-${kind}`).addEventListener("click", async () => {
      const target = state.chatExport;
      if (!target) return;
      byId("export-error").textContent = "";
      const ok = await downloadChatDocument(kind, target.scope, target.thread, byId("export-error"));
      if (ok) closeDialog(byId("export-dialog"));
    });
  }
  byId("import-thread-button").addEventListener("click", () => {
    const input = byId("import-thread-file");
    input.value = "";
    input.click();
  });
  byId("import-thread-file").addEventListener("change", () => {
    const file = byId("import-thread-file").files && byId("import-thread-file").files[0];
    if (file) void importChatFile(file);
  });
  byId("chat-fetch-button").addEventListener("click", () => {
    byId("fetch-error").textContent = "";
    byId("fetch-url").value = "";
    openDialog(byId("fetch-dialog"));
    byId("fetch-url").focus();
  });
  byId("fetch-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    byId("fetch-error").textContent = "";
    const ok = await fetchChatUrl(byId("fetch-url").value, byId("fetch-error"));
    if (ok) {
      byId("fetch-url").value = "";
      closeDialog(byId("fetch-dialog"));
    }
  });
  byId("chat-attach-files").addEventListener("change", (event) => {
    queueChatFiles(event.target.files || []);
    event.target.value = "";
  });
  const composer = byId("chat-form");
  composer.addEventListener("dragover", (event) => {
    event.preventDefault();
  });
  composer.addEventListener("drop", (event) => {
    event.preventDefault();
    if (event.dataTransfer?.files?.length) queueChatFiles(event.dataTransfer.files);
  });
  byId("chat-input").addEventListener("keydown", (event) => {
    if (event.key !== "Enter" || event.isComposing || event.shiftKey || event.altKey ||
        event.ctrlKey || event.metaKey) return;
    event.preventDefault();
    byId("chat-form").requestSubmit();
  });

  byId("agent-provider").addEventListener("change", (event) =>
    void saveWorkspaceSettings({ provider: event.target.value,
      model: byId("agent-model").value.trim() }));
  byId("agent-model").addEventListener("change", (event) =>
    void saveWorkspaceSettings({ model: event.target.value.trim() }));
  byId("agent-reasoning").addEventListener("change", (event) =>
    void setAgentReasoning(event.target.value,
      event.target.options[event.target.selectedIndex]?.textContent || event.target.value));
  byId("agent-cycle-reasoning-button").addEventListener("click", () =>
    void cycleAgentReasoning());
  byId("agent-task-mode").addEventListener("change", (event) =>
    void setAgentSetting("task_mode", event.target.value));
  byId("agent-permission").addEventListener("change", (event) =>
    void setAgentSetting("permission_mode", event.target.value));
  byId("agent-turn-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const text = byId("agent-turn-input").value.trim();
    if (handleChatSlashCommand(text)) {
      byId("agent-turn-input").value = "";
      return;
    }
    if (!state.session) return;
    const sessionId = state.session.id;
    try {
      const response = await api(`${API_ROOT}/sessions/${encodeURIComponent(sessionId)}/turns`, {
        method: "POST",
        body: { text },
      });
      if (state.session?.id !== sessionId) return;
      state.session.turn_id = response.turn_id;
      state.session.status = "running";
      state.session.active_elapsed_ms = 0;
      state.agentClock = {
        sessionId: state.session.id,
        turnId: response.turn_id,
        baseMs: 0,
        startedAt: performance.now(),
      };
      byId("agent-turn-input").value = "";
      renderAgent();
    } catch (error) { agentNotice(errorMessage(error), "error", sessionId); }
  });
  byId("agent-turn-input").addEventListener("keydown", (event) => {
    if (event.key !== "Enter" || event.isComposing || event.keyCode === 229) return;
    if ((event.shiftKey || event.altKey) && !event.ctrlKey && !event.metaKey) {
      event.preventDefault();
      const input = event.currentTarget;
      input.setRangeText("\n", input.selectionStart, input.selectionEnd, "end");
      input.dispatchEvent(new Event("input", { bubbles: true }));
      return;
    }
    if (event.ctrlKey || event.metaKey) return;
    event.preventDefault();
    byId("agent-turn-form").requestSubmit();
  });
  byId("cancel-turn-button").addEventListener("click", () => void cancelActiveAgentTurn());
  byId("guard-review-button").addEventListener("click", () => void reviewGuardFile());
  byId("guard-allow-button").addEventListener("click", () => void resolveGuard("allow"));
  byId("guard-deny-button").addEventListener("click", () => void resolveGuard("deny"));

  byId("workspace-review-button").addEventListener("click", () => void loadWorkspaceReview());
  byId("refresh-directory-button").addEventListener("click", () => void loadDirectory(state.directory.path));
  byId("create-file-button").addEventListener("click", () => openMutation("create-file"));
  byId("create-directory-button").addEventListener("click", () => openMutation("mkdir"));
  byId("mutation-form").addEventListener("submit", (event) => {
    event.preventDefault();
    byId("mutation-error").textContent = "";
    void applyMutation().catch((error) => {
      if (error instanceof ApiError && error.code === "revision_conflict") {
        closeDialog(byId("mutation-dialog"));
        showConflict("The workspace changed after this directory was reviewed. Reload the directory before retrying.",
          () => loadDirectory(state.directory.path));
      } else {
        byId("mutation-error").textContent = errorMessage(error);
        openDialog(byId("mutation-dialog"));
      }
    });
  });
  byId("file-editor").addEventListener("keydown", (event) => {
    const direction = editorHistoryDirection(event);
    if (direction) {
      event.preventDefault();
      applyEditorHistory(direction);
      return;
    }
    if (event.key === "Tab" && !event.ctrlKey && !event.metaKey && !event.altKey &&
        !event.isComposing && event.keyCode !== 229) {
      event.preventDefault();
      applyEditorIndentation(event.shiftKey);
    }
  });
  byId("file-editor").addEventListener("beforeinput", (event) => {
    if (!state.file) return;
    if (event.inputType === "historyUndo" || event.inputType === "historyRedo") {
      event.preventDefault();
      applyEditorHistory(event.inputType === "historyUndo" ? "undo" : "redo");
      return;
    }
    state.file.pendingEditorInput = editorSnapshot();
  });
  byId("file-editor").addEventListener("input", () => {
    if (!state.file) return;
    const before = state.file.pendingEditorInput || state.file.history.current;
    state.file.pendingEditorInput = null;
    recordEditorChange(state.file.history, before, editorSnapshot());
    refreshEditorDraft();
  });
  byId("file-editor").addEventListener("select", () => {
    if (state.file && state.file.history) {
      updateEditorHistorySelection(state.file.history, editorSnapshot());
    }
    updateEditorIndentControls();
  });
  byId("file-editor").addEventListener("scroll", syncEditorHighlightScroll);
  byId("undo-file-button").addEventListener("click", () => applyEditorHistory("undo"));
  byId("redo-file-button").addEventListener("click", () => applyEditorHistory("redo"));
  byId("editor-indent-width").addEventListener("change", () => {
    if (!state.file) return;
    const input = byId("editor-indent-width");
    const parsed = Number(input.value);
    state.file.tabWidth = Number.isInteger(parsed) ? Math.max(1, Math.min(32, parsed)) : state.file.tabWidth;
    input.value = String(state.file.tabWidth);
    setEditorTabSize(state.file.tabWidth);
  });
  byId("editor-indent-style").addEventListener("change", () => {
    if (state.file) state.file.tabStyle = byId("editor-indent-style").value === "tab" ? "tab" : "spaces";
  });
  byId("editor-reformat-button").addEventListener("click", reformatEditorDraft);
  byId("insert-file-button").addEventListener("click", openInsertFileDialog);
  byId("insert-file-cancel").addEventListener("click", cancelInsertWorkspaceFile);
  byId("insert-file-dialog").addEventListener("cancel", () => {
    if (editorInsertController) editorInsertController.abort();
  });
  byId("insert-file-form").addEventListener("submit", (event) => {
    event.preventDefault();
    const source = byId("insert-file-path").value.trim();
    if (!source) return;
    byId("insert-file-error").textContent = "";
    void insertWorkspaceFile(source);
  });
  byId("save-file-button").addEventListener("click", () => void saveFile());
  byId("editor-assist-button").addEventListener("click", () => {
    byId("assist-error").textContent = "";
    openDialog(byId("assist-dialog"));
  });
  byId("assist-form").addEventListener("submit", (event) => {
    event.preventDefault();
    byId("assist-error").textContent = "";
    void requestAssist(byId("assist-instruction").value.trim(), byId("assist-selection").checked)
      .catch((error) => {
        byId("assist-error").textContent = errorMessage(error);
        openDialog(byId("assist-dialog"));
      });
  });

  byId("keep-draft-button").addEventListener("click", () => {
    state.conflictAction = null;
    closeDialog(byId("conflict-dialog"));
  });
  byId("reload-conflict-button").addEventListener("click", async () => {
    const action = state.conflictAction;
    state.conflictAction = null;
    closeDialog(byId("conflict-dialog"));
    if (action) await action();
  });
}

async function boot() {
  bindEvents();
  state.showThinkingTraces = storageGet(THINKING_STORAGE_KEY) === "show";
  const theme = storageGet(THEME_STORAGE_KEY);
  if (["auto", "dark", "light"].includes(theme)) {
    applyTheme(theme);
  }
  renderJobs();
  renderImage();
  renderImageInputList();
  renderVideo();
  renderVideoInputList();
  renderChat();
  renderAgent();
  const remembered = storageGet(TOKEN_STORAGE_KEY);
  if (remembered) {
    try {
      await connect(remembered, true);
      return;
    } catch (error) {
      if (error instanceof ApiError && error.status === 401) {
        byId("auth-error").textContent = "Invalid authentication";
      } else {
        byId("auth-error").textContent = errorMessage(error);
      }
    }
  }
  openDialog(byId("auth-dialog"));
  renderImageOptions();
  renderVideoOptions();
}

void boot();
