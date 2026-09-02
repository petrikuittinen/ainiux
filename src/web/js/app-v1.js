const API_ROOT = "/ainiux/v1";
const TOKEN_STORAGE_KEY = "ainiux.controller.token.v1";
const THEME_STORAGE_KEY = "ainiux.ui.theme.v1";
const TERMINAL_STATES = new Set(["succeeded", "failed", "cancelled"]);

const state = {
  token: "",
  connected: false,
  capabilities: null,
  status: null,
  jobs: new Map(),
  streams: new Map(),
  threads: [],
  thread: null,
  chatPending: false,
  sessions: [],
  session: null,
  agentLogs: new Map(),
  guard: null,
  directory: { path: ".", revision: "", entries: [] },
  file: null,
  mutation: null,
  conflictAction: null,
};

const byId = (id) => document.getElementById(id);

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

function storageGet(key) {
  try {
    return sessionStorage.getItem(key) || "";
  } catch (_) {
    return "";
  }
}

function storageSet(key, value) {
  try {
    if (value) sessionStorage.setItem(key, value);
    else sessionStorage.removeItem(key);
  } catch (_) {
    // Private browsing policies may disable tab storage; memory mode still works.
  }
}

function toast(message, kind = "info") {
  const region = byId("toast-region");
  const item = element("div", `toast ${kind === "error" ? "error" : ""}`, message);
  region.append(item);
  window.setTimeout(() => item.remove(), 5500);
}

function errorMessage(error) {
  if (error instanceof ApiError) return `${error.message} (${error.code})`;
  return error instanceof Error ? error.message : "Unexpected controller error";
}

