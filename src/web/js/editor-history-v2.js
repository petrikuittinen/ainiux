const DEFAULT_HISTORY_LIMIT = 100;

function snapshot(value) {
  const text = String(value && value.value !== undefined ? value.value : "");
  const start = Math.max(0, Math.min(text.length, Number(value && value.selectionStart) || 0));
  const end = Math.max(start, Math.min(text.length, Number(value && value.selectionEnd) || start));
  const direction = value && ["forward", "backward"].includes(value.selectionDirection)
    ? value.selectionDirection : "none";
  return { value: text, selectionStart: start, selectionEnd: end, selectionDirection: direction };
}

function selection(value) {
  return {
    start: value.selectionStart,
    end: value.selectionEnd,
    direction: value.selectionDirection,
  };
}

function commonChange(before, after) {
  let start = 0;
  const shared = Math.min(before.value.length, after.value.length);
  while (start < shared && before.value[start] === after.value[start]) start += 1;
  let suffix = 0;
  while (suffix < shared - start &&
      before.value[before.value.length - suffix - 1] === after.value[after.value.length - suffix - 1]) {
    suffix += 1;
  }
  return {
    start,
    removed: before.value.slice(start, before.value.length - suffix),
    inserted: after.value.slice(start, after.value.length - suffix),
    beforeSelection: selection(before),
    afterSelection: selection(after),
  };
}

function applyChange(current, change, forward) {
  const removed = forward ? change.removed : change.inserted;
  const inserted = forward ? change.inserted : change.removed;
  if (current.value.slice(change.start, change.start + removed.length) !== removed) return null;
  const nextSelection = forward ? change.afterSelection : change.beforeSelection;
  return snapshot({
    value: current.value.slice(0, change.start) + inserted +
      current.value.slice(change.start + removed.length),
    selectionStart: nextSelection.start,
    selectionEnd: nextSelection.end,
    selectionDirection: nextSelection.direction,
  });
}

export function createEditorHistory(initial, limit = DEFAULT_HISTORY_LIMIT) {
  return {
    undo: [],
    redo: [],
    current: snapshot(initial),
    limit: Math.max(1, Number.isSafeInteger(limit) ? limit : DEFAULT_HISTORY_LIMIT),
  };
}

export function recordEditorChange(history, beforeValue, afterValue) {
  const before = snapshot(beforeValue || history.current);
  const after = snapshot(afterValue);
  history.current = after;
  if (before.value === after.value) return false;
  history.undo.push(commonChange(before, after));
  if (history.undo.length > history.limit) history.undo.splice(0, history.undo.length - history.limit);
  history.redo.length = 0;
  return true;
}

export function updateEditorHistorySelection(history, currentValue) {
  const current = snapshot(currentValue);
  if (history.current.value === current.value) history.current = current;
}

export function undoEditorChange(history, currentValue) {
  if (!history.undo.length) return null;
  const current = snapshot(currentValue || history.current);
  if (current.value !== history.current.value) return null;
  const change = history.undo[history.undo.length - 1];
  const result = applyChange(current, change, false);
  if (!result) return null;
  history.undo.pop();
  history.redo.push(change);
  history.current = result;
  return result;
}

export function redoEditorChange(history, currentValue) {
  if (!history.redo.length) return null;
  const current = snapshot(currentValue || history.current);
  if (current.value !== history.current.value) return null;
  const change = history.redo[history.redo.length - 1];
  const result = applyChange(current, change, true);
  if (!result) return null;
  history.redo.pop();
  history.undo.push(change);
  history.current = result;
  return result;
}

export function editorHistoryDirection(event) {
  if (!event || event.isComposing) return "";
  const code = String(event.code || "");
  const key = String(event.key || "").toLowerCase();
  const isKey = (letter) => code === `Key${letter.toUpperCase()}` || key === letter;
  const control = Boolean(event.ctrlKey);
  const command = Boolean(event.metaKey);
  const alt = Boolean(event.altKey);
  const shift = Boolean(event.shiftKey);
  const altGraph = typeof event.getModifierState === "function" && event.getModifierState("AltGraph");
  const primary = (control || command) && !(control && command) && !alt;
  const fallback = alt && !control && !command && !shift && !altGraph;
  if ((primary && !shift && (isKey("u") || isKey("z"))) ||
      (fallback && (isKey("u") || isKey("z")))) return "undo";
  if ((primary && ((!shift && isKey("y")) || (shift && isKey("z")))) ||
      (fallback && isKey("y"))) return "redo";
  return "";
}
