import assert from "node:assert/strict";
import test from "node:test";
import {
  createEditorHistory, editorHistoryDirection, recordEditorChange,
  redoEditorChange, undoEditorChange,
} from "../../../src/web/js/editor-history-v2.js";

const state = (value, selectionStart = value.length, selectionEnd = selectionStart,
  selectionDirection = "none") => ({ value, selectionStart, selectionEnd, selectionDirection });

test("editor history stores compact reversible edits and selections", () => {
  const initial = state("hello", 1);
  const history = createEditorHistory(initial);
  const edited = state("hello world", 11);
  assert.equal(recordEditorChange(history, initial, edited), true);
  assert.equal(history.undo[0].inserted, " world");
  assert.equal(history.undo[0].removed, "");
  assert.deepEqual(undoEditorChange(history, edited), initial);
  assert.deepEqual(redoEditorChange(history, initial), edited);
});

test("a new edit clears redo and history is bounded", () => {
  const history = createEditorHistory(state("a"), 2);
  recordEditorChange(history, state("a"), state("ab"));
  recordEditorChange(history, state("ab"), state("abc"));
  recordEditorChange(history, state("abc"), state("abcd"));
  assert.equal(history.undo.length, 2);
  assert.equal(undoEditorChange(history, state("abcd")).value, "abc");
  recordEditorChange(history, state("abc"), state("abc!"));
  assert.equal(history.redo.length, 0);
  assert.equal(redoEditorChange(history, state("abc!")), null);
  assert.equal(undoEditorChange(history, state("untracked")), null);
  assert.equal(history.undo.length, 2);
});

test("editor shortcuts include native bindings and Alt fallbacks without AltGr", () => {
  for (const event of [
    { key: "u", ctrlKey: true }, { key: "z", ctrlKey: true },
    { key: "z", metaKey: true }, { key: "u", altKey: true },
    { key: "Ω", code: "KeyZ", altKey: true },
  ]) assert.equal(editorHistoryDirection(event), "undo");
  for (const event of [
    { key: "y", ctrlKey: true }, { key: "z", metaKey: true, shiftKey: true },
    { key: "y", altKey: true },
  ]) assert.equal(editorHistoryDirection(event), "redo");
  assert.equal(editorHistoryDirection({ key: "z", ctrlKey: true, altKey: true,
    getModifierState: (name) => name === "AltGraph" }), "");
  assert.equal(editorHistoryDirection({ key: "z", altKey: true, isComposing: true }), "");
});
