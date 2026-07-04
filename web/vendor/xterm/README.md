# Vendored xterm.js

`xterm.mjs` / `xterm.css` from `@xterm/xterm@6.0.0`, `addon-fit.mjs` from
`@xterm/addon-fit@0.11.0` — copied straight from each package's built
`lib/*.mjs` (ESM build) so `web/index.html` can `import` them directly as a
`<script type="module">`, no bundler/CDN required.

To refresh:

```
npm install @xterm/xterm @xterm/addon-fit --no-save --prefix /tmp/xterm-fetch
cp /tmp/xterm-fetch/node_modules/@xterm/xterm/lib/xterm.mjs web/vendor/xterm/
cp /tmp/xterm-fetch/node_modules/@xterm/xterm/css/xterm.css web/vendor/xterm/
cp /tmp/xterm-fetch/node_modules/@xterm/addon-fit/lib/addon-fit.mjs web/vendor/xterm/
```
