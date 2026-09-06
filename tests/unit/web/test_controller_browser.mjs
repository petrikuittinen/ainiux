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
  let workspace = { provider: "openrouter", model: "workspace-model", revision: "1",
    settings_fields: fields, settings: { temperature: "0.5", reasoning: "low", stream: "on" } };
  let session = null, assistRequest = null;
  const jobs = new Map(), models = Array.from({ length: 350 }, (_, i) => `model-${349 - i}`);
  const assets = new Map();
  const index = await readFile(new URL("../../../src/web/index.html", import.meta.url), "utf8");
  assets.set("/ui/", ["text/html", index]);
  for (const name of ["app-v22.js", "selector-v3.js", "highlight-v4.js", "syntax-v3.js", "image-options-v1.js", "editor-history-v2.js", "app-v18.css"]) {
    assets.set(`/ui/assets/${name}`, [name.endsWith("css") ? "text/css" : "text/javascript",
      await readFile(new URL(`../../../src/web/${name.endsWith("css") ? "css" : "js"}/${name}`, import.meta.url))]);
  }
  const server = createServer(async (req, res) => {
    try {
      const url = new URL(req.url, "http://localhost"), path = url.pathname;
      if (assets.has(path)) { const [type, body] = assets.get(path); res.setHeader("Content-Type", type); res.end(body); return; }
      let raw = ""; for await (const part of req) raw += part;
      const body = raw ? JSON.parse(raw) : {};
      const send = (value) => { res.setHeader("Content-Type", "application/json"); res.end(JSON.stringify(value)); };
      if (path.endsWith("/events")) { res.setHeader("Content-Type", "text/event-stream"); res.write(": connected\n\n"); return; }
      if (path.endsWith("/capabilities")) return send({ providers: ["none", "deepseek", "openrouter", "openai"], operations: ["models", "chat", "chat_threads", "sessions", "dired", "files", "editor_assist"] });
      if (path.endsWith("/status")) return send({ status: "ready" });
      if (path.endsWith("/images/catalog")) return send({ models: [] });
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
      if (path.endsWith("/dired")) return send({ path: ".", revision: "r1", entries: [{ type: "file", name: "notes.txt", path: "notes.txt", size: 5, revision: "r1" }] });
      if (path.endsWith("/files")) return send({ path: "notes.txt", revision: "r1", content: "hello", editable: true });
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
      for (let i = 0; i < 100; ++i) {
        assert.deepEqual(errors, []);
        if (await evaluate(expression)) return;
        await new Promise((resolve) => setTimeout(resolve, 25));
      }
      throw new Error(`Timed out: ${expression}`);
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
    const checkToolbar = async (panel, mobile = false) => {
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
          visibleKbd: toolbar.querySelectorAll("kbd:not([hidden])").length,
          selects: controls.filter(node => node.tagName === "SELECT").map(node => node.id) };
      })()`);
      assert.equal(result.within, true); assert.equal(result.overlaps, false);
      assert.equal(result.inputCount, 0); assert.equal(result.hasCatalogNoise, false);
      assert.equal(result.visibleKbd, 0);
      assert.ok(Math.max(...result.selectBottoms) - Math.min(...result.selectBottoms) <= 1, "toolbar controls share a baseline");
      assert.ok(result.height < (mobile ? 180 : 110), `compact ${panel} toolbar: ${result.height}`);
      assert.deepEqual(result.selects, panel === "chat" ? [] : ["agent-task-mode", "agent-permission"]);
    };
    await command("Emulation.setDeviceMetricsOverride", { width: 1440, height: 1000, deviceScaleFactor: 1, mobile: false }, sid);
    await command("Page.navigate", { url: `http://127.0.0.1:${server.address().port}/ui/` }, sid);
    await wait('document.querySelector(".model-picker").open && document.querySelectorAll("#thread-list button").length === 3');
    assert.deepEqual(createdProviders, ["none"]);
    assert.equal(await evaluate('document.querySelector("#model-picker-title").textContent'), "Choose provider");
    assert.equal(await evaluate('document.querySelector("#chat-provider").disabled'), false);
    assert.equal(await evaluate('document.querySelector("#chat-model").disabled'), false);
    assert.equal(await evaluate('document.querySelector("#chat-provider-link").textContent'), "Provider: none");
    await evaluate('[...document.querySelectorAll(".picker-option")].find(node => node.textContent === "deepseek").click()');
    await wait('document.querySelector(".model-picker").open && document.querySelector("#model-picker-title").textContent.includes("deepseek") && document.querySelectorAll(".picker-option").length === 3');
    assert.equal(await evaluate('document.querySelector(".model-picker input[aria-label*=\"manual\"]").getClientRects().length'), 0);
    assert.equal(await evaluate('[...document.querySelectorAll(".model-picker button")].some(node => node.textContent === "Enter model manually")'), true);
    await evaluate('[...document.querySelectorAll(".picker-option")].find(node => node.textContent === "deepseek-reasoner").click()');
    await wait('document.querySelector("#chat-model").value === "deepseek-reasoner" && !document.querySelector(".model-picker").open');
    await wait('document.querySelector("#chat-settings-save-status").textContent === "Saved"');
    assert.equal(await evaluate('document.querySelector("#chat-cycle-reasoning-button").textContent'), "Reasoning: auto");
    assert.equal(await evaluate('document.querySelector("#chat-form").querySelectorAll("select:not([hidden]), input:not([hidden]), .model-identity").length'), 0);
    await click("#thread-list button");
    await wait('document.querySelector("#chat-model").value === "model-1"');
    await wait('document.querySelectorAll("#thread-list button").length === 2');
    assert.equal(threads.some((thread) => thread.model === "deepseek-reasoner"), false);
    assert.equal(await evaluate('document.querySelector("#chat-reasoning").value'), "high");
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
    assert.equal(threads[0].model, second);
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
    assert.equal(threads[0].settings.temperature, "0.7");
    await click('[data-panel="chat-panel"]'); await click("#thread-list button:nth-child(2)");
    await wait('document.querySelector("#chat-model").value === "model-2"');
    assert.equal(await evaluate('document.querySelector("#chat-reasoning").value'), "low");
    await click("#thread-list button:first-child");
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
    await click("#editor-assist-button");
    await evaluate('document.querySelector("#assist-instruction").value = "Improve this"; document.querySelector("#assist-form").requestSubmit()');
    await wait('!document.querySelector("#assist-dialog").open');
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
        await click(`[data-panel="${panel}-panel"]`); await checkToolbar(panel, width === 800);
        await screenshot(`${panel}-${width === 1440 ? "desktop" : "tablet"}`);
      }
    }
    await command("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 1, mobile: true }, sid);
    await click('[data-panel="agent-panel"]'); await checkToolbar("agent", true); await screenshot("agent-mobile");
    await click('[data-panel="chat-panel"]'); await checkToolbar("chat", true); await screenshot("chat-mobile");
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
