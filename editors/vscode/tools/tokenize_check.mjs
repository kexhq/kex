// Tokenizes a Kex sample with the same engine VS Code uses, so grammar edits
// can be checked without launching an editor:  node tools/tokenize_check.mjs
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
const require = createRequire(import.meta.url);
const vsctm = require("vscode-textmate");
const oniguruma = require("vscode-oniguruma");

const wasm = readFileSync(require.resolve("vscode-oniguruma/release/onig.wasm"));

const SAMPLE = `make Parser, implement: Showable, Comparable do
  let parse(s: String) -> Json or! ParseError = Ok(1)
  let safe(s: String) -> Json or ParseError = Ok(1)
  find :> (X -> Bool) -> X?
  any? :> (X -> Bool) -> Bool
  let value = list.or("fallback")
  let n = maybe.or(0)
end
`;

await oniguruma.loadWASM(wasm);
const registry = new vsctm.Registry({
  onigLib: Promise.resolve({
    createOnigScanner: (s) => new oniguruma.OnigScanner(s),
    createOnigString: (s) => new oniguruma.OnigString(s),
  }),
  loadGrammar: async () =>
    vsctm.parseRawGrammar(
      readFileSync(new URL("../syntaxes/kex.tmLanguage.json", import.meta.url), "utf8"),
      "kex.tmLanguage.json",
    ),
});

const grammar = await registry.loadGrammar("source.kex");
let stack = vsctm.INITIAL;
const seen = new Map();
for (const line of SAMPLE.split("\n")) {
  const result = grammar.tokenizeLine(line, stack);
  for (const token of result.tokens) {
    const text = line.slice(token.startIndex, token.endIndex).trim();
    if (!text) continue;
    const scope = token.scopes[token.scopes.length - 1];
    if (!seen.has(scope)) seen.set(scope, new Set());
    seen.get(scope).add(text);
  }
  stack = result.ruleStack;
}

for (const [scope, texts] of [...seen].sort())
  console.log(scope.padEnd(42), [...texts].slice(0, 6).join(" "));
