// Keep these navigation semantics aligned with ui/text_selector.cpp.
export class SelectorState {
  constructor(items = [], value = "", search = "") {
    this.original = items.map((item) => typeof item === "string" ? { value: item, label: item } : item);
    this.items = [...this.original];
    this.selected = Math.max(0, this.items.findIndex((item) => item.value === value));
    this.sorted = false;
    this.search = search;
  }
  move(key, page = 10) {
    const last = Math.max(0, this.items.length - 1);
    const offsets = { ArrowUp: -1, ArrowDown: 1, PageUp: -page, PageDown: page };
    this.selected = key === "Home" ? 0 : key === "End" ? last :
      Math.max(0, Math.min(last, this.selected + (offsets[key] || 0)));
  }
  find(term = this.search) {
    if (!term) return false;
    this.search = term;
    const fold = (text) => text.replace(/[A-Z]/g, (ch) => ch.toLowerCase());
    for (let step = 1; step <= this.items.length; ++step) {
      const index = (this.selected + step) % this.items.length;
      if (fold(this.items[index].label).includes(fold(term))) {
        this.selected = index;
        return true;
      }
    }
    return false;
  }
  sort() {
    const current = this.items[this.selected];
    this.sorted = !this.sorted;
    this.items = [...this.original];
    if (this.sorted) this.items.sort((a, b) => a.label < b.label ? -1 : a.label > b.label ? 1 : 0);
    this.selected = Math.max(0, this.items.indexOf(current));
  }
}

