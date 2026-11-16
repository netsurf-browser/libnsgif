Tests
=====

The `test/data/` directory contains test images and decoded reference data.

A test has the following:

| File    | Purpose |
| ------- | ------- |
| `.gif`  | The GIF image to test (may be broken/invalid). |
| `.meta` | Simple `key: value` format. Keys are `title`, `description`, `fixed-by`, `stderr`, `ulimit`. |
| `.pbm`  | Transparency mask data in PBM (P1) format. |
| `.ppm`  | Colour channel data in PPM (P3) format. |

In `.meta`, the `description` and `stderr` keys may be repeated. The
`description` may be wrapped over multiple `description` lines. The `stderr`
lines are one line per expected line of stderr output, which must match
exactly; no `stderr` lines means no output is expected.

The `.ppm` and `.pbm` hold every decoded frame stacked vertically, each
preceded by a `# frame N:` comment. If they are missing, the decoded output
isn't checked. In the `.pbm` mask, `1` is transparent and `0` is opaque.

The `ulimit` key sets a virtual memory limit (KiB) for decoding the image
(`ulimit -v`). It is for testing allocation failures. The decoded output
isn't written for these tests, so they don't have `.ppm` or `.pbm` files.
Note that ulimit tests are skipped where `ulimit -v` isn't supported and
under sanitizers.
