(() => {
  // el("span", "search-name", "map"), or an array of child nodes for content.
  const el = (tag, className, content) => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (Array.isArray(content)) node.append(...content);
    else if (content != null) node.textContent = content;
    return node;
  };

  const fetchJson = (url) => fetch(url, { cache: "no-store" }).then((res) => res.json());

  // ── Version switcher ────────────────────────────────────────────────
  const badge = document.getElementById("version-badge");
  if (badge) versionSwitcher(badge).catch(() => {});

  async function versionSwitcher(badge) {
    const { root, package: pkg, version, page } = badge.dataset;
    const data = await fetchJson(root + "versions.json");
    const versions = (data.versions || []).filter((v) => v.package === pkg);
    if (versions.length === 0) return;
    const select = el("select", "version-select", versions.map((v) => {
      const option = el("option", "", `${v.label} ${v.id}`);
      option.value = v.id;
      option.selected = v.id === version;
      return option;
    }));
    select.setAttribute("aria-label", "Documentation version");
    select.addEventListener("change", async () => {
      const base = `${root}${pkg}/${select.value}/`;
      // The same page may not exist in the target version (renamed module,
      // or a source that no longer parses): fall back to the version's
      // index rather than landing on a 404.
      const found = await fetch(base + page, { method: "HEAD", cache: "no-store" })
        .then((res) => res.ok, () => false);
      window.location.href = base + (found ? page : "index.html");
    });
    badge.replaceWith(select);
  }

  // ── Search, shared by the modal and the full results page ───────────

  // Fetches search.json once, on first use. A failed fetch is an empty index.
  const indexLoader = (base) => {
    let pending;
    return () => pending ||= fetchJson(base + "search.json").then((data) => data.entries || [], () => []);
  };

  // Rank, don't just filter: a name hit outranks a summary hit, an exact
  // type outranks a mention in a signature, and an arrow in the query is a
  // function's business, not a module's. Returns -1 when a token matches
  // nowhere (all tokens must hit).
  const rank = (entry, tokens) => {
    const name = (entry.name || "").toLowerCase();
    const qname = (entry.qualifiedName || "").toLowerCase();
    const types = (entry.types || []).join(" ").toLowerCase();
    const sigs = (entry.signatures || []).join(" ").toLowerCase();
    const summary = (entry.summary || "").toLowerCase();
    let total = 0;
    for (const t of tokens) {
      const score = Math.max(
        name === t ? 100 : name.startsWith(t) ? 50 : name.includes(t) ? 20 : 0,
        qname === t ? 90 : qname.includes(t) ? 30 : 0,
        types === t ? 60 : types.includes(t) ? 25 : 0,
        sigs.includes(t) ? 10 : 0,
        summary.includes(t) ? 5 : 0
      );
      if (score === 0) return -1;
      total += score;
    }
    if (tokens.includes("->")) total += entry.kind === "function" ? 80 : -300;
    return total;
  };

  // The best `limit` entries for `query`, best first.
  const search = (entries, query, limit) => {
    const tokens = query.toLowerCase().split(" ").filter(Boolean);
    return entries
      .map((entry) => ({ entry, score: rank(entry, tokens) }))
      .filter((scored) => scored.score >= 0)
      .sort((a, b) => b.score - a.score)
      .slice(0, limit)
      .map((scored) => scored.entry);
  };

  // A result row: kind, name, defining module, then whatever `details` adds.
  const hit = (e, base, className, details) => {
    const head = el("span", "search-head", [
      el("span", `search-kind search-kind-${e.kind}`, e.kind),
      el("span", "search-name", e.name),
    ]);
    if (e.qualifiedName && e.qualifiedName !== e.name) {
      // The DEFINING module or entity, not the full qualified name —
      // "Enumerable" for Enumerable.map, "[X]" for [X].map.
      const dot = e.qualifiedName.lastIndexOf(".");
      head.append(el("span", "search-qual", dot === -1 ? e.qualifiedName : e.qualifiedName.slice(0, dot)));
    }
    const link = el("a", className, [head, ...details]);
    link.href = `${base}${e.urlPath}.html${e.anchor ? "#" + e.anchor : ""}`;
    return link;
  };

  const signature = (e) => (e.signatures && e.signatures[0]) || "";

  // ── Search modal ────────────────────────────────────────────────────
  const trigger = document.getElementById("search");
  if (trigger) searchModal(trigger);

  function searchModal(trigger) {
    const { root, package: pkg, version, page } = trigger.dataset;
    const base = `${root}${pkg}/${version}/`;
    const loadIndex = indexLoader(base);
    // The urlPath of the page the trigger sits on ("fs.html" → "fs";
    // "index.html" → "").
    const here = page === "index.html" ? "" : (page || "").replace(/\.html$/, "");
    let selected = 0;

    // The modal is built here so the static page stays bare; it never
    // appears without the script that drives it.
    const input = el("input");
    input.type = "search";
    input.placeholder = `Search ${pkg} ${version}`;
    input.autocomplete = "off";
    const results = el("div", "search-modal-results");
    const overlay = el("div", "search-modal", [el("div", "search-modal-panel", [input, results])]);
    overlay.hidden = true;
    document.body.append(overlay);

    const query = () => input.value.trim();
    const row = (e) => hit(e, base, "search-hit", [el("span", "search-sig", signature(e) || e.summary || "")]);
    const rows = () => results.querySelectorAll(".search-hit");

    const footer = () => {
      const more = el("a", "", "Open full search results");
      more.href = `${base}search.html${query() ? "?q=" + encodeURIComponent(query()) : ""}`;
      return el("div", "search-modal-footer", [more]);
    };

    const select = (index) => {
      const list = rows();
      if (list.length === 0) return;
      selected = (index + list.length) % list.length;
      list.forEach((item, i) => item.classList.toggle("selected", i === selected));
      list[selected].scrollIntoView({ block: "nearest" });
    };

    // The empty state: what a search can answer (name vs type), a few
    // example terms, and the entities of the page you are on.
    const showIdle = async () => {
      const chips = ["map", "Integer", "String?", "String -> String -> String", "FS.File"].map((example) => {
        const chip = el("button", "search-chip", example);
        chip.addEventListener("click", () => { input.value = example; run(); input.focus(); });
        return chip;
      });
      const hint = el("div", "search-hint", [
        el("p", "search-hint-title", "Search by name or type."),
        el("p", "search-hint-body", "A name finds what it is called; a type finds everything that uses it."),
        el("div", "search-chips", chips),
      ]);
      results.replaceChildren(hint, footer());
      if (!here) return;
      const local = (await loadIndex()).filter((e) => e.urlPath === here).slice(0, 4);
      // The reader may have typed, or closed the modal, while the index loaded.
      if (local.length === 0 || query() || !hint.isConnected) return;
      hint.after(el("div", "search-local", [el("p", "search-hint-title", "On this page"), ...local.map(row)]));
    };

    const run = async () => {
      const q = query();
      if (!q) return showIdle();
      const found = search(await loadIndex(), q, 50);
      if (query() !== q) return;
      results.replaceChildren(...(found.length ? found.map(row) : [el("div", "search-empty", "No matches")]), footer());
      select(0);
    };

    const open = () => {
      overlay.hidden = false;
      input.value = "";
      showIdle();
      input.focus();
    };
    const close = () => { overlay.hidden = true; trigger.focus(); };

    trigger.addEventListener("click", open);
    input.addEventListener("input", run);
    input.addEventListener("keydown", (ev) => {
      const current = results.querySelector(".search-hit.selected");
      if (ev.key === "Escape") close();
      else if (ev.key === "ArrowDown") { ev.preventDefault(); select(current ? selected + 1 : 0); }
      else if (ev.key === "ArrowUp") { ev.preventDefault(); select(current ? selected - 1 : -1); }
      else if (ev.key === "Enter" && current) window.location.href = current.href;
    });
    overlay.addEventListener("click", (ev) => { if (ev.target === overlay) close(); });

    document.addEventListener("keydown", (ev) => {
      const target = ev.target;
      const typing = target && (target.tagName === "INPUT" || target.tagName === "TEXTAREA" || target.isContentEditable);
      const slash = ev.key === "/" && overlay.hidden && !typing;
      if (slash || ((ev.metaKey || ev.ctrlKey) && ev.key === "k")) { ev.preventDefault(); open(); }
    });
  }

  // ── Full results page (search.html) ─────────────────────────────────
  const pageInput = document.getElementById("sp-input");
  if (pageInput) searchPage(pageInput, document.getElementById("sp-results"));

  function searchPage(input, results) {
    const loadIndex = indexLoader("");
    const row = (e) => hit(e, "", "search-page-hit", [
      el("span", "search-sig", signature(e)),
      ...(e.summary ? [el("span", "search-summary", e.summary)] : []),
    ]);

    const run = async () => {
      const q = input.value.trim();
      // The term lives in the URL, so a results page is a link, not a state.
      const url = new URL(window.location.href);
      if (q) url.searchParams.set("q", q); else url.searchParams.delete("q");
      window.history.replaceState({}, "", url);
      if (!q) return results.replaceChildren();
      const found = search(await loadIndex(), q, 200);
      if (input.value.trim() !== q) return;
      results.replaceChildren(...(found.length ? found.map(row) : [el("p", "search-empty", "No matches.")]));
    };

    input.addEventListener("input", run);
    input.value = new URLSearchParams(window.location.search).get("q") || "";
    run();
  }
})();
