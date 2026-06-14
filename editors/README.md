# Editor support for Lume

## Syntax highlighting

`lume.tmLanguage.json` is a TextMate grammar (scope `source.lume`, file type
`.lm`). It works in VS Code, Sublime Text, Zed, and anything that consumes
TextMate grammars.

### VS Code (quick local install)

1. Create a folder `~/.vscode/extensions/lume-lang/`.
2. Copy `lume.tmLanguage.json` into it.
3. Add a `package.json`:

```json
{
  "name": "lume-lang",
  "version": "0.1.0",
  "engines": { "vscode": "^1.60.0" },
  "contributes": {
    "languages": [{ "id": "lume", "extensions": [".lm"], "aliases": ["Lume"] }],
    "grammars": [{ "language": "lume", "scopeName": "source.lume", "path": "./lume.tmLanguage.json" }]
  }
}
```

4. Reload VS Code. `.lm` files now highlight.

A full LSP (completion, go-to-definition, inline diagnostics) is on the
[roadmap](../docs/ROADMAP.md).
