Tests
=====

The `test/data/` directory contains test images and decoded reference data.

A test has the following:

| File    | Purpose |
| ------- | ------- |
| `.gif`  | The GIF image to test (may be broken/invalid). |
| `.meta` | Simple `key: value` format. Keys are `title`, `description`, `fixed-by`, `stderr`. |
| `.pbm`  | Transparency mask data in PBM (P1) format. |
| `.ppm`  | Colour channel data in PPM (P3) format. |

In `.meta`, the `description` and `stderr` keys may be repeated. The
`description` may be wrapped over multiple `description` lines. The `stderr`
lines are one line per expected line of stderr output, which must match
exactly; no `stderr` lines means no output is expected.

The `.ppm` and `.pbm` hold every decoded frame stacked vertically, each
preceded by a `# frame N:` comment. If they are missing, the decoded output
isn't checked. In the `.pbm` mask, `1` is transparent and `0` is opaque.
