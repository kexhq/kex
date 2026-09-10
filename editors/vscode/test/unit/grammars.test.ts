// What a .ket template's grammar must do around its host language
// (kexhq/kex#314).
//
// A .ket file is a host document — HTML, Markdown, anything — with `<% %>`
// holes of Kex in it, and the host is knowable only from the extension in
// front of .ket. So `foo.html.ket` and `foo.md.ket` get their own grammars,
// each the host's grammar plus the tag rules. The part worth testing is the
// part that is easy to get wrong: the tags must outrank the host EVERYWHERE,
// not just where the host happens to have nothing to say. A host rule that
// owns a region — a Markdown heading, an HTML attribute value — would swallow
// a tag if the tags were merely listed first under `patterns`, which is why
// they are an `L:` injection instead.

import { describe, expect, test } from 'bun:test';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import * as vsctm from 'vscode-textmate';
import * as oniguruma from 'vscode-oniguruma';

const require_ = createRequire(import.meta.url);
const wasm = readFileSync(require_.resolve('vscode-oniguruma/release/onig.wasm'));
const onigLib = oniguruma.loadWASM(wasm).then(() => ({
  createOnigScanner: (patterns: string[]) => new oniguruma.OnigScanner(patterns),
  createOnigString: (s: string) => new oniguruma.OnigString(s),
}));

const repoFile = (path: string) => new URL(`../../${path}`, import.meta.url).pathname;

// Stands in for a real host grammar: one rule that opens a region and then
// owns every position inside it, the shape both `text.html.markdown` (a
// heading, from its `#` to end of line) and `text.html.basic` (an attribute
// value, between its quotes) take. Nothing else is consulted until the region
// closes, which is exactly what the tag rules have to survive. Using a stub
// rather than VS Code's own grammars keeps this test off the filesystem of
// whatever machine it runs on.
const stubHost = (scopeName: string) => ({
  scopeName,
  patterns: [{ begin: '^#', end: '$', name: 'markup.heading.stub' }],
});

const GRAMMARS: Record<string, string> = {
  'source.ket': 'syntaxes/ket.tmLanguage.json',
  'source.kex': 'syntaxes/kex.tmLanguage.json',
  'text.html.ket': 'syntaxes/ket-html.tmLanguage.json',
  'text.markdown.ket': 'syntaxes/ket-markdown.tmLanguage.json',
};

// The host grammars name their real hosts; both resolve to the stub here.
const HOSTS = new Set(['text.html.basic', 'text.html.markdown']);

const registry = new vsctm.Registry({
  onigLib,
  loadGrammar: async (scope) => {
    if (HOSTS.has(scope)) return stubHost(scope) as vsctm.IRawGrammar;
    const path = GRAMMARS[scope];
    if (!path) return null;
    return vsctm.parseRawGrammar(readFileSync(repoFile(path), 'utf8'), repoFile(path));
  },
});

/** Every scope carried by the tokens whose text is exactly `text`. */
const scopesOf = async (scopeName: string, source: string, text: string) => {
  const grammar = await registry.loadGrammar(scopeName);
  if (!grammar) throw new Error(`no grammar for ${scopeName}`);
  const found: string[] = [];
  let rules = vsctm.INITIAL;
  for (const line of source.split('\n')) {
    const result = grammar.tokenizeLine(line, rules);
    rules = result.ruleStack;
    for (const token of result.tokens) {
      if (line.slice(token.startIndex, token.endIndex) === text) found.push(...token.scopes);
    }
  }
  return found;
};

const hosts = [
  ['HTML', 'text.html.ket'],
  ['Markdown', 'text.markdown.ket'],
] as const;

for (const [host, scopeName] of hosts) {
  describe(`the ${host} host of a .ket template`, () => {
    test('a tag inside a region the host already owns is still a tag', async () => {
      expect(await scopesOf(scopeName, '# <%= name %>', '<%=')).toContain('punctuation.section.embedded.begin.ket');
    });

    test('the host still highlights what is not a tag', async () => {
      expect(await scopesOf(scopeName, '# plain heading', ' plain heading')).toContain('markup.heading.stub');
    });

    test('Kex inside a tag is Kex', async () => {
      expect(await scopesOf(scopeName, '<% if ready do %>', 'if')).toContain('keyword.control.kex');
      expect(await scopesOf(scopeName, '<%= 42 %>', '42')).toContain('constant.numeric.kex');
    });

    test('a comment tag is a comment, never code', async () => {
      const scopes = await scopesOf(scopeName, '<%# if ready do %>', ' if ready do ');
      expect(scopes).toContain('comment.block.ket');
      expect(scopes).not.toContain('keyword.control.kex');
    });

    test('<%% is an escaped literal, not the start of a tag', async () => {
      expect(await scopesOf(scopeName, '<%% not a tag', '<%%')).toContain('constant.character.escape.ket');
    });
  });
}

describe('package.json', () => {
  const manifest = JSON.parse(readFileSync(repoFile('package.json'), 'utf8'));
  const languages: { id: string; extensions?: string[]; configuration?: string }[] = manifest.contributes.languages;
  const grammars: { language: string; scopeName: string; path: string }[] = manifest.contributes.grammars;
  const extensionsOf = (id: string) => languages.find((language) => language.id === id)?.extensions ?? [];

  // VS Code picks the longest matching extension, so `.html.ket` reaching the
  // HTML host at all depends on these compound entries existing — and on the
  // plain `ket` language never claiming them back.
  test('the compound extensions reach their host grammar', () => {
    expect(extensionsOf('ket-html')).toContain('.html.ket');
    expect(extensionsOf('ket-markdown')).toContain('.md.ket');
    expect(extensionsOf('ket')).toEqual(['.ket']);
  });

  test('every contributed grammar is declared for a contributed language', () => {
    for (const grammar of grammars) {
      expect(languages.map((language) => language.id)).toContain(grammar.language);
      expect(readFileSync(repoFile(grammar.path), 'utf8')).toContain(grammar.scopeName);
    }
  });

  test('the packaged files carry every grammar and language configuration', () => {
    const packaged: string[] = manifest.files;
    expect(packaged).toContain('syntaxes/**');
    for (const language of languages) {
      if (language.configuration) expect(packaged).toContain(language.configuration.replace('./', ''));
    }
  });
});
