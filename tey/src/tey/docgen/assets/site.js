// The docs site's script. Three independent features, each of which starts
// only when its element is on the page:
//
//   #version-badge   the version switcher
//   #search          the search modal, on every documentation page
//   #sp-input        the full results page, search.html
//
// All of it is enhancement: without this script the pages still read.

(function () {
  "use strict";

  // ── Helpers ─────────────────────────────────────────────────────────

  // Markup below is built as text, so every value in it goes through this.
  function escapeHtml(value) {
    const entities = { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" };
    return String(value ?? "").replace(/[&<>"']/g, (char) => entities[char]);
  }

  async function fetchJson(url) {
    const response = await fetch(url, { cache: "no-store" });
    return response.json();
  }

  // ── Version switcher ────────────────────────────────────────────────
  // Replaces the static version badge with a <select> of every published
  // version of the same package.

  async function startVersionSwitcher(badge) {
    const { root, package: packageName, version: current, page } = badge.dataset;

    const data = await fetchJson(root + "versions.json");
    const versions = (data.versions || []).filter((v) => v.package === packageName);
    if (versions.length === 0) return;

    const select = document.createElement("select");
    select.className = "version-select";
    select.ariaLabel = "Documentation version";
    select.innerHTML = versions.map((v) => versionOption(v, current)).join("");
    select.addEventListener("change", () => {
      openVersion(`${root}${packageName}/${select.value}/`, page);
    });

    badge.replaceWith(select);
  }

  function versionOption(version, current) {
    const id = escapeHtml(version.id);
    const selected = version.id === current ? " selected" : "";
    return `<option value="${id}"${selected}>${escapeHtml(version.label)} ${id}</option>`;
  }

  // Opens `page` in the version at `versionUrl`. The same page may not exist
  // there (a renamed module, or a source that no longer parses), so this
  // falls back to that version's index rather than landing on a 404.
  async function openVersion(versionUrl, page) {
    let exists = false;
    try {
      const response = await fetch(versionUrl + page, { method: "HEAD", cache: "no-store" });
      exists = response.ok;
    } catch (error) {
      exists = false;
    }
    window.location.href = versionUrl + (exists ? page : "index.html");
  }

  // ── Search index ────────────────────────────────────────────────────

  // Returns a function that fetches `<baseUrl>search.json` on its first call
  // and answers every later call from that one request. A failed fetch is an
  // empty index, so search finds nothing instead of breaking.
  function createIndexLoader(baseUrl) {
    let request = null;
    return function loadIndex() {
      if (!request) {
        request = fetchJson(baseUrl + "search.json")
          .then((data) => data.entries || [])
          .catch(() => []);
      }
      return request;
    };
  }

  // ── Search ranking ──────────────────────────────────────────────────
  // Rank, don't just filter: a name hit outranks a summary hit, and an exact
  // type outranks a mention in a signature.
  //
  // Each row is a field of an entry and the points a query word earns for
  // matching it exactly, at its start, or anywhere inside it. A word counts
  // once, for the best field it matches.
  const FIELD_POINTS = [
    // field        exact  prefix  inside
    ["name",          100,     50,     20],
    ["qualifiedName",  90,     30,     30],
    ["types",          60,     25,     25],
    ["signatures",     10,     10,     10],
    ["summary",         5,      5,      5],
  ];

  // An arrow in the query means the reader is looking for a function.
  const ARROW_BONUS_FOR_FUNCTION = 80;
  const ARROW_PENALTY_OTHERWISE = -300;

  // The text of one field, lowercased. `types` and `signatures` are lists.
  function fieldText(entry, field) {
    const value = entry[field] || "";
    const text = Array.isArray(value) ? value.join(" ") : value;
    return text.toLowerCase();
  }

  function wordScore(word, texts) {
    let best = 0;
    for (const [field, exact, prefix, inside] of FIELD_POINTS) {
      const text = texts[field];
      let points = 0;
      if (text === word) points = exact;
      else if (text.startsWith(word)) points = prefix;
      else if (text.includes(word)) points = inside;
      best = Math.max(best, points);
    }
    return best;
  }

  // The entry's score for the query words, or -1 when any word matches
  // nowhere: every word must hit.
  function rank(entry, words) {
    const texts = {};
    for (const [field] of FIELD_POINTS) texts[field] = fieldText(entry, field);

    let total = 0;
    for (const word of words) {
      const score = wordScore(word, texts);
      if (score === 0) return -1;
      total += score;
    }
    if (words.includes("->")) {
      total += entry.kind === "function" ? ARROW_BONUS_FOR_FUNCTION : ARROW_PENALTY_OTHERWISE;
    }
    return total;
  }

  // The best `limit` entries for `query`, best first.
  function search(entries, query, limit) {
    const words = query.toLowerCase().split(" ").filter((word) => word !== "");
    const matches = [];
    for (const entry of entries) {
      const score = rank(entry, words);
      if (score >= 0) matches.push({ entry, score });
    }
    matches.sort((a, b) => b.score - a.score);
    return matches.slice(0, limit).map((match) => match.entry);
  }

  // ── Result rows ─────────────────────────────────────────────────────

  // The module or type an entry is defined on, not its full qualified name:
  // "Enumerable" for Enumerable.map, "[X]" for [X].map. Empty when the entry
  // has no owner to show.
  function ownerOf(entry) {
    const qualified = entry.qualifiedName;
    if (!qualified || qualified === entry.name) return "";
    const lastDot = qualified.lastIndexOf(".");
    return lastDot === -1 ? qualified : qualified.slice(0, lastDot);
  }

  // One result as a link: kind, name, owner, then its signature.
  //
  //   baseUrl      what the entry's page path is relative to
  //   className    the row's class; the modal and the page style it apart
  //   withSummary  the results page shows the summary under the signature;
  //                the modal's compact rows show it only in place of a
  //                missing signature
  function resultRow(entry, { baseUrl, className, withSummary }) {
    const anchor = entry.anchor ? "#" + entry.anchor : "";
    const href = `${baseUrl}${entry.urlPath}.html${anchor}`;
    const owner = ownerOf(entry);
    const signature = (entry.signatures && entry.signatures[0]) || "";
    const summary = entry.summary || "";

    const ownerHtml = owner ? `<span class="search-qual">${escapeHtml(owner)}</span>` : "";
    const detail = withSummary ? signature : signature || summary;
    const summaryHtml = withSummary && summary
      ? `<span class="search-summary">${escapeHtml(summary)}</span>`
      : "";

    return `
      <a class="${className}" href="${escapeHtml(href)}">
        <span class="search-head">
          <span class="search-kind search-kind-${escapeHtml(entry.kind)}">${escapeHtml(entry.kind)}</span>
          <span class="search-name">${escapeHtml(entry.name)}</span>
          ${ownerHtml}
        </span>
        <span class="search-sig">${escapeHtml(detail)}</span>
        ${summaryHtml}
      </a>`;
  }

  // ── Search modal ────────────────────────────────────────────────────
  // Opened by the header's search button, "/" or Cmd/Ctrl+K. It is built
  // here rather than in the page so it never appears without the script
  // that drives it.

  const MODAL_RESULT_LIMIT = 50;
  const EXAMPLE_QUERIES = ["map", "Integer", "String?", "String -> String -> String", "FS.File"];

  function startSearchModal(trigger) {
    const { root, package: packageName, version, page } = trigger.dataset;
    const baseUrl = `${root}${packageName}/${version}/`;
    const loadIndex = createIndexLoader(baseUrl);
    // The search index's path for the page we are on: "fs" for fs.html.
    // The version's index page is not a source page, so it has none.
    const currentPath = page === "index.html" ? "" : page.replace(/\.html$/, "");

    document.body.insertAdjacentHTML("beforeend", `
      <div class="search-modal" hidden>
        <div class="search-modal-panel">
          <input type="search" autocomplete="off"
                 placeholder="Search ${escapeHtml(packageName)} ${escapeHtml(version)}">
          <div class="search-modal-results"></div>
        </div>
      </div>`);
    const modal = document.body.lastElementChild;
    const input = modal.querySelector("input");
    const results = modal.querySelector(".search-modal-results");

    function row(entry) {
      return resultRow(entry, { baseUrl, className: "search-hit", withSummary: false });
    }

    function rows() {
      return Array.from(results.querySelectorAll(".search-hit"));
    }

    // What the modal shows before anything is typed: what a search can
    // answer, a few example queries, and what is documented on this page.
    function idleHtml(entries) {
      const chips = EXAMPLE_QUERIES
        .map((query) => `<button class="search-chip">${escapeHtml(query)}</button>`)
        .join("");
      const onThisPage = entries.filter((entry) => currentPath && entry.urlPath === currentPath).slice(0, 4);
      const onThisPageHtml = onThisPage.length === 0 ? "" : `
        <div class="search-local">
          <p class="search-hint-title">On this page</p>
          ${onThisPage.map(row).join("")}
        </div>`;

      return `
        <div class="search-hint">
          <p class="search-hint-title">Search by name or type.</p>
          <p class="search-hint-body">A name finds what it is called; a type finds everything that uses it.</p>
          <div class="search-chips">${chips}</div>
        </div>
        ${onThisPageHtml}`;
    }

    function resultsHtml(entries, query) {
      const found = search(entries, query, MODAL_RESULT_LIMIT);
      if (found.length === 0) return `<div class="search-empty">No matches</div>`;
      return found.map(row).join("");
    }

    function footerHtml(query) {
      const queryString = query ? "?q=" + encodeURIComponent(query) : "";
      const href = `${baseUrl}search.html${queryString}`;
      return `
        <div class="search-modal-footer">
          <a href="${escapeHtml(href)}">Open full search results</a>
        </div>`;
    }

    async function render() {
      const entries = await loadIndex();
      const query = input.value.trim();
      const body = query ? resultsHtml(entries, query) : idleHtml(entries);
      results.innerHTML = body + footerHtml(query);
      if (query) selectRow(0);
    }

    // ── Keyboard selection ──

    function selectedIndex() {
      return rows().findIndex((item) => item.classList.contains("selected"));
    }

    function selectRow(index) {
      const all = rows();
      if (all.length === 0) return;
      all.forEach((item, i) => item.classList.toggle("selected", i === index));
      all[index].scrollIntoView({ block: "nearest" });
    }

    // Moves the selection one row down (+1) or up (-1), wrapping at the
    // ends. With nothing selected, down starts at the first row and up at
    // the last.
    function moveSelection(step) {
      const count = rows().length;
      if (count === 0) return;
      const current = selectedIndex();
      let next = current + step;
      if (current === -1) next = step > 0 ? 0 : count - 1;
      selectRow((next + count) % count);
    }

    function openSelected() {
      const selected = rows()[selectedIndex()];
      if (selected) window.location.href = selected.href;
    }

    // ── Opening and closing ──

    function open() {
      modal.hidden = false;
      input.value = "";
      input.focus();
      render();
    }

    function close() {
      modal.hidden = true;
      trigger.focus();
    }

    function isTypingElsewhere(event) {
      const target = event.target;
      return target.isContentEditable || target.tagName === "INPUT" || target.tagName === "TEXTAREA";
    }

    // ── Events ──

    trigger.addEventListener("click", open);
    input.addEventListener("input", render);

    input.addEventListener("keydown", (event) => {
      switch (event.key) {
        case "Escape":
          close();
          break;
        case "ArrowDown":
          event.preventDefault();
          moveSelection(+1);
          break;
        case "ArrowUp":
          event.preventDefault();
          moveSelection(-1);
          break;
        case "Enter":
          openSelected();
          break;
      }
    });

    modal.addEventListener("click", (event) => {
      const chip = event.target.closest(".search-chip");
      if (chip) {
        input.value = chip.textContent;
        input.focus();
        render();
        return;
      }
      // A click on the dimmed backdrop, outside the panel.
      if (event.target === modal) close();
    });

    document.addEventListener("keydown", (event) => {
      const slash = event.key === "/" && modal.hidden && !isTypingElsewhere(event);
      const commandK = (event.metaKey || event.ctrlKey) && event.key === "k";
      if (slash || commandK) {
        event.preventDefault();
        open();
      }
    });
  }

  // ── Full results page (search.html) ─────────────────────────────────
  // The modal links here with ?q=<query>. The query lives in the URL, so a
  // results page is something to link to rather than a state to recreate.

  const PAGE_RESULT_LIMIT = 200;

  function startSearchPage(input, results) {
    const loadIndex = createIndexLoader("");

    function row(entry) {
      return resultRow(entry, { baseUrl: "", className: "search-page-hit", withSummary: true });
    }

    function writeQueryToUrl(query) {
      const url = new URL(window.location.href);
      if (query) url.searchParams.set("q", query);
      else url.searchParams.delete("q");
      window.history.replaceState({}, "", url);
    }

    async function render() {
      const entries = await loadIndex();
      const query = input.value.trim();
      writeQueryToUrl(query);

      if (!query) {
        results.innerHTML = "";
        return;
      }
      const found = search(entries, query, PAGE_RESULT_LIMIT);
      results.innerHTML = found.length === 0
        ? `<p class="search-empty">No matches.</p>`
        : found.map(row).join("");
    }

    input.addEventListener("input", render);
    input.value = new URLSearchParams(window.location.search).get("q") || "";
    render();
  }

  // ── Start ───────────────────────────────────────────────────────────

  const versionBadge = document.getElementById("version-badge");
  if (versionBadge) startVersionSwitcher(versionBadge).catch(() => {});

  const searchTrigger = document.getElementById("search");
  if (searchTrigger) startSearchModal(searchTrigger);

  const searchPageInput = document.getElementById("sp-input");
  if (searchPageInput) startSearchPage(searchPageInput, document.getElementById("sp-results"));
})();
