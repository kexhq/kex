// Type definitions for @kexhq/kex

/** Extra options forwarded to the underlying Emscripten module factory. */
export interface KexModuleOptions {
  locateFile?: (path: string, prefix: string) => string;
  [key: string]: unknown;
}

/**
 * A Kex REPL session backed by the wasm interpreter. Internal state (the
 * Emscripten module and the C-side session handle) is fully private —
 * construct instances via Kex.create(), not `new Kex(...)`.
 */
export class Kex {
  private constructor(module: unknown, session: number);

  /** Creates a new, independent Kex REPL session backed by the wasm interpreter. */
  static create(moduleOptions?: KexModuleOptions): Promise<Kex>;

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
  eval(source: string): Promise<string>;

  /**
   * Tab completion, reusing the same logic the native REPL's readline
   * integration uses. `line` is the current input line, `start` is the
   * index where the word under the cursor begins, `text` is that word
   * itself. Returns the list of completions (already rewritten to what
   * should be inserted in place of `text`).
   */
  complete(line: string, start: number, text: string): string[];

  /** Frees this session's underlying interpreter state. Safe to call more than once. */
  destroy(): void;

  /** The REPL banner with version info, matching the native CLI exactly. */
  banner(): string;

  /** Kex version string (e.g. "0.2.0"). */
  version(): string;
}
