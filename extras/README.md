# extras

Runnable demos of capabilities that are **also in the language** but not the headline.
Loqi's one job is structured text extraction (`cat data | loqi extract.lq > out.ndjson`,
see [`examples/ai/extract.lq`](../examples/ai/extract.lq)); these show the rest of the
runtime in action.

- [`ai/server.lq`](ai/server.lq): an AI endpoint with `http.serve`. POST a prompt,
  get the model's answer back. The HTTP server, the LLM call, and JSON are all built in.
- [`ai/web.lq`](ai/web.lq): `http.get` + `json.parse`, fetch and read live JSON in two
  lines, no SDK.

Both need `curl` on `PATH`; `server.lq` also needs `ANTHROPIC_API_KEY`. They are not
part of the smoke-tested example set (which leads with extraction); run them directly:

```sh
loqi extras/ai/web.lq
ANTHROPIC_API_KEY=sk-... loqi extras/ai/server.lq
```
