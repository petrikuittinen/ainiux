// Optional real-browser regression. AINIUX_TEST_BROWSER points to an existing
// Chromium executable. Uses its DevTools pipe; no npm packages are needed.
import assert from "node:assert/strict";
import test from "node:test";
import { createServer } from "node:http";
import { readFile, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawn } from "node:child_process";

test("web controller selectors, per-thread saves, workspace settings and history", {
  skip: !process.env.AINIUX_TEST_BROWSER, timeout: 30000,
}, async () => {
  const fields = [
    { id: "temperature", optional: true, choices: [], hint: "0–2" },
    { id: "reasoning", optional: false, choices: ["auto", "low", "high"] },
    { id: "stream", optional: false, choices: ["on", "off"] },
  ];
  const threads = [1, 2].map((id) => ({ id, revision: 1, name: `Thread ${id}`,
    provider: "openrouter", model: `model-${id}`, settings_fields: fields,
    settings: { temperature: id === 1 ? "0.2" : "0.8", reasoning: id === 1 ? "high" : "low", stream: "on" },
    messages: [], message_count: 0 }));
  let nextThreadId = 3, createdProviders = [];
  let chatUploads = [], chatJobs = [], appendedMessages = [];
  let workspace = { provider: "openrouter", model: "workspace-model", revision: "1",
    settings_fields: fields, settings: { temperature: "0.5", reasoning: "low", stream: "on" } };
  let session = null, assistRequest = null;
  const jobs = new Map(), models = Array.from({ length: 350 }, (_, i) => `model-${349 - i}`);
  const assets = new Map();
  const index = await readFile(new URL("../../../src/web/index.html", import.meta.url), "utf8");
  assets.set("/ui/", ["text/html", index]);
  for (const name of ["app-v29.js", "selector-v3.js", "highlight-v5.js", "syntax-v4.js", "image-options-v1.js", "video-options-v3.js", "editor-history-v2.js", "editor-indentation-v1.js", "app-v23.css"]) {
    assets.set(`/ui/assets/${name}`, [name.endsWith("css") ? "text/css" : "text/javascript",
      await readFile(new URL(`../../../src/web/${name.endsWith("css") ? "css" : "js"}/${name}`, import.meta.url))]);
  }
  const server = createServer(async (req, res) => {
    try {
      const url = new URL(req.url, "http://localhost"), path = url.pathname;
      if (assets.has(path)) { const [type, body] = assets.get(path); res.setHeader("Content-Type", type); res.end(body); return; }
      let raw = ""; for await (const part of req) raw += part;
      let body = {};
      if (raw && String(req.headers["content-type"] || "").includes("json")) body = JSON.parse(raw);
      const send = (value) => { res.setHeader("Content-Type", "application/json"); res.end(JSON.stringify(value)); };
      if (path.endsWith("/events")) { res.setHeader("Content-Type", "text/event-stream"); res.write(": connected\n\n"); return; }
      if (path.endsWith("/capabilities")) return send({ providers: ["none", "deepseek", "openrouter", "openai"], operations: ["models", "chat", "chat_threads", "sessions", "dired", "files", "editor_assist"] });
      if (path.endsWith("/status")) return send({ status: "ready" });
      if (path.endsWith("/images/catalog")) return send({ models: [] });
      if (path.endsWith("/videos/catalog")) return send({ models: [] });
      if (path.endsWith("/workspace/settings")) {
        if (req.method === "POST") workspace = { ...workspace, ...body, settings: { ...workspace.settings, ...body.settings }, revision: String(Number(workspace.revision) + 1) };
        return send(workspace);
      }
      if (path.endsWith("/chat/threads")) {
        if (req.method === "POST") {
          createdProviders.push(body.provider || "");
          const thread = { id: nextThreadId++, revision: 1, name: body.name || "New chat",
            provider: body.provider || "none", model: body.model || "", settings_fields: fields,
            settings: { temperature: "", reasoning: "auto", stream: "on" }, messages: [], message_count: 0 };
          threads.push(thread); res.statusCode = 201; return send({ thread });
        }
        return send({ threads });
      }
      if (path.endsWith("/chat/inputs") && req.method === "POST") {
        const id = `chat_input_${chatUploads.length + 1}`;
        chatUploads.push({ id, bytes: raw.length });
        res.statusCode = 201;
        return send({ id, kind: "text", mime_type: "text/plain", display_name: "notes.txt",
          converted: false, byte_size: raw.length, expires_at: "2099-01-01T00:00:00Z" });
      }
      const messageMatch = path.match(/\/chat\/threads\/(\d+)\/messages$/);
      if (messageMatch && req.method === "POST") {
        const thread = threads.find((item) => item.id === Number(messageMatch[1]));
        appendedMessages.push(body);
        thread.revision += 1;
        thread.messages.push(...(body.messages || []));
        thread.message_count = thread.messages.length;
        return send({ thread: { id: thread.id, revision: thread.revision,
          message_count: thread.message_count } });
      }
      if (path.endsWith("/jobs/chat") && req.method === "POST") {
        chatJobs.push(body);
        const job = { id: `chat-${jobs.size}`, operation: "chat", state: "succeeded",
          result: { content: "ok", provider: body.provider || "openrouter", model: body.model || "" } };
        jobs.set(job.id, job); res.statusCode = 202; return send({ job });
      }
      const abandonMatch = path.match(/\/chat\/threads\/(\d+)\/abandon$/);
      if (abandonMatch) {
        const index = threads.findIndex((item) => item.id === Number(abandonMatch[1]));
        const thread = threads[index];
        if (!thread || body.revision !== thread.revision) {
          res.statusCode = thread ? 409 : 404; return send({ error: { message: "abandon conflict" } });
        }
        if (thread.messages.some((item) => item.role === "user" || item.role === "assistant")) {
          return send({ id: thread.id, deleted: false, reason: "not_empty" });
        }
        threads.splice(index, 1); return send({ id: thread.id, deleted: true });
      }
      const threadMatch = path.match(/\/chat\/threads\/(\d+)(\/settings)?$/);
      if (threadMatch) {
        const thread = threads.find((item) => item.id === Number(threadMatch[1]));
        if (req.method === "POST") {
          assert.equal(body.revision, thread.revision);
          Object.assign(thread, { ...body, revision: thread.revision + 1, settings: { ...thread.settings, ...body.settings } });
        }
        return send({ thread });
      }
      if (path.endsWith("/jobs/models")) {
        const listedModels = body.provider === "deepseek"
          ? ["deepseek-chat", "deepseek-reasoner", "deepseek-coder"] : models;
        const job = { id: `models-${jobs.size}`, state: "succeeded",
          result: { models: listedModels } };
        jobs.set(job.id, job); return send({ job });
      }
      if (path.endsWith("/jobs/editor-assist")) {
        assistRequest = body;
        const job = { id: "assist", operation: "editor-assist", state: "succeeded",
          result: { edit: { start: 0, length: 5, replacement: "HELLO" } } };
        jobs.set(job.id, job); return send({ job });
      }
      if (path.includes("/jobs/")) return send(jobs.get(path.split("/").at(-1)) || {});
      if (path.endsWith("/sessions/agent")) {
        session = { id: "session-1", status: "ready", task_mode: "plan", permission_mode: "smart", turn_id: null, event_cursor: 0 };
        return send({ session: { ...session, ...workspace } });
      }
      if (path.endsWith("/sessions")) return send(session ? [{ ...session, ...workspace }] : []);
      if (path.endsWith("/history")) return send({ turn_id: "", messages: [{ seq: 1, role: "assistant", content: "Earlier project work" }], before: 0 });
      if (path.includes("/sessions/")) return send({ ...session, ...workspace, reasoning: workspace.settings.reasoning });
      if (path.endsWith("/dired")) return send({ path: ".", revision: "r1", entries: [
        { type: "file", name: "notes.txt", path: "notes.txt", size: 5, revision: "r1" },
        { type: "file", name: "sample.js", path: "sample.js", size: 27, revision: "r1" },
      ] });
      if (path.endsWith("/files")) {
        const javascript = url.searchParams.get("path") === "sample.js";
        return send(javascript
          ? { path: "sample.js", revision: "r1", content: "if (ready) {\n  call();\n}\n", editable: true }
          : { path: "notes.txt", revision: "r1", content: "hello", editable: true });
      }
      res.statusCode = 404; send({ error: { message: `No mock route ${path}` } });
    } catch (error) { res.statusCode = 500; res.end(JSON.stringify({ error: { message: error.message } })); }
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const profile = await mkdtemp(join(tmpdir(), "ainiux-browser-test-"));
  const browser = spawn(process.env.AINIUX_TEST_BROWSER, ["--headless", "--no-sandbox", "--disable-gpu", "--remote-debugging-pipe", `--user-data-dir=${profile}`],
    { stdio: ["ignore", "ignore", "ignore", "pipe", "pipe"] });
  let nextId = 0, buffer = ""; const pending = new Map(), errors = [];
  const command = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
    const id = ++nextId; pending.set(id, { resolve, reject });
    browser.stdio[3].write(JSON.stringify({ id, method, params, sessionId }) + "\0");
  });
  browser.stdio[4].on("data", (chunk) => {
    buffer += chunk.toString();
    for (;;) {
      const end = buffer.indexOf("\0"); if (end < 0) break;
      const event = JSON.parse(buffer.slice(0, end)); buffer = buffer.slice(end + 1);
      if (event.id && pending.has(event.id)) {
        const handler = pending.get(event.id); pending.delete(event.id);
        if (event.error) handler.reject(new Error(event.error.message)); else handler.resolve(event.result);
      } else if (event.method === "Runtime.exceptionThrown") errors.push(event.params.exceptionDetails.exception?.description || event.params.exceptionDetails.text);
    }
  });
  try {
    const target = await command("Target.createTarget", { url: "about:blank" });
    const attached = await command("Target.attachToTarget", { targetId: target.targetId, flatten: true });
    const sid = attached.sessionId;
    await command("Runtime.enable", {}, sid); await command("Page.enable", {}, sid);
    await command("Page.addScriptToEvaluateOnNewDocument", { source: 'localStorage.setItem("ainiux.controller.token.v1", "test-controller")' }, sid);
    const evaluate = async (expression) => {
      const response = await command("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true }, sid);
      if (response.exceptionDetails) throw new Error(response.exceptionDetails.exception?.description || expression);
      return response.result?.value;
    };
    const wait = async (expression) => {
      for (let i = 0; i < 200; ++i) {
        assert.deepEqual(errors, []);
        if (await evaluate(expression)) return;
        await new Promise((resolve) => setTimeout(resolve, 25));
      }
      const debug = await evaluate(`({
        listButtons: document.querySelectorAll("#thread-list .list-button").length,
        threadText: document.querySelector("#thread-list")?.textContent,
        provider: document.querySelector("#chat-provider")?.value,
        model: document.querySelector("#chat-model")?.value,
        picker: document.querySelector(".model-picker")?.open || false,
        heading: document.querySelector("#conversation-heading")?.textContent,
        toast: document.querySelector("#toast-region")?.textContent,
        authOpen: document.querySelector("#auth-dialog")?.open || false,
        token: localStorage.getItem("ainiux.controller.token.v1"),
        scripts: [...document.scripts].map((node) => node.src),
      })`);
      throw new Error(`Timed out: ${expression} debug=${JSON.stringify(debug)} created=${JSON.stringify(createdProviders)} errors=${JSON.stringify(errors)}`);
    };
    const click = (selector) => evaluate(`document.querySelector(${JSON.stringify(selector)}).click()`);
    const key = async (value, modifiers = 0, code) => {
      await command("Input.dispatchKeyEvent", { type: "keyDown", key: value, text: value === "Enter" ? "\r" : undefined,
        modifiers, code, windowsVirtualKeyCode: value === "Enter" ? 13 : value === "Escape" ? 27 : undefined }, sid);
      await command("Input.dispatchKeyEvent", { type: "keyUp", key: value, modifiers, code }, sid);
    };
    const screenshot = async (name) => {
      if (!process.env.AINIUX_TEST_SCREENSHOTS) return;
      await evaluate('document.querySelector("#toast-region").replaceChildren()');
      const shot = await command("Page.captureScreenshot", { format: "png" }, sid);
      await writeFile(join(process.env.AINIUX_TEST_SCREENSHOTS, name + ".png"), Buffer.from(shot.data, "base64"));
    };
    const checkToolbar = async (panel, mobile = false, compact = true) => {
      const result = await evaluate(`(() => {
        const panel = document.querySelector("#${panel}-panel");
        const toolbar = panel.querySelector("${panel === "chat" ? ".conversation-bar" : ".agent-toolbar"}");
        const controls = [...toolbar.querySelectorAll("a, select, button")].filter(node => node.getClientRects().length);
        const rectangles = controls.map(node => node.getBoundingClientRect());
        const within = rectangles.every(r => r.left >= 0 && r.right <= innerWidth && r.top >= 0 && r.bottom <= innerHeight);
        const overlaps = rectangles.some((a, i) => rectangles.slice(i + 1).some(b => a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom));
        return { within, overlaps, height: toolbar.getBoundingClientRect().height,
          inputCount: toolbar.querySelectorAll("input").length,
          selectBottoms: controls.filter(node => node.tagName === "SELECT" || node.tagName === "BUTTON").map(node => node.getBoundingClientRect().bottom),
          hasCatalogNoise: /models|list unavailable|Choose…|Model settings/.test(toolbar.innerText),
          visibleKbd: [...toolbar.querySelectorAll("kbd")].filter((node) => node.getClientRects().length).length,
          selects: controls.filter(node => node.tagName === "SELECT").map(node => node.id) };
      })()`);
      assert.equal(result.within, true); assert.equal(result.overlaps, false);
      assert.equal(result.inputCount, 0); assert.equal(result.hasCatalogNoise, false);
      assert.equal(result.visibleKbd, 0);
      if (compact && !mobile && result.selectBottoms.length > 1) {
        const baseline = Math.max(...result.selectBottoms) - Math.min(...result.selectBottoms);
        assert.ok(baseline <= 8, `toolbar controls share a baseline ${baseline}`);
      }
      if (compact) {
        assert.ok(result.height < (mobile ? 180 : 110), `compact ${panel} toolbar: ${result.height}`);
      }
      assert.deepEqual(result.selects, panel === "chat" ? [] : ["agent-task-mode", "agent-permission"]);
    };
    await command("Emulation.setDeviceMetricsOverride", { width: 1440, height: 1000, deviceScaleFactor: 1, mobile: false }, sid);
    await command("Page.navigate", { url: `http://127.0.0.1:${server.address().port}/ui/` }, sid);
    await wait('document.querySelectorAll("#thread-list .list-button").length === 3 && document.querySelector("#chat-provider").value === "openrouter"');
    assert.deepEqual(createdProviders, ["openrouter"]);
    assert.equal(await evaluate('document.querySelector(".model-picker")?.open || false'), false);
    assert.equal(await evaluate('document.querySelector("#chat-provider").disabled'), false);
    assert.equal(await evaluate('document.querySelector("#chat-model").disabled'), false);
    assert.equal(await evaluate('document.querySelector("#chat-provider-link").textContent'), "Provider: openrouter");
    assert.equal(await evaluate('document.querySelector("#chat-model").value'), "model-1");
    await click("#new-thread-button");
    for (let i = 0; i < 100 && createdProviders.length < 2; ++i) {
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
    assert.deepEqual(createdProviders, ["openrouter", "openrouter"]);
    assert.equal(await evaluate('document.querySelector(".model-picker")?.open || false'), false);
    assert.equal(await evaluate('document.querySelector("#chat-provider").value'), "openrouter");
    await new Promise((resolve) => setTimeout(resolve, 50));
    const composerLayout = await evaluate(`(() => {
      const input = document.querySelector("#chat-input").getBoundingClientRect();
      const send = document.querySelector("#chat-send").getBoundingClientRect();
      const attach = document.querySelector("#chat-attach-button").getBoundingClientRect();
      return {
        aligned: send.left >= input.right - 2 && attach.left >= input.right - 2 &&
          send.top < input.bottom && send.bottom > input.top,
        sendEnabled: !document.querySelector("#chat-send").disabled,
        input: { top: input.top, right: input.right, bottom: input.bottom, height: input.height },
        send: { top: send.top, left: send.left, bottom: send.bottom, height: send.height },
        attach: { top: attach.top, left: attach.left, bottom: attach.bottom },
      };
    })()`);
    assert.ok(composerLayout.aligned, JSON.stringify(composerLayout));
    assert.equal(composerLayout.sendEnabled, true);
    await evaluate(`(() => {
      const file = new File(["hold me"], "sticky.txt", { type: "text/plain" });
      const input = document.querySelector("#chat-attach-files");
      const transfer = new DataTransfer();
      transfer.items.add(file);
      input.files = transfer.files;
      input.dispatchEvent(new Event("change"));
    })()`);
    await wait('document.querySelectorAll(".chat-attach-chip").length === 1');
    await click("#new-thread-button");
    await wait('document.querySelectorAll(".chat-attach-chip").length === 0 && document.querySelector("#chat-send") && !document.querySelector("#chat-send").disabled');
    await evaluate(`(() => {
      const file = new File(["hello pdf"], "notes.txt", { type: "text/plain" });
      const input = document.querySelector("#chat-attach-files");
      const transfer = new DataTransfer();
      transfer.items.add(file);
      input.files = transfer.files;
      input.dispatchEvent(new Event("change"));
    })()`);
    await wait('document.querySelectorAll(".chat-attach-chip").length === 1');
    await click("#chat-send");
    for (let i = 0; i < 100 && !chatJobs.length; ++i) {
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
    assert.ok(chatUploads.length >= 1, "attach uploads the local file");
    assert.ok(appendedMessages.some((body) => Array.isArray(body.messages) &&
      body.messages.some((message) => Array.isArray(message.input_ids) && message.input_ids.length)),
      "send appends the attachment identifiers");
    assert.ok(chatJobs.some((body) => body.thread_id), "chat job uses the stored thread");
    assert.equal(await evaluate('document.querySelector("#chat-cycle-reasoning-button").textContent'), "Reasoning: auto");
    assert.equal(await evaluate('document.querySelector("#chat-form").querySelectorAll("select:not([hidden]), input:not([hidden]), .model-identity").length'), 0);
    await wait('document.querySelector("#chat-model-status").textContent === ""');
    await checkToolbar("chat");
    assert.equal(await evaluate('document.querySelector("#chat-provider-link").textContent'), "Provider: openrouter");
    assert.equal(await evaluate('getComputedStyle(document.querySelector("#chat-provider-link")).fontSize === getComputedStyle(document.body).fontSize'), true);
    await screenshot("chat-desktop");
    await click("#chat-model-link");
    assert.equal(await evaluate('document.activeElement === document.querySelector("#chat-model + button")'), true);
    await click('[data-panel="chat-panel"]');
    await evaluate('document.querySelector("#chat-input").focus()');
    await key("p", 1, "KeyP");
    await wait('document.querySelector(".model-picker").open');
    assert.equal(await evaluate('document.querySelector("#model-picker-title").textContent'), "Choose provider");
    await key("Escape");
    assert.equal(await evaluate('document.activeElement.id'), "chat-input");
    await key("m", 1, "KeyM");
    await wait('document.querySelectorAll(".picker-option").length === 350');
    await key(".");
    assert.equal(await evaluate('document.querySelector(".picker-option").textContent'), "model-0");
    await key("/");
    assert.equal(await evaluate('document.activeElement.type'), "search", await evaluate('document.activeElement.outerHTML'));
    await command("Input.insertText", { text: "model-12" }, sid); await key("Enter");
    const first = await evaluate('document.querySelector(".picker-option[aria-selected=true]").textContent');
    await key("/"); await key("Enter");
    const second = await evaluate('document.querySelector(".picker-option[aria-selected=true]").textContent');
    assert.notEqual(first, second); assert.ok(second.includes("model-12"));
    await key("Enter");
    await wait(`document.querySelector("#chat-model").value === ${JSON.stringify(second)} && !document.querySelector(".model-picker").open`);
    await wait('document.querySelector("#chat-settings-save-status").textContent === "Saved"');
    assert.ok(threads.some((thread) => thread.model === second));
    await click('[data-panel="settings-panel"]');
    const settingsLayout = await evaluate(`(() => {
      const cards = [...document.querySelectorAll(".settings-grid > section")];
      const pickerHeights = [document.querySelector("#chat-provider + button"),
        document.querySelector("#chat-model + button"), document.querySelector("#workspace-provider + button"),
        document.querySelector("#workspace-model + button"), document.querySelector("#theme-select")]
        .map(node => node.getBoundingClientRect().height);
      return { headings: cards.map(card => card.querySelector("h2").textContent), pickerHeights,
        visibleModelInputs: [...document.querySelectorAll("#settings-panel input")]
          .filter(node => node.getClientRects().length && node.id.endsWith("-model")).length,
        modelCountNoise: /[0-9]+ models?/.test(document.querySelector("#settings-panel").innerText) };
    })()`);
    assert.deepEqual(settingsLayout.headings.slice(0, 5),
      ["Current chat", "Workspace agent & editor", "Appearance", "Status", "Capabilities"]);
    assert.equal(settingsLayout.visibleModelInputs, 0);
    assert.equal(settingsLayout.modelCountNoise, false);
    assert.ok(Math.max(...settingsLayout.pickerHeights) - Math.min(...settingsLayout.pickerHeights) <= 1,
      "settings pickers and theme control have equal heights");
    await evaluate('const input = document.querySelector("#chat-settings-fields input"); input.value = "0.7"; input.dispatchEvent(new Event("change"));');
    await wait('document.querySelector("#chat-settings-save-status").textContent === "Saved"');
    assert.ok(threads.some((thread) => thread.settings.temperature === "0.7"));
    await click('[data-panel="chat-panel"]'); await click("#thread-list .thread-item:nth-child(2) .list-button");
    await wait('document.querySelector("#chat-model").value === "model-2"');
    assert.equal(await evaluate('document.querySelector("#chat-reasoning").value'), "low");
    await click("#thread-list .thread-item:last-child .list-button");
    await wait(`document.querySelector("#chat-model").value === ${JSON.stringify(second)}`);
    assert.equal(await evaluate('document.querySelector("#chat-settings-fields input").value'), "0.7");
    await click('[data-panel="agent-panel"]');
    await wait('document.querySelector("#agent-events").textContent.includes("Earlier project work")');
    assert.equal(await evaluate('document.querySelector("#agent-model").value'), "workspace-model");
    await checkToolbar("agent"); await screenshot("agent-desktop");
    await evaluate('document.querySelector("#agent-turn-input").focus()');
    await key("m", 1, "KeyM");
    await wait('document.querySelector(".model-picker").open');
    assert.ok(await evaluate('document.querySelector("#model-picker-title").textContent.includes("openrouter")'));
    await key("Escape");
    await click("#agent-provider-link");
    assert.equal(await evaluate('document.activeElement === document.querySelector("#workspace-provider + button")'), true);
    await key("p", 1, "KeyP");
    await wait('document.querySelector(".model-picker").open');
    await key("Escape");
    await evaluate('const model = document.querySelector("#workspace-model"); model.value = "editor/agent-v2"; model.dispatchEvent(new Event("change"));');
    await wait('document.querySelector("#workspace-settings-save-status").textContent === "Saved"');
    assert.equal(workspace.model, "editor/agent-v2");
    await click('[data-panel="workspace-panel"]');
    await evaluate(`{ const editor = document.querySelector("#file-editor");
      editor.value = "old line\\n".repeat(200); editor.disabled = false;
      editor.setSelectionRange(editor.value.length, editor.value.length);
      editor.scrollTop = editor.scrollHeight; editor.scrollLeft = 40; editor.disabled = true; }`);
    await click(".file-main button");
    await wait('!document.querySelector("#editor-assist-button").disabled');
    await click("#edit-file-button");
    assert.deepEqual(await evaluate(`(() => { const editor = document.querySelector("#file-editor");
      return { start: editor.selectionStart, end: editor.selectionEnd,
        scrollTop: editor.scrollTop, scrollLeft: editor.scrollLeft }; })()`),
      { start: 0, end: 0, scrollTop: 0, scrollLeft: 0 });
    await evaluate('document.querySelector("#file-editor").setSelectionRange(5, 5)');
    await command("Input.insertText", { text: "!" }, sid);
    await wait('document.querySelector("#file-editor").value === "hello!" && !document.querySelector("#undo-file-button").disabled');
    await key("u", 2, "KeyU");
    assert.equal(await evaluate('document.querySelector("#file-editor").value'), "hello");
    await key("y", 2, "KeyY");
    assert.equal(await evaluate('document.querySelector("#file-editor").value'), "hello!");
    await key("z", 1, "KeyZ");
    assert.equal(await evaluate('document.querySelector("#file-editor").value'), "hello");
    await key("y", 1, "KeyY");
    assert.equal(await evaluate('document.querySelector("#file-editor").value'), "hello!");
    await click("#undo-file-button");
    assert.equal(await evaluate('document.querySelector("#file-editor").value'), "hello");
    assert.equal(await evaluate('document.querySelector("#editor-reformat-button").disabled'), true);
    await evaluate('[...document.querySelectorAll(".file-main button")].find(node => node.textContent.includes("sample.js")).click()');
    await wait('document.querySelector("#editor-heading").textContent === "sample.js"');
    assert.deepEqual(await evaluate(`(() => ({
      width: document.querySelector("#editor-indent-width").value,
      style: document.querySelector("#editor-indent-style").value,
      editorTab: getComputedStyle(document.querySelector("#file-editor")).tabSize,
      overlayTab: getComputedStyle(document.querySelector("#file-edit-highlight")).tabSize,
      viewerTab: getComputedStyle(document.querySelector("#file-highlight")).tabSize,
    }))()`), { width: "2", style: "spaces", editorTab: "2", overlayTab: "2", viewerTab: "2" });
    await click("#edit-file-button");
    await evaluate(`{ const width = document.querySelector("#editor-indent-width");
      width.value = "4"; width.dispatchEvent(new Event("change"));
      const style = document.querySelector("#editor-indent-style");
      style.value = "tab"; style.dispatchEvent(new Event("change"));
      const editor = document.querySelector("#file-editor"); editor.setSelectionRange(0, 0); }`);
    await key("Tab", 0, "Tab");
    assert.ok(await evaluate('document.querySelector("#file-editor").value.startsWith("\\t")'));
    await click("#undo-file-button");
    await evaluate(`{ const width = document.querySelector("#editor-indent-width");
      width.value = "2"; width.dispatchEvent(new Event("change"));
      const style = document.querySelector("#editor-indent-style");
      style.value = "spaces"; style.dispatchEvent(new Event("change")); }`);
    await evaluate(`{ const editor = document.querySelector("#file-editor");
      editor.focus();
      const start = editor.value.indexOf("  call"); editor.setSelectionRange(start, start + 8, "forward");
      editor.dispatchEvent(new Event("select")); }`);
    await wait('document.querySelector("#editor-reformat-button").textContent === "Reformat selection"');
    await key("Tab", 0, "Tab");
    assert.ok(await evaluate('document.querySelector("#file-editor").value.includes("    call")'));
    await key("Tab", 8, "Tab");
    assert.ok(await evaluate('document.querySelector("#file-editor").value.includes("  call")'));
    await evaluate(`{ const editor = document.querySelector("#file-editor"); editor.focus();
      editor.setSelectionRange(0, editor.value.length); }`);
    await command("Input.insertText", { text: "if (ready) {\ncall();\n}\nafter();" }, sid);
    await evaluate(`{ const editor = document.querySelector("#file-editor");
      editor.setSelectionRange(0, editor.value.indexOf("after"), "forward"); }`);
    await wait('document.querySelector("#editor-reformat-button").textContent === "Reformat selection"');
    await click("#editor-reformat-button");
    assert.equal(await evaluate('document.querySelector("#file-editor").value'), "if (ready) {\n  call();\n}\nafter();");
    assert.equal(await evaluate('document.querySelector("#file-edit-highlight").textContent'),
      await evaluate('document.querySelector("#file-editor").value'));
    await click("#undo-file-button");
    assert.equal(await evaluate('document.querySelector("#file-editor").value'), "if (ready) {\ncall();\n}\nafter();");
    await evaluate('const editor = document.querySelector("#file-editor"); editor.setSelectionRange(0, 0)');
    await wait('document.querySelector("#editor-reformat-button").textContent === "Reformat file"');
    await click("#editor-reformat-button");
    assert.equal(await evaluate('document.querySelector("#file-editor").value'), "if (ready) {\n  call();\n}\nafter();");
    const editorToolbar = await evaluate(`(() => { const bar = document.querySelector(".editor-toolbar");
      const controls = [...bar.querySelectorAll("input, select, button")].filter(node => node.getClientRects().length);
      const rectangles = controls.map(node => node.getBoundingClientRect());
      return { within: rectangles.every(r => r.left >= 0 && r.right <= innerWidth),
        overlaps: rectangles.some((a, i) => rectangles.slice(i + 1).some(b =>
          a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom)) }; })()`);
    assert.equal(editorToolbar.within, true); assert.equal(editorToolbar.overlaps, false);
    await evaluate('[...document.querySelectorAll(".file-main button")].find(node => node.textContent.includes("notes.txt")).click()');
    await wait('document.querySelector("#confirm-dialog")?.open || document.querySelector("#editor-heading").textContent === "notes.txt"');
    if (await evaluate('document.querySelector("#confirm-dialog")?.open === true')) {
      await click("#confirm-submit");
    }
    await wait('document.querySelector("#editor-heading").textContent === "notes.txt" && document.querySelector("#file-editor").value === "hello"');
    await click("#editor-assist-button");
    await evaluate('document.querySelector("#assist-instruction").value = "Improve this"; document.querySelector("#assist-form").requestSubmit()');
    for (let i = 0; i < 100 && !assistRequest; ++i) {
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
    assert.ok(assistRequest); assert.equal(assistRequest.provider, undefined); assert.equal(assistRequest.model, undefined);
    await wait('document.querySelector("#file-editor").value === "HELLO"');
    await click("#undo-file-button");
    assert.equal(await evaluate('document.querySelector("#file-editor").value'), "hello");
    // Physical-key fallback on Option layouts, and no Ctrl/AltGr or composing interception.
    await click('[data-panel="workspace-panel"]');
    assert.equal(await evaluate('document.querySelector("dialog[open]")?.id || ""'), "");
    await key("µ", 1, "KeyM");
    await wait('document.querySelector(".model-picker").open'); await key("Escape");
    await key("m", 3, "KeyM");
    assert.equal(await evaluate('document.querySelector(".model-picker").open'), false);
    await evaluate('window.dispatchEvent(new KeyboardEvent("keydown", {key:"m", code:"KeyM", altKey:true, isComposing:true}))');
    assert.equal(await evaluate('document.querySelector(".model-picker").open'), false);
    const longModel = "vendor/deepseek-v4-flash-vision-experimental-with-a-very-long-model-name";
    await click('[data-panel="settings-panel"]');
    await evaluate(`{ const input = document.querySelector("#workspace-model"); input.value = ${JSON.stringify(longModel)}; input.dispatchEvent(new Event("change")); }`);
    await wait(`document.querySelector("#agent-model").value === ${JSON.stringify(longModel)} && !document.querySelector("#workspace-model").disabled`);
    await click('[data-panel="chat-panel"]'); await click("#chat-model-link");
    await evaluate(`{ const input = document.querySelector("#chat-model"); input.value = ${JSON.stringify(longModel)}; input.dispatchEvent(new Event("change")); }`);
    await wait(`document.querySelector("#chat-model-link").textContent === ${JSON.stringify(`Model: ${longModel}`)}`);
    // Wide and tablet layouts retain the same controls with long model names.
    for (const width of [1440, 800]) {
      await command("Emulation.setDeviceMetricsOverride", { width, height: 1000, deviceScaleFactor: 1, mobile: false }, sid);
      for (const panel of ["chat", "agent"]) {
        await click(`[data-panel="${panel}-panel"]`); await checkToolbar(panel, width === 800, false);
        await screenshot(`${panel}-${width === 1440 ? "desktop" : "tablet"}`);
      }
    }
    await command("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 1, mobile: true }, sid);
    await click('[data-panel="agent-panel"]'); await checkToolbar("agent", true, false); await screenshot("agent-mobile");
    await click('[data-panel="chat-panel"]'); await checkToolbar("chat", true, false); await screenshot("chat-mobile");
    await key("m", 1, "KeyM");
    assert.ok(await evaluate('document.querySelector(".model-picker").getBoundingClientRect().width <= innerWidth'));
    await key("Escape");
    await evaluate('document.querySelector("#chat-model").disabled = true');
    await key("m", 1, "KeyM");
    assert.equal(await evaluate('document.querySelector(".model-picker").open'), false);
    assert.deepEqual(errors, []);
  } finally {
    browser.kill();
    await new Promise((resolve) => browser.once("exit", resolve));
    server.closeAllConnections(); await new Promise((resolve) => server.close(resolve));
    await rm(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
});