function formatBytes(value) {
  const bytes = Number(value);
  if (!Number.isFinite(bytes) || bytes < 0) return "—";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
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

async function api(path, options = {}) {
  if (!state.token) throw new ApiError(401, "not_connected", "Connect with a controller token first");
  const headers = new Headers(options.headers || {});
  headers.set("Authorization", `Bearer ${state.token}`);
  headers.set("Accept", "application/json");
  let body;
  if (options.body !== undefined) {
    headers.set("Content-Type", "application/json");
    body = JSON.stringify(options.body);
  }
  const response = await fetch(path, {
    method: options.method || "GET",
    headers,
    body,
    signal: options.signal,
    credentials: "omit",
    cache: "no-store",
    referrerPolicy: "no-referrer",
  });
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
    throw new ApiError(response.status, failure.code, failure.message, failure.details);
  }
  return payload;
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

function startStream(key, path, onMessage, onExpired, isDone) {
  stopStream(key);
  const controller = new AbortController();
  state.streams.set(key, controller);
  void (async () => {
    let cursor = 0;
    let failures = 0;
    while (state.connected && !controller.signal.aborted && !isDone()) {
      try {
        const headers = new Headers({
          Authorization: `Bearer ${state.token}`,
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
          await onExpired();
          cursor = 0;
          failures = 0;
          if (isDone()) return;
          continue;
        }
        if (!response.ok) {
          let failure = {};
          try { failure = (await response.json()).error || {}; } catch (_) { /* handled below */ }
          throw new ApiError(response.status, failure.code, failure.message, failure.details);
        }
        failures = 0;
        await readSse(response, (message) => {
          const eventId = Number(message.id || (message.data && message.data.id));
          if (Number.isSafeInteger(eventId) && eventId > cursor) cursor = eventId;
          onMessage(message.data, message.event);
        }, controller.signal);
        if (isDone()) return;
      } catch (error) {
        if (controller.signal.aborted) return;
        failures += 1;
        if (error instanceof ApiError && error.status === 401) {
          disconnect("Controller authentication expired");
          return;
        }
        if (failures >= 6) {
          toast(`Event stream paused: ${errorMessage(error)}`, "error");
          return;
        }
      }
      const delay = Math.min(5000, 300 * (2 ** failures));
      await new Promise((resolve) => window.setTimeout(resolve, delay));
    }
  })().finally(() => {
    if (state.streams.get(key) === controller) state.streams.delete(key);
  });
}

function supports(operation) {
  return Boolean(state.capabilities && Array.isArray(state.capabilities.operations) &&
    state.capabilities.operations.includes(operation));
}

function populateProviders() {
  const providers = state.capabilities && Array.isArray(state.capabilities.providers)
    ? state.capabilities.providers : [];
  for (const select of document.querySelectorAll(".provider-select")) {
    const previous = select.value;
    clear(select);
    const automatic = element("option", "", "Server default");
    automatic.value = "";
    select.append(automatic);
    for (const provider of providers) {
      const option = element("option", "", provider);
      option.value = provider;
      select.append(option);
    }
    if ([...select.options].some((option) => option.value === previous)) select.value = previous;
  }
}

function applyCapabilities() {
  populateProviders();
  byId("new-thread-button").disabled = !supports("chat_threads");
  byId("chat-send").disabled = !state.thread || state.thread.read_only === true ||
    state.chatPending || !supports("chat");
  byId("new-agent-button").disabled = !supports("sessions");
  byId("workspace-review-button").disabled = !supports("review");
  byId("refresh-directory-button").disabled = !supports("dired");
  byId("create-file-button").disabled = !supports("files");
  byId("create-directory-button").disabled = !supports("workspace_mutations");
  for (const control of byId("goal-job-form").elements) control.disabled = !supports("run") && !supports("plan");
  for (const control of byId("image-job-form").elements) control.disabled = !supports("image");
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
  const [capabilities, status] = await Promise.all([
    api(`${API_ROOT}/capabilities`),
    api(`${API_ROOT}/status`),
  ]);
  state.capabilities = capabilities;
  state.status = status;
  renderSettings();
  applyCapabilities();
}

async function connect(token, remember) {
  const cleaned = token.trim();
  if (!cleaned) throw new ApiError(401, "missing_token", "Enter a controller token");
  state.token = cleaned;
  try {
    await refreshSettings();
  } catch (error) {
    state.token = "";
    throw error;
  }
  state.connected = true;
  if (remember) storageSet(TOKEN_STORAGE_KEY, cleaned);
  else storageSet(TOKEN_STORAGE_KEY, "");
  byId("token-input").value = "";
  byId("connection-badge").textContent = "Connected";
  byId("connection-badge").className = "status-badge online";
  byId("disconnect-button").hidden = false;
  closeDialog(byId("auth-dialog"));
  const tasks = [];
  if (supports("chat_threads")) tasks.push(loadThreads());
  if (supports("sessions")) tasks.push(loadSessions());
  if (supports("dired")) tasks.push(loadDirectory("."));
  if (supports("review")) tasks.push(loadWorkspaceReview());
  await Promise.allSettled(tasks);
  toast("Connected to the Ainiux control server");
}

function disconnect(message = "Disconnected") {
  stopAllStreams();
  state.token = "";
  state.connected = false;
  state.capabilities = null;
  state.status = null;
  state.thread = null;
  state.session = null;
  state.file = null;
  state.guard = null;
  storageSet(TOKEN_STORAGE_KEY, "");
  byId("token-input").value = "";
  byId("connection-badge").textContent = "Offline";
  byId("connection-badge").className = "status-badge offline";
  byId("disconnect-button").hidden = true;
  setEmpty(byId("thread-list"), "Connect to load threads.");
  setEmpty(byId("chat-messages"), "Choose or create a thread.");
  setEmpty(byId("agent-list"), "Connect to load sessions.");
  setEmpty(byId("agent-events"), "Create or select an agent session.");
  setEmpty(byId("directory-list"), "Connect to browse the workspace.");
  byId("file-editor").value = "";
  byId("file-editor").disabled = true;
  byId("chat-input").disabled = true;
  byId("agent-turn-input").disabled = true;
  closeDialog(byId("guard-dialog"));
  byId("auth-error").textContent = message;
  openDialog(byId("auth-dialog"));
}

function switchPanel(panelId) {
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
    return;
  }
  if (job.operation === "editor-assist" && result.edit) {
    container.append(element("pre", "", result.edit.replacement || "Empty replacement"));
    return;
  }
  if (TERMINAL_STATES.has(job.state) && Object.keys(result).length) {
    const safeResult = { ...result };
    delete safeResult.data_base64;
    container.append(element("pre", "", JSON.stringify(safeResult, null, 2)));
  }
}

function renderJobs() {
  const list = byId("job-list");
  if (state.jobs.size === 0) {
    setEmpty(list, "No jobs in this tab.");
    return;
  }
  clear(list);
  const jobs = [...state.jobs.values()].reverse();
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
  };
  state.jobs.set(merged.id, merged);
  renderJobs();
  return merged;
}