export function createSelector(document) {
  const make = (tag, text = "") => {
    const node = document.createElement(tag); node.textContent = text; return node;
  };
  const button = (label) => { const node = make("button", label); node.type = "button"; return node; };
  const dialog = make("dialog"); dialog.className = "model-picker";
  dialog.setAttribute("aria-labelledby", "model-picker-title");
  const card = make("div"); card.className = "dialog-card";
  const title = make("h2"); title.id = "model-picker-title";
  const toolbar = make("div"); toolbar.className = "button-row";
  const sort = button("Sort A–Z (.)"), search = button("Search (/)"), next = button("Find next");
  toolbar.append(sort, search, next);
  const searchRow = make("form"); searchRow.hidden = true;
  const searchInput = make("input"); searchInput.type = "search";
  searchInput.setAttribute("aria-label", "Search names; empty repeats previous search");
  const find = button("Find"); find.type = "submit"; searchRow.append(searchInput, find);
  const list = make("div"); list.className = "picker-list"; list.tabIndex = 0;
  list.setAttribute("role", "listbox"); list.setAttribute("aria-label", "Available names");
  const status = make("p"); status.className = "field-hint"; status.setAttribute("role", "status");
  const hint = make("p", "↑ ↓ move · PgUp/PgDn page · Home/End · Enter select · Esc cancel");
  hint.className = "field-hint";
  const manualRow = make("form"), manual = make("input");
  manualRow.hidden = true;
  manual.maxLength = 512; manual.autocomplete = "off";
  manual.placeholder = "provider/model-name";
  manual.setAttribute("aria-label", "Model name (manual entry)");
  const use = button("Use model"); use.type = "submit"; use.disabled = true;
  manualRow.append(manual, use);
  const manualToggle = button("Enter model manually"); manualToggle.className = "ghost";
  const retry = button("Reload list"), cancel = button("Cancel"); cancel.className = "ghost";
  const actions = make("div"); actions.className = "button-row";
  actions.append(manualToggle, retry, cancel);
  card.append(title, toolbar, searchRow, list, status, hint, manualRow, actions);
  dialog.append(card); document.body.append(dialog);
  const terms = new Map();
  let nav = new SelectorState(), config = null, opener = null;
  const render = () => {
    list.replaceChildren();
    nav.items.forEach((item, index) => {
      const row = make("div", item.label); row.id = `picker-option-${index}`;
      row.className = "picker-option"; row.setAttribute("role", "option");
      row.setAttribute("aria-selected", String(index === nav.selected));
      row.addEventListener("click", () => commit(item.value)); list.append(row);
    });
    const row = list.children[nav.selected];
    if (row) { list.setAttribute("aria-activedescendant", row.id); row.scrollIntoView({ block: "nearest" }); }
    else list.removeAttribute("aria-activedescendant");
    sort.setAttribute("aria-pressed", String(nav.sorted));
    sort.textContent = nav.sorted ? "Original order (.)" : "Sort A–Z (.)";
    status.textContent = nav.items.length ? `${nav.selected + 1} / ${nav.items.length}` : config?.status || "No names available.";
  };
  const commit = (value) => { const callback = config?.onSelect; dialog.close(); if (callback) callback(value); };
  const beginSearch = () => { searchRow.hidden = false; searchInput.value = ""; searchInput.focus(); };
  const findNext = (term) => {
    const found = nav.find(term || nav.search); terms.set(config.key, nav.search);
    searchRow.hidden = true; render(); list.focus();
    status.textContent += found ? ` · ${nav.search}` : nav.search ? ` · No match for ${nav.search}` : " · Enter a search term first.";
  };
  search.addEventListener("click", beginSearch);
  next.addEventListener("click", () => findNext(nav.search));
  sort.addEventListener("click", () => { nav.sort(); render(); list.focus(); });
  searchRow.addEventListener("submit", (event) => { event.preventDefault(); findNext(searchInput.value); });
  manual.addEventListener("input", () => { use.disabled = !manual.value.trim(); });
  manualToggle.addEventListener("click", () => {
    manualRow.hidden = !manualRow.hidden;
    manualToggle.textContent = manualRow.hidden ? "Enter model manually" : "Hide manual entry";
    if (!manualRow.hidden) manual.focus();
  });
  manualRow.addEventListener("submit", (event) => {
    event.preventDefault();
    const value = manual.value.trim();
    if (value) commit(value);
  });
  cancel.addEventListener("click", () => dialog.close());
  retry.addEventListener("click", () => config?.onReload?.());
  dialog.addEventListener("cancel", (event) => {
    if (!searchRow.hidden) { event.preventDefault(); searchRow.hidden = true; list.focus(); }
  });
  dialog.addEventListener("close", () => {
    // A queued close event from the previous opening must not reset a reopened picker.
    if (dialog.open) return;
    config = null; if (opener?.isConnected) opener.focus();
  });
  // Never treat punctuation typed in an input as a selector command.
  list.addEventListener("keydown", (event) => {
    if (event.isComposing || event.ctrlKey || event.altKey || event.metaKey) return;
    if (["ArrowUp", "ArrowDown", "PageUp", "PageDown", "Home", "End"].includes(event.key)) {
      event.preventDefault();
      nav.move(event.key, Math.max(1, Math.floor(list.clientHeight / (list.firstElementChild?.offsetHeight || 32)))); render();
    } else if (event.key === ".") { event.preventDefault(); nav.sort(); render(); }
    else if (event.key === "/") { event.preventDefault(); beginSearch(); }
    else if (event.key === "Enter") { event.preventDefault(); if (nav.items.length) commit(nav.items[nav.selected].value); }
    else if (event.key.length === 1) {
      event.preventDefault(); const previous = nav.search; nav.find(event.key); nav.search = previous; render();
    }
  });
  return {
    open(options) {
      config = options; opener = document.activeElement;
      nav = new SelectorState(options.items, options.value, terms.get(options.key) || "");
      title.textContent = options.title; manualRow.hidden = true;
      manualToggle.hidden = !options.manual; manualToggle.textContent = "Enter model manually";
      retry.hidden = !options.onReload; manual.value = options.value || "";
      use.disabled = !manual.value.trim(); searchRow.hidden = true;
      dialog.showModal(); render(); list.focus();
      if (options.autoSelectOnly && nav.items.length === 1) commit(nav.items[0].value);
    },
    update(key, items, message) {
      if (!config || config.key !== key) return;
      const value = nav.items[nav.selected]?.value || config.value, sorted = nav.sorted;
      nav = new SelectorState(items, value, nav.search); if (sorted) nav.sort();
      config.status = message; render();
      if (config.autoSelectOnly && nav.items.length === 1) commit(nav.items[0].value);
    },
    close() { if (dialog.open) dialog.close(); },
  };
}
