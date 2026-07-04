// Thin wrapper around the wasm REPL bindings (src/wasm_repl.cxx in the main
// repo) for consumption as an npm package — this is what kex.run imports to
// embed the interpreter. At publish time, the "prepack" build step (see
// ../README.md) copies this file into dist/ alongside the built
// kex_repl_wasm.js/.wasm/.data, so everything resolves relative to this same
// directory at runtime (works in both Node and a bundler, since it's plain
// import.meta.url-relative resolution, no cwd-dependent paths).
import createKexReplModule from "./kex_repl_wasm.js";

// Private fields keep the raw Emscripten Module and the C-side session
// pointer out of the public API entirely — callers only ever see eval/
// complete/destroy, never the ccall plumbing underneath.
export class Kex {
  #module;
  #session;
  #destroyed = false;

  // Not called directly — use Kex.create(), since standing up the wasm
  // module is inherently async and a constructor can't await.
  constructor(module, session) {
    this.#module = module;
    this.#session = session;
  }

  /** Creates a new, independent Kex REPL session backed by the wasm interpreter. */
  static async create(moduleOptions = {}) {
    const module = await createKexReplModule({
      // In Node, the preload-file package (kex_repl_wasm.data) is fetched
      // via a plain fs.readFileSync(path) call, which doesn't accept a
      // file:// URL string — only a real filesystem path. url.pathname
      // (not .href) gives that directly on POSIX. In a browser this needs
      // an actual URL for fetch(), hence .href there instead.
      locateFile: (path) => {
        const url = new URL(path, import.meta.url);
        return typeof window === "undefined" ? url.pathname : url.href;
      },
      ...moduleOptions,
    });
    const session = module.ccall("kex_repl_create", "number", [], []);
    return new Kex(module, session);
  }

  #assertAlive() {
    if (this.#destroyed) throw new Error("this Kex session has already been destroyed");
  }

  /**
   * Evaluates one chunk of Kex source against this session's persistent
   * interpreter state. State (`let`/`var` bindings, spawned processes)
   * persists across calls on the same session, like a real REPL.
   *
   * Multi-line `do ... end` blocks must be accumulated into one string by
   * the caller before calling eval — see web/index.html in the main repo
   * for the exact `countBlocks()` logic the real REPL uses.
   *
   * The returned string already contains ANSI color escape codes (matching
   * the native CLI's REPL exactly) — render it through a real terminal
   * emulator (e.g. xterm.js) rather than stripping them, unless you
   * specifically want plain text.
   */
  async eval(source) {
    this.#assertAlive();
    // kex_repl_eval is void-returning on purpose — see src/wasm_repl.cxx's
    // own doc comment: a direct return value from an Asyncify-instrumented
    // export wasn't reliably reaching the JS caller through ccall's async
    // plumbing, so the result is fetched via a second, ordinary synchronous
    // call instead (kex_repl_last_result never touches Asyncify).
    await this.#module.ccall(
      "kex_repl_eval", null,
      ["number", "string"], [this.#session, source],
      { async: true }
    );
    return this.#module.ccall("kex_repl_last_result", "string", ["number"], [this.#session]);
  }

  /**
   * Tab completion, reusing the same logic the native REPL's readline
   * integration uses. `line` is the current input line, `start` is the
   * index where the word under the cursor begins, `text` is that word
   * itself. Returns the list of completions (already rewritten to what
   * should be inserted in place of `text`).
   */
  complete(line, start, text) {
    this.#assertAlive();
    const raw = this.#module.ccall(
      "kex_repl_complete", "string",
      ["number", "string", "number", "string"],
      [this.#session, line, start, text]
    );
    return raw.split("\n").filter((m) => m.length > 0);
  }

  /** Frees this session's underlying interpreter state. Safe to call more than once. */
  destroy() {
    if (this.#destroyed) return;
    this.#module.ccall("kex_repl_destroy", null, ["number"], [this.#session]);
    this.#destroyed = true;
  }
}