async function submitJob(operation, payload, context = null) {
  const idempotency = typeof crypto.randomUUID === "function"
    ? crypto.randomUUID() : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const response = await api(`${API_ROOT}/jobs/${operation}`, {
    method: "POST",
    headers: { "Idempotency-Key": `wui-${idempotency}` },
    body: payload,
  });
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
      if (event && TERMINAL_STATES.has(event.type)) job.state = event.type;
      renderJobs();
      if (event && TERMINAL_STATES.has(event.type)) void refreshJob(jobId);
    },
    async () => {
      await refreshJob(jobId);
      toast(`Event replay expired for ${jobId}; loaded its current snapshot.`);
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
    toast(errorMessage(error), "error");
  }
}

async function handleJobCompletion(job) {
  if (job._handled) return;
  job._handled = true;
  if (job._context && job._context.type === "chat") await finishChatJob(job, job._context);
  if (job._context && job._context.type === "assist") finishAssistJob(job, job._context);
}

async function cancelJob(jobId) {
  try {
    const job = await api(`${API_ROOT}/jobs/${encodeURIComponent(jobId)}/cancel`, { method: "POST" });
    updateJob(job);
    toast(`Cancellation requested for ${jobId}`);
  } catch (error) {
    toast(errorMessage(error), "error");
  }
}

async function refreshKnownJobs() {
  await Promise.allSettled([...state.jobs.keys()].map((id) => refreshJob(id)));
}

function renderThreads() {
  const list = byId("thread-list");
  if (!state.threads.length) {
    setEmpty(list, "No saved threads.");
    return;
  }
  clear(list);
  for (const thread of state.threads) {
    const button = element("button", `list-button ${state.thread && state.thread.id === thread.id ? "selected" : ""}`);
    button.type = "button";
    button.append(element("strong", "", thread.name || `Thread ${thread.id}`),
      element("small", "", `${thread.message_count || 0} messages · revision ${thread.revision}`));
    button.addEventListener("click", () => void loadThread(thread.id));
    list.append(button);
  }
}

function renderChat() {
  const messages = byId("chat-messages");
  if (!state.thread) {
    byId("conversation-heading").textContent = "Select a thread";
    byId("thread-meta").textContent = "";
    setEmpty(messages, "Choose or create a thread.");
    byId("chat-input").disabled = true;
    byId("chat-send").disabled = true;
    return;
  }
  byId("conversation-heading").textContent = state.thread.name || `Thread ${state.thread.id}`;
  byId("thread-meta").textContent = `Revision ${state.thread.revision} · ${state.thread.message_count || state.thread.messages.length} messages`;
  const transcript = Array.isArray(state.thread.messages) ? state.thread.messages : [];
  if (!transcript.length) setEmpty(messages, "This thread is empty.");
  else {
    clear(messages);
    for (const message of transcript) {
      const card = element("article", `message ${message.role || "system"}`);
      card.append(element("div", "role", message.role || "message"), element("pre", "", message.content || ""));
      messages.append(card);
    }
    messages.scrollTop = messages.scrollHeight;
  }
  const readOnly = state.thread.read_only === true;
  if (readOnly) byId("thread-meta").textContent += " · read-only";
  byId("chat-input").disabled = readOnly;
  byId("chat-send").disabled = readOnly || state.chatPending || !supports("chat");
  renderThreads();
}

