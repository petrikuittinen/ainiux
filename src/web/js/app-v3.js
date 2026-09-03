const API_ROOT = "/ainiux/v1";
const TOKEN_STORAGE_KEY = "ainiux.controller.token.v1";
const THEME_STORAGE_KEY = "ainiux.ui.theme.v1";
const TERMINAL_STATES = new Set(["succeeded", "failed", "cancelled"]);

const state = {
  token: "",
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
  chatPending: false,
  chatMetrics: new Map(),
  sessions: [],
  session: null,
  agentLogs: new Map(),
  agentClock: null,
  agentClockTimer: null,
  modelCatalogs: new Map(),
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

function formatDuration(value) {
  if (value === null || value === undefined || value === "") return "";
  const milliseconds = Number(value);
  if (!Number.isFinite(milliseconds) || milliseconds < 0) return "";
  return `${new Intl.NumberFormat().format(Math.round(milliseconds))} ms`;
}

function metricText(metrics, context = null, activeElapsed = null) {
  const source = metrics && typeof metrics === "object" ? metrics : {};
  const pieces = [];
  const used = context && context.used_tokens !== undefined
    ? context.used_tokens : source.context_used_tokens;
  const windowTokens = context && context.window_tokens !== undefined
    ? context.window_tokens : source.context_window_tokens;
  const usedText = formatTokenCount(used, true);
  const windowText = formatTokenCount(windowTokens);
  if (usedText) pieces.push(`Context ${usedText}${windowText ? ` / ${windowText}` : ""} tok`);
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
  let response;
  try {
    response = await fetch(path, {
      method: options.method || "GET",
      headers,
      body,
      signal: options.signal,
      credentials: "omit",
      cache: "no-store",
      referrerPolicy: "no-referrer",
    });
  } catch (error) {
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

function modelControls() {
  return [
    { providerId: "chat-provider", modelId: "chat-model", listId: "chat-model-list", statusId: "chat-model-status", apiId: "chat-api" },
    { providerId: "goal-provider", modelId: "goal-model", listId: "goal-model-list", statusId: "goal-model-status", apiId: "goal-api" },
    { providerId: "thread-provider", modelId: "thread-model", listId: "thread-model-list", statusId: "thread-model-status", apiId: null },
    { providerId: "agent-provider", modelId: "agent-model", listId: "agent-model-list", statusId: "agent-model-status", apiId: "agent-api" },
  ];
}

function modelCatalogKey(control) {
  return JSON.stringify([
    byId(control.providerId).value,
    control.apiId ? byId(control.apiId).value : "",
  ]);
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
  else if (catalog.state === "ready") status.textContent = `${models.length} model${models.length === 1 ? "" : "s"}`;
  else status.textContent = "Model list unavailable; enter one manually.";
  const input = byId(control.modelId);
  if (catalog && catalog.state === "ready" && models.length === 1 && !input.value.trim()) {
    input.value = models[0];
  }
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
        api: control.apiId ? byId(control.apiId).value : "",
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
  refreshModelControls();
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

function setConnectionStatus(label, className) {
  byId("connection-badge").textContent = label;
  byId("connection-badge").className = `status-badge ${className}`;
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
  const wasConnected = state.connected;
  state.connected = false;
  if (wasConnected) {
    stopAllStreams();
    resetLoadingModelCatalogs();
  }
  setConnectionStatus("Reconnecting…", "reconnecting");
  byId("disconnect-button").hidden = false;
  applyCapabilities();
  if (wasConnected) toast("Connection lost. Ainiux will reconnect automatically.", "error");
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
      if (threadId) await loadThread(threadId);
    })());
  }
  if (supports("sessions")) {
    tasks.push((async () => {
      await loadSessions();
      if (sessionId && state.sessions.some((item) => item.id === sessionId)) {
        await selectSession(sessionId);
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
  await restoreBrowserState();
  toast(reconnected ? "Reconnected to the Ainiux control server" :
    "Connected to the Ainiux control server");
}

async function attemptReconnect() {
  state.reconnectTimer = null;
  if (!state.authenticated || state.connected || !state.token) return;
  setConnectionStatus("Reconnecting…", "reconnecting");
  try {
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
  state.authenticated = previouslyValidated;
  try {
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
    state.authenticated = false;
    throw error;
  }
  await markConnected(false);
  return true;
}

function forgetAuthentication(message = "") {
  cancelReconnect();
  stopAllStreams();
  stopAgentClock();
  state.token = "";
  state.authenticated = false;
  state.connected = false;
  state.reconnectAttempt = 0;
  state.capabilities = null;
  state.status = null;
  state.thread = null;
  state.chatMetrics.clear();
  state.session = null;
  state.modelCatalogs.clear();
  state.file = null;
  state.guard = null;
  storageSet(TOKEN_STORAGE_KEY, "");
  byId("token-input").value = "";
  setConnectionStatus("Offline", "offline");
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

function invalidateAuthentication() {
  forgetAuthentication("Invalid authentication");
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
    const details = [formatDate(thread.modified_at), `${thread.message_count || 0} messages`];
    if (thread.provider || thread.model) details.push(`${thread.provider || "provider"} / ${thread.model || "default"}`);
    button.append(element("strong", "", thread.name || "New chat"),
      element("small", "", details.join(" · ")));
    button.addEventListener("click", () => void loadThread(thread.id));
    list.append(button);
  }
}

function renderChat() {
  const messages = byId("chat-messages");
  if (!state.thread) {
    byId("conversation-heading").textContent = "Select a thread";
    byId("thread-meta").textContent = "";
    byId("chat-metrics").textContent = "";
    setEmpty(messages, "Choose or create a thread.");
    byId("chat-input").disabled = true;
    byId("chat-send").disabled = true;
    return;
  }
  byId("conversation-heading").textContent = state.thread.name || "New chat";
  byId("thread-meta").textContent = `${formatDate(state.thread.modified_at)} · ${state.thread.message_count || state.thread.messages.length} messages`;
  byId("chat-metrics").textContent = metricText(state.chatMetrics.get(state.thread.id));
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
    const messages = state.thread.messages;
    state.thread = { ...state.thread, ...appended.thread, messages };
    state.thread.messages.push({ role: "user", content: text });
    if (!state.thread.name || state.thread.name === "New chat") {
      const firstLine = text.split(/\r?\n/, 1)[0].trim();
      state.thread.name = [...firstLine].slice(0, 40).join("") || "New chat";
    }
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
    state.chatMetrics.set(context.threadId, job.result.metrics || null);
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
  target.textContent = metricText(metrics, state.session.context, activeElapsed);
}

function renderAgent() {
  renderSessions();
  const events = byId("agent-events");
  if (!state.session) {
    byId("agent-console-heading").textContent = "No active session";
    byId("agent-meta").textContent = "";
    byId("agent-metrics").textContent = "";
    setEmpty(events, "Create or select an agent session.");
    byId("agent-turn-input").disabled = true;
    byId("agent-turn-submit").disabled = true;
    byId("cancel-turn-button").hidden = true;
    byId("close-agent-button").hidden = true;
    stopAgentClock();
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
      appendMetrics(card, data.metrics);
      events.append(card);
    }
    events.scrollTop = events.scrollHeight;
  }
  const ready = session.status === "ready" && !session.turn_id;
  byId("agent-turn-input").disabled = !ready;
  byId("agent-turn-submit").disabled = !ready;
  byId("cancel-turn-button").hidden = !session.turn_id;
  byId("close-agent-button").hidden = false;
  syncAgentClock();
  renderAgentMetrics();
}

async function loadSessions() {
  try {
    const sessions = await api(`${API_ROOT}/sessions`);
    state.sessions = Array.isArray(sessions) ? sessions : [];
    if (state.session) {
      const updated = state.sessions.find((item) => item.id === state.session.id);
      if (updated) state.session = { ...state.session, ...updated };
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
    if (state.session && state.session.id !== sessionId) {
      stopStream(`session:${state.session.id}`);
      stopAgentClock();
    }
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
        if (event.type === "turn_started") {
          state.session.turn_id = event.turn_id;
          state.session.status = "running";
          state.session.active_elapsed_ms = 0;
        }
        if (event.type === "approval_required") state.session.status = "waiting_guard";
        if (event.type === "ready" || event.type === "session_created" || event.type === "session_closed") {
          state.session = event.data;
        }
        if (["turn_completed", "turn_failed", "approval_resolved"].includes(event.type)) {
          if (["turn_completed", "turn_failed"].includes(event.type)) {
            state.session.turn_id = null;
            state.session.status = "ready";
            state.session.active_elapsed_ms = null;
            if (event.data && event.data.metrics) state.session.last_turn_metrics = event.data.metrics;
          }
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
  for (const control of modelControls()) {
    byId(control.providerId).addEventListener("change", () => refreshModelControl(control));
    if (control.apiId) byId(control.apiId).addEventListener("change", () => refreshModelControl(control));
  }

  byId("theme-select").addEventListener("change", (event) => {
    document.documentElement.dataset.theme = event.target.value;
    storageSet(THEME_STORAGE_KEY, event.target.value);
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
      byId("thread-name-input").value = "";
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
  byId("chat-input").addEventListener("keydown", (event) => {
    if (event.key !== "Enter" || event.isComposing || event.shiftKey || event.altKey ||
        event.ctrlKey || event.metaKey) return;
    event.preventDefault();
    byId("chat-form").requestSubmit();
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
      state.session.active_elapsed_ms = 0;
      state.agentClock = {
        sessionId: state.session.id,
        turnId: response.turn_id,
        baseMs: 0,
        startedAt: performance.now(),
      };
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
}

void boot();
