# Editor support for Loqi

## Syntax highlighting

`loqi.tmLanguage.json` is a TextMate grammar (scope `source.loqi`, file type
`.lq`). It works in VS Code, Sublime Text, Zed, and anything that consumes
TextMate grammars.

### VS Code (quick local install)

1. Create a folder `~/.vscode/extensions/loqi-lang/`.
2. Copy `loqi.tmLanguage.json` into it.
3. Add a `package.json`:

```json
{
  "name": "loqi-lang",
  "version": "0.1.0",
  "engines": { "vscode": "^1.60.0" },
  "contributes": {
    "languages": [{ "id": "loqi", "extensions": [".lq"], "aliases": ["Loqi"] }],
    "grammars": [{ "language": "loqi", "scopeName": "source.loqi", "path": "./loqi.tmLanguage.json" }]
  }
}
```

4. Reload VS Code. `.lq` files now highlight.

A full LSP (completion, go-to-definition, inline diagnostics) is on the
[roadmap](../docs/ROADMAP.md).
