import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const vsctm = require("vscode-textmate");
const oniguruma = require("vscode-oniguruma");
const wasm = readFileSync(require.resolve("vscode-oniguruma/release/onig.wasm"));
await oniguruma.loadWASM(wasm);
const registry = new vsctm.Registry({
  onigLib: Promise.resolve({
    createOnigScanner: (s) => new oniguruma.OnigScanner(s),
    createOnigString: (s) => new oniguruma.OnigString(s),
  }),
  loadGrammar: async () => vsctm.parseRawGrammar(
    readFileSync(new URL("../syntaxes/kex.tmLanguage.json", import.meta.url), "utf8"),
    "kex.tmLanguage.json"),
});
const grammar = await registry.loadGrammar("source.kex");
const file = readFileSync(process.argv[2], "utf8");
let stack = vsctm.INITIAL;
file.split("\n").forEach((line, n) => {
  const r = grammar.tokenizeLine(line, stack);
  for (const t of r.tokens) {
    const text = line.slice(t.startIndex, t.endIndex);
    if (!text.trim()) continue;
    const scope = t.scopes[t.scopes.length - 1];
    // A comment line whose tokens are NOT comment-scoped means a rule leaked.
    if (line.trim().startsWith("#") && !scope.includes("comment") &&
        !scope.includes("documentation") && !scope.includes("markup") &&
        !scope.includes("entity.name.type") && !scope.includes("variable.parameter"))
      console.log(`LEAK line ${n + 1}: ${JSON.stringify(text)} -> ${scope}`);
    if (process.env.SHOW && scope.includes(process.env.SHOW))
      console.log(`${String(n + 1).padStart(4)}  ${JSON.stringify(text).padEnd(14)} ${scope}`);
  }
  stack = r.ruleStack;
});
console.log("done");
