# Skyrim 1.7.104.0 terrain hook compatibility

The plugin loaded but disabled terrain deformation because the old render-depth
call offset pointed at byte 00. The updated public 1.7 branch selects +0x3B3
only for runtime 1.7.104.0; older runtimes retain their existing offsets.

Verified against the user's 1.7.104.0 executable and its format-5 Address Library,
with a comparison to the working 1.6.640.0 executable and matching library:

| Hook | Caller AE ID | 1.6.640 offset | 1.7.104 offset | Target AE ID |
|---|---:|---:|---:|---:|
| Render depth | 36559 | 0x395 | 0x3B3 | 107139 |
| Main update | 36544 | 0x160 | 0x160 | 36564 |
| Optional post-opaque snow spray | 106583 | 0x308 | 0x308 | 106587 |

All three sites were disassembled from PE exception-directory code regions and
confirmed as instruction-aligned five-byte calls to the same function IDs.
Argument setup at the moved depth call is preserved.

Audited 1.7.104.0 EXE SHA-256:
`846efccf0c1374d71f892907f46549560f2fcb0a75cb87a3eed438baa0f1402f`.

`tools/offline/InspectRuntimeHooks.py` reproduces the read-only audit with
`--exe`, `--library` and optional `--out` arguments. It requires Python and
Capstone and supports Address Library formats 1/2/5. Its audit profiles cover
1.6.640.0 and 1.7.104.0, not unverified 1.7.99 or future runtimes.

The port also checks both required call opcodes before installing geometry or
update hooks, logs the chosen runtime/depth offset, and corrects the old overly
broad claim that no plugin modifications had occurred when a check failed.
Existing call targets remain chained. The public release's optional snow spray
keeps its existing render point; experimental snow throws and authored mesh
features are not part of this compatibility change.

Validation: public Release build, both executable audits, ShaderGen (zero
failures), StampSurfaceTest and TerrainProcessingGuardTest pass. The same
render-depth offset fix was tested by the user on the experimental branch
before this port; the public DLL should receive a separate in-game smoke test.