async function loadThreads() {
  try {
    const response = await api(`${API_ROOT}/chat/threads`);
    state.threads = Array.isArray(response.threads) ? response.threads : [];
    renderThreads();
  } catch (error) {
    setEmpty(byId("thread-list"), errorMessage(error));
  }
}

async function loadThread(threadId) {
  try {
    const response = await api(`${API_ROOT}/chat/threads/${encodeURIComponent(threadId)}`);
    state.thread = response.thread;
    renderChat();
  } catch (error) {
    toast(errorMessage(error), "error");
  }
}

async function appendThreadMessages(threadId, revision, messages) {
  return api(`${API_ROOT}/chat/threads/${encodeURIComponent(threadId)}/messages`, {
    method: "POST",
    body: { revision, messages },
  });
}

function showConflict(message, action) {
  state.conflictAction = action;
  byId("conflict-message").textContent = message;
  openDialog(byId("conflict-dialog"));
}

async function sendChatMessage(text) {
  if (!state.thread || state.chatPending) return;
  state.chatPending = true;
  renderChat();
  const threadId = state.thread.id;
  try {
    const appended = await appendThreadMessages(threadId, state.thread.revision,
      [{ role: "user", content: text }]);
    state.thread.revision = appended.thread.revision;
    state.thread.message_count = appended.thread.message_count;
    state.thread.messages.push({ role: "user", content: text });
    const transcript = state.thread.messages.slice(-64).map((message) => ({
      role: message.role,
      content: message.content,
    }));
    const payload = optionalPayload({
      provider: byId("chat-provider").value,
      model: byId("chat-model").value.trim(),
      api: byId("chat-api").value,
      messages: transcript,
    });
    await submitJob("chat", payload, {
      type: "chat",
      threadId,
      revision: state.thread.revision,
    });
    byId("chat-input").value = "";
    renderChat();
    switchPanel("jobs-panel");
  } catch (error) {
    state.chatPending = false;
    renderChat();
    if (error instanceof ApiError && error.code === "revision_conflict") {
      showConflict("The chat thread changed in another client. Reload it before sending again.",
        () => loadThread(threadId));
    } else toast(errorMessage(error), "error");
  }
}

async function finishChatJob(job, context) {
  state.chatPending = false;
  if (job.state !== "succeeded" || !job.result || typeof job.result.content !== "string") {
    renderChat();
    return;
  }
  try {
    await appendThreadMessages(context.threadId, context.revision,
      [{ role: "assistant", content: job.result.content }]);
    if (state.thread && state.thread.id === context.threadId) await loadThread(context.threadId);
    await loadThreads();
    toast("Chat response saved to the thread");
  } catch (error) {
    if (error instanceof ApiError && error.code === "revision_conflict") {
      showConflict("The model response completed, but the thread changed before it could be appended. The response remains visible in Jobs; reload the thread to continue.",
        () => loadThread(context.threadId));
    } else toast(errorMessage(error), "error");
  } finally {
    renderChat();
  }
}

function renderSessions() {
  const list = byId("agent-list");
  if (!state.sessions.length) {
    setEmpty(list, "No retained agent sessions.");
    return;
  }
  clear(list);
  for (const session of state.sessions) {
    const selected = state.session && state.session.id === session.id;
    const button = element("button", `list-button ${selected ? "selected" : ""}`);
    button.type = "button";
    button.append(element("strong", "", `${session.task_mode || "act"} · ${session.id}`),
      element("small", "", `${session.status} · ${session.provider || "provider"} / ${session.model || "default"}`));
    button.addEventListener("click", () => void selectSession(session.id));
    list.append(button);
  }
}

