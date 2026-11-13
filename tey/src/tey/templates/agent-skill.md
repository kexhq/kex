---
name: kex
description: Write, check and test Kex (.kex) source. Use before writing or editing any Kex code in this package. Kex looks like Ruby but is statically typed, immutable and functional, so Ruby and Elixir habits produce code that does not compile.
---

# Writing Kex

The guide to Kex lives in the compiler, so that it always describes the
version installed. Read it before writing any Kex:

```sh
kex --skill
```

It ends with a list of topics; read the ones the task touches with
`kex --skill <topic>`: `functions`, `make`, `control`, `errors`, `modules`,
`processes`, `testing`, `style`, `tey`.

Then work in this loop:

1. Look names up, never guess them: `kex -s List`, `kex -s List.find`.
2. Check after every edit: `kex -C --source-root src <file>`.
3. Run the specs with `tey test`, and the program with `tey run`.

If `kex` is not on the path, install it with `brew install kexhq/tey/tey` or
`curl -fsSL https://kex.run/install.sh | sh`.
