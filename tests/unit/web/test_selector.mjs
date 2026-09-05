import assert from "node:assert/strict";
import test from "node:test";
import { SelectorState, createSelector } from "../../../src/web/js/selector-v3.js";

test("large catalogs navigate by rows, pages and endpoints without wrapping", () => {
  const state = new SelectorState(Array.from({ length: 350 }, (_, i) => `model-${i}`));
  state.move("ArrowUp"); assert.equal(state.selected, 0);
  state.move("PageDown", 17); assert.equal(state.selected, 17);
  state.move("End"); state.move("ArrowDown"); assert.equal(state.selected, 349);
  state.move("PageUp", 17); assert.equal(state.selected, 332);
  state.move("Home"); assert.equal(state.selected, 0);
  const empty = new SelectorState(); empty.move("End"); empty.sort();
  assert.equal(empty.selected, 0); assert.equal(empty.find("x"), false);
});

test("search repeats, wraps, preserves the full list, and reports missing terms", () => {
  const state = new SelectorState(["Alpha", "google/gemini", "openai/gpt", "GOOGLE/gemini-pro"]);
  assert.equal(state.find("gemini"), true); assert.equal(state.selected, 1);
  state.find(); assert.equal(state.selected, 3);
  state.find(); assert.equal(state.selected, 1);
  assert.equal(state.items.length, 4);
  assert.equal(state.find("missing"), false); assert.equal(state.selected, 1);
  assert.equal(state.find(""), false);
});

test("sort toggles original order, preserves identity, and keeps stable equal labels", () => {
  const source = [{ value: "z", label: "Zulu" }, { value: "b", label: "Alpha" }, { value: "a", label: "Alpha" }];
  const state = new SelectorState(source, "a"); state.sort();
  assert.deepEqual(state.items.map((item) => item.value), ["b", "a", "z"]);
  assert.equal(state.items[state.selected].value, "a");
  state.sort(); assert.deepEqual(state.items, source); assert.equal(state.selected, 2);
  assert.deepEqual(source.map((item) => item.value), ["z", "b", "a"]);
});

// A small DOM/event harness: tests exercise actual picker handlers, not their
// implementation copied into assertions. No browser library is required.
function fixture() {
  class Node {
    constructor(tag) {
      this.tagName = tag.toUpperCase(); this.children = []; this.listeners = {};
      this.attributes = {}; this.hidden = false; this.isConnected = true;
      this.clientHeight = 320; this.offsetHeight = 32; this.textContent = "";
    }
    append(...nodes) { this.children.push(...nodes); }
    replaceChildren(...nodes) { this.children = nodes; }
    setAttribute(key, value) { this.attributes[key] = value; }
    removeAttribute(key) { delete this.attributes[key]; }
    addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
    dispatch(type, values = {}) {
      const event = { preventDefault() { this.defaultPrevented = true; }, ...values };
      for (const fn of this.listeners[type] || []) fn(event);
      return event;
    }
    focus() { doc.activeElement = this; }
    scrollIntoView() { this.scrolled = true; }
    showModal() { this.open = true; }
    close() { this.open = false; this.dispatch("close"); }
    get firstElementChild() { return this.children[0]; }
  }
  const doc = { createElement: (tag) => new Node(tag), body: new Node("body"), activeElement: new Node("button") };
  const picker = createSelector(doc);
  const dialog = doc.body.children[0], card = dialog.children[0];
  const [title, toolbar, search, list, status, hint, manual, actions] = card.children;
  return { doc, picker, dialog, toolbar, search, list, status, manual, actions };
}

test("keyboard search, empty repeat, sort, selection and focus restoration", () => {
  const f = fixture(), opener = f.doc.activeElement; let selected;
  f.picker.open({ key: "models", title: "Models", items: ["z", "gpt-one", "gpt-two", "a"], onSelect: (v) => selected = v });
  assert.equal(f.doc.activeElement, f.list);
  f.list.dispatch("keydown", { key: "/" }); f.search.children[0].value = "gpt";
  f.search.dispatch("submit"); assert.equal(f.list.attributes["aria-activedescendant"], "picker-option-1");
  f.list.dispatch("keydown", { key: "/" }); f.search.children[0].value = "";
  f.search.dispatch("submit"); assert.equal(f.list.attributes["aria-activedescendant"], "picker-option-2");
  f.list.dispatch("keydown", { key: "." });
  f.list.dispatch("keydown", { key: "Enter" }); assert.equal(selected, "gpt-two");
  assert.equal(f.doc.activeElement, opener);
});

test("manual model punctuation, cancelled search, catalog refresh and touch selection", () => {
  const f = fixture(); let selected;
  f.picker.open({ key: "models:p", title: "Models", manual: true, value: "custom/x.1", items: [], onSelect: (v) => selected = v });
  assert.equal(f.manual.hidden, true);
  assert.equal(f.actions.children[0].textContent, "Enter model manually");
  f.actions.children[0].dispatch("click");
  assert.equal(f.manual.hidden, false);
  f.manual.children[0].value = "custom/x.1";
  assert.equal(f.manual.children[0].dispatch("keydown", { key: "." }).defaultPrevented, undefined);
  f.manual.dispatch("submit"); assert.equal(selected, "custom/x.1");
  f.picker.open({ key: "models:p", items: ["z", "a"], value: "z", onSelect: (v) => selected = v });
  f.toolbar.children[0].dispatch("click");
  f.picker.update("models:other", ["stale"]); assert.equal(f.list.children.length, 2);
  f.picker.update("models:p", ["z", "a", "b"]); assert.equal(f.list.children[2].textContent, "z");
  f.list.dispatch("keydown", { key: "/" });
  assert.equal(f.dialog.dispatch("cancel").defaultPrevented, true);
  assert.equal(f.search.hidden, true); assert.equal(f.dialog.open, true);
  f.list.children[0].dispatch("click"); assert.equal(selected, "a");
});

test("manual entry rejects an empty value and a sole fetched model is automatic", () => {
  const f = fixture(); let selected = "";
  f.picker.open({ key: "models:p", manual: true, autoSelectOnly: true, items: [],
    onSelect: (value) => selected = value });
  f.actions.children[0].dispatch("click");
  assert.equal(f.manual.children[1].disabled, true);
  f.manual.dispatch("submit"); assert.equal(selected, ""); assert.equal(f.dialog.open, true);
  f.picker.update("models:p", ["only-model"], "");
  assert.equal(selected, "only-model"); assert.equal(f.dialog.open, false);
});

test("a delayed close event cannot reset a newly reopened picker", () => {
  const f = fixture(); let selected;
  f.picker.open({ key: "provider", items: ["p"] });
  f.dialog.open = false; // Native dialog close events are queued, not synchronous.
  f.picker.open({ key: "models:p", items: ["m"], onSelect: (value) => selected = value });
  f.dialog.dispatch("close");
  assert.equal(f.doc.activeElement, f.list);
  f.list.dispatch("keydown", { key: "Enter" });
  assert.equal(selected, "m");
});