function renderAgent() {
  renderSessions();
  const events = byId("agent-events");
  if (!state.session) {
    byId("agent-console-heading").textContent = "No active session";
    byId("agent-meta").textContent = "";
    setEmpty(events, "Create or select an agent session.");
    byId("agent-turn-input").disabled = true;
    byId("agent-turn-submit").disabled = true;
    byId("cancel-turn-button").hidden = true;
    byId("close-agent-button").hidden = true;
    return;
  }
  const session = state.session;
  byId("agent-console-heading").textContent = session.id;
  byId("agent-meta").textContent = `${session.status} · ${session.task_mode} · ${session.permission_mode} · ${session.provider || "provider"} / ${session.model || "default"}`;
  const logs = state.agentLogs.get(session.id) || [];
  if (!logs.length) setEmpty(events, "Waiting for session events…");
  else {
    clear(events);
    for (const entry of logs) {
      const card = element("article", "event-card");
      card.append(element("div", "event-type", entry.type || "event"));
      const data = entry.data || {};
      const text = typeof data.content === "string" ? data.content :
        typeof data.message === "string" ? data.message :
          typeof data.text === "string" ? data.text : JSON.stringify(data, null, 2);
      if (text && text !== "{}") card.append(element("pre", "", text));
      events.append(card);
    }
    events.scrollTop = events.scrollHeight;
  }
  const ready = session.status === "ready" && !session.turn_id;
  byId("agent-turn-input").disabled = !ready;
  byId("agent-turn-submit").disabled = !ready;
  byId("cancel-turn-button").hidden = !session.turn_id;
  byId("close-agent-button").hidden = false;
}

async function loadSessions() {
  try {
    const sessions = await api(`${API_ROOT}/sessions`);
    state.sessions = Array.isArray(sessions) ? sessions : [];
    if (state.session) {
      const updated = state.sessions.find((item) => item.id === state.session.id);
      if (updated) state.session = updated;
    }
    renderAgent();
  } catch (error) {
    setEmpty(byId("agent-list"), errorMessage(error));
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
    if (state.session && state.session.id !== sessionId) stopStream(`session:${state.session.id}`);
    state.session = await api(`${API_ROOT}/sessions/${encodeURIComponent(sessionId)}`);
    if (!state.agentLogs.has(sessionId)) state.agentLogs.set(sessionId, []);
    renderAgent();
    if (state.session.approval) showGuard(sessionId, {
      ...state.session.approval,
      review_file: state.session.approval.review_file,
    }, state.session.turn_id);
    watchSession(sessionId);
  } catch (error) {
    toast(errorMessage(error), "error");
  }
}

function watchSession(sessionId) {
  const key = `session:${sessionId}`;
  startStream(key, `${API_ROOT}/sessions/${encodeURIComponent(sessionId)}/events`,
    (event) => {
      const logs = state.agentLogs.get(sessionId) || [];
      if (event && event.id && logs.some((entry) => entry.id === event.id)) return;
      logs.push(event);
      if (logs.length > 150) logs.shift();
      state.agentLogs.set(sessionId, logs);
      if (event.type === "approval_required") showGuard(sessionId, {
        id: event.data.approval_id,
        ...event.data,
      }, event.turn_id);
      if (state.session && state.session.id === sessionId) {
        if (event.type === "ready" || event.type === "session_created" || event.type === "session_closed") {
          state.session = event.data;
        }
        if (["turn_completed", "turn_failed", "approval_resolved"].includes(event.type)) {
          void refreshSelectedSession();
        }
        renderAgent();
      }
    },
    async () => {
      if (state.session && state.session.id === sessionId) await refreshSelectedSession();
      toast("Agent event replay expired; loaded the current session state.");
    },
    () => !state.connected || !state.sessions.some((session) => session.id === sessionId));
}

async function refreshSelectedSession() {
  if (!state.session) return;
  try {
    state.session = await api(`${API_ROOT}/sessions/${encodeURIComponent(state.session.id)}`);
    await loadSessions();
    renderAgent();
  } catch (error) {
    toast(errorMessage(error), "error");
  }
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
    toast(`Guard decision sent: ${decision}`);
  } catch (error) {
    closeDialog(byId("guard-dialog"));
    state.guard = null;
    if (error instanceof ApiError && [404, 409].includes(error.status)) {
      toast("That approval is no longer pending; session state was refreshed.", "error");
      await refreshSelectedSession();
    } else toast(errorMessage(error), "error");
  }
}

async function reviewGuardFile() {
  if (!state.guard) return;
  try {
    const response = await api(`${API_ROOT}/sessions/${encodeURIComponent(state.guard.sessionId)}/approvals/${encodeURIComponent(state.guard.approval.id)}/review-file`);
    byId("guard-review").textContent = response.content || "";
    byId("guard-review").hidden = false;
  } catch (error) {
    toast(errorMessage(error), "error");
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

function clearEditor() {
  state.file = null;
  byId("file-editor").value = "";
  byId("file-editor").disabled = true;
  byId("editor-heading").textContent = "Editor";
  byId("save-file-button").disabled = true;
  byId("editor-assist-button").disabled = true;
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
    const open = element("button", "", `${entry.type === "directory" ? "▸" : "·"} ${entry.name}`);
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
  byId("editor-assist-button").disabled = state.file.dirty || !supports("editor_assist");
}

async function loadFile(path) {
  if (state.file && state.file.dirty && state.file.path !== path &&
      !window.confirm("Discard the unsaved editor draft and open another file?")) return;
  try {
    const response = await api(`${API_ROOT}/files?path=${wirePath(path)}`);
    state.file = { ...response, dirty: false };
    byId("file-editor").value = response.content || "";
    byId("file-editor").disabled = false;
    byId("editor-heading").textContent = response.path;
    updateEditorMeta();
    renderDirectory();
  } catch (error) {
    toast(errorMessage(error), "error");
  }
}

async function saveFile() {
  if (!state.file || !state.file.dirty) return;
  const path = state.file.path;
  try {
    const response = await api(`${API_ROOT}/files?path=${wirePath(path)}`, {
      method: "PUT",
      body: { revision: state.file.revision, content: byId("file-editor").value },
    });
    state.file.revision = response.file.revision;
    state.file.content = byId("file-editor").value;
    state.file.dirty = false;
    updateEditorMeta();
    await loadDirectory(state.directory.path);
    toast(`Saved ${path}`);
  } catch (error) {
    if (error instanceof ApiError && error.code === "revision_conflict") {
      showConflict(`The server copy of ${path} changed. Your draft is still in the editor.`, () => loadFile(path));
    } else toast(errorMessage(error), "error");
  }
}

function openMutation(type, entry = null) {
  if ((type === "rename" || type === "delete") && openFileCoveredBy(entry) &&
      state.file.dirty && !window.confirm("Discard the unsaved editor draft before changing this target?")) {
    return;
  }
  state.mutation = { type, entry };
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
    description.textContent = "Create a directory only if the parent revision still matches.";
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
  if (type === "create-file" && response.file) await loadFile(response.file.path);
  else if (type === "delete" && openPath) clearEditor();
  else if (type === "rename" && openPath) {
    const suffix = openPath === entry.path ? "" : openPath.slice(entry.path.length);
    state.file.dirty = false;
    await loadFile(`${path}${suffix}`);
  }
  toast("Workspace action completed");
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
    provider: byId("chat-provider").value,
    model: byId("chat-model").value.trim(),
    api: byId("chat-api").value,
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
    toast("The assist proposal is ready in Jobs, but the editor changed, so it was not applied.", "error");
    return;
  }
  try {
    byId("file-editor").value = applyByteEdit(byId("file-editor").value, job.result.edit);
    state.file.dirty = true;
    updateEditorMeta();
    switchPanel("workspace-panel");
    byId("file-editor").focus();
    toast("AI proposal applied to the draft. Review it, then save explicitly.");
  } catch (error) {
    toast(errorMessage(error), "error");
  }
}

function bindEvents() {
  for (const button of document.querySelectorAll(".primary-nav button")) {
    button.addEventListener("click", () => switchPanel(button.dataset.panel));
  }
  for (const button of document.querySelectorAll(".dialog-cancel")) {
    button.addEventListener("click", () => closeDialog(button.closest("dialog")));
  }

  byId("theme-select").addEventListener("change", (event) => {
    document.documentElement.dataset.theme = event.target.value;
    storageSet(THEME_STORAGE_KEY, event.target.value);
  });
  byId("disconnect-button").addEventListener("click", () => disconnect());
  byId("auth-dialog").addEventListener("cancel", (event) => {
    if (!state.connected) event.preventDefault();
  });
  byId("guard-dialog").addEventListener("cancel", (event) => event.preventDefault());
  byId("auth-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    byId("auth-error").textContent = "";
    try {
      await connect(byId("token-input").value, byId("remember-token").checked);
    } catch (error) {
      byId("token-input").value = "";
      byId("auth-error").textContent = errorMessage(error);
      byId("token-input").focus();
    }
  });

  byId("refresh-settings-button").addEventListener("click", () => void refreshSettings().catch((error) => toast(errorMessage(error), "error")));
  byId("refresh-jobs-button").addEventListener("click", () => void refreshKnownJobs());
  byId("clear-finished-button").addEventListener("click", () => {
    for (const [id, job] of state.jobs) if (TERMINAL_STATES.has(job.state)) state.jobs.delete(id);
    renderJobs();
  });

  byId("goal-job-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    const operation = byId("goal-operation").value;
    if (!supports(operation)) return toast(`${operation} is not supported by this server`, "error");
    try {
      await submitJob(operation, optionalPayload({
        goal: byId("goal-input").value.trim(),
        provider: byId("goal-provider").value,
        model: byId("goal-model").value.trim(),
        api: byId("goal-api").value,
      }));
      byId("goal-input").value = "";
    } catch (error) { toast(errorMessage(error), "error"); }
  });

  byId("image-job-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    try {
      await submitJob("image", optionalPayload({
        prompt: byId("image-prompt").value.trim(),
        provider: byId("image-provider").value,
        model: byId("image-model").value.trim(),
        size: byId("image-size").value.trim(),
        aspect: byId("image-aspect").value.trim(),
        quality: byId("image-quality").value.trim(),
        format: byId("image-format").value.trim(),
      }));
      byId("image-prompt").value = "";
    } catch (error) { toast(errorMessage(error), "error"); }
  });

  byId("new-thread-button").addEventListener("click", () => openDialog(byId("new-thread-dialog")));
  byId("refresh-threads-button").addEventListener("click", () => void loadThreads());
  byId("new-thread-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    try {
      const response = await api(`${API_ROOT}/chat/threads`, {
        method: "POST",
        body: optionalPayload({
          revision: 0,
          name: byId("thread-name-input").value.trim(),
          provider: byId("thread-provider").value,
          model: byId("thread-model").value.trim(),
        }),
      });
      closeDialog(byId("new-thread-dialog"));
      state.thread = response.thread;
      await loadThreads();
      renderChat();
    } catch (error) { toast(errorMessage(error), "error"); }
  });
  byId("chat-form").addEventListener("submit", (event) => {
    event.preventDefault();
    const text = byId("chat-input").value.trim();
    if (text) void sendChatMessage(text);
  });

  byId("new-agent-button").addEventListener("click", () => openDialog(byId("new-agent-dialog")));
  byId("refresh-agents-button").addEventListener("click", () => void loadSessions());
  byId("new-agent-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    try {
      const response = await api(`${API_ROOT}/sessions/agent`, {
        method: "POST",
        body: optionalPayload({
          kind: "agent",
          provider: byId("agent-provider").value,
          model: byId("agent-model").value.trim(),
          api: byId("agent-api").value,
          task_mode: byId("agent-task-mode").value,
          permission_mode: byId("agent-permission").value,
        }),
      });
      closeDialog(byId("new-agent-dialog"));
      await loadSessions();
      await selectSession(response.session.id);
    } catch (error) { toast(errorMessage(error), "error"); }
  });
  byId("agent-turn-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!state.session) return;
    try {
      const response = await api(`${API_ROOT}/sessions/${encodeURIComponent(state.session.id)}/turns`, {
        method: "POST",
        body: { text: byId("agent-turn-input").value.trim() },
      });
      state.session.turn_id = response.turn_id;
      state.session.status = "running";
      byId("agent-turn-input").value = "";
      renderAgent();
    } catch (error) { toast(errorMessage(error), "error"); }
  });
  byId("cancel-turn-button").addEventListener("click", async () => {
    if (!state.session || !state.session.turn_id) return;
    try {
      await api(`${API_ROOT}/sessions/${encodeURIComponent(state.session.id)}/turns/${encodeURIComponent(state.session.turn_id)}/cancel`, { method: "POST" });
      toast("Agent turn cancellation requested");
    } catch (error) { toast(errorMessage(error), "error"); }
  });
  byId("close-agent-button").addEventListener("click", async () => {
    if (!state.session || !window.confirm("Close this interactive agent session?")) return;
    const id = state.session.id;
    try {
      await api(`${API_ROOT}/sessions/${encodeURIComponent(id)}`, { method: "DELETE" });
      stopStream(`session:${id}`);
      state.session = null;
      await loadSessions();
      renderAgent();
    } catch (error) { toast(errorMessage(error), "error"); }
  });
  byId("guard-review-button").addEventListener("click", () => void reviewGuardFile());
  byId("guard-allow-button").addEventListener("click", () => void resolveGuard("allow"));
  byId("guard-deny-button").addEventListener("click", () => void resolveGuard("deny"));

  byId("workspace-review-button").addEventListener("click", () => void loadWorkspaceReview());
  byId("refresh-directory-button").addEventListener("click", () => void loadDirectory(state.directory.path));
  byId("create-file-button").addEventListener("click", () => openMutation("create-file"));
  byId("create-directory-button").addEventListener("click", () => openMutation("mkdir"));
  byId("mutation-form").addEventListener("submit", (event) => {
    event.preventDefault();
    void applyMutation().catch((error) => {
      if (error instanceof ApiError && error.code === "revision_conflict") {
        closeDialog(byId("mutation-dialog"));
        showConflict("The workspace changed after this directory was reviewed. Reload the directory before retrying.",
          () => loadDirectory(state.directory.path));
      } else toast(errorMessage(error), "error");
    });
  });
  byId("file-editor").addEventListener("input", () => {
    if (!state.file) return;
    state.file.dirty = byId("file-editor").value !== state.file.content;
    updateEditorMeta();
  });
  byId("save-file-button").addEventListener("click", () => void saveFile());
  byId("editor-assist-button").addEventListener("click", () => openDialog(byId("assist-dialog")));
  byId("assist-form").addEventListener("submit", (event) => {
    event.preventDefault();
    void requestAssist(byId("assist-instruction").value.trim(), byId("assist-selection").checked)
      .catch((error) => toast(errorMessage(error), "error"));
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
  const theme = storageGet(THEME_STORAGE_KEY);
  if (["auto", "dark", "light"].includes(theme)) {
    document.documentElement.dataset.theme = theme;
    byId("theme-select").value = theme;
  }
  renderJobs();
  renderChat();
  renderAgent();
  const remembered = storageGet(TOKEN_STORAGE_KEY);
  if (remembered) {
    byId("remember-token").checked = true;
    try {
      await connect(remembered, true);
      return;
    } catch (error) {
      storageSet(TOKEN_STORAGE_KEY, "");
      byId("auth-error").textContent = errorMessage(error);
    }
  }
  openDialog(byId("auth-dialog"));
}

void boot();
