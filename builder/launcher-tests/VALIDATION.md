# Local launcher validation

Final frozen candidate SHA256:
`b57772f3c3ea1837dffe404fc5ea9df7e56a986ac44579377ad82301f4048321`.
Corpus SHA256:
`db4d8b53ffc879d54b502d0a2f1060fb96911c2afc8e17b733d74f723e960434`.

The disposable Docker run recorded 1,518 differential cases covering all 100
inventoried config keys and 90 generated-option override names. Perl accepted
923 cases. Neither oracle nor candidate timed out. Runtime was 327 seconds.
`parity-release-report.json` contains raw inputs and both captures.

The strict comparator reports 1,279 matching cases and 239 differences. These
are deliberately retained as failures rather than silently waived:

| Difference | Cases | Reviewed cause |
| --- | ---: | --- |
| Status and filesystem | 17 | Native rejects sequence elements containing mappings before creating session files; Perl accepts address-stringified malformed values. |
| Status and filesystem | 6 | Native rejects single/empty/two-element lists in scalar Log writer/destination fields; Perl emits array addresses. |
| Argv | 2 | CLI/environment Unicode is delivered as UTF-8 instead of Perl's double-encoded bytes. |
| Diagnostics only | 100 | Nested-list parser wording. |
| Diagnostics only | 83 | Mapping/type rejection wording. |
| Diagnostics only | 20 | Empty-value warnings or diagnostic wording, including empty timezone rejection. |
| Diagnostics only | 6 | Single/two-element compound UnixRelay/Log validation wording. |
| Diagnostics only | 5 | Missing environment interpolation warning and four YAML syntax/duplicate diagnostics. |

No remaining difference in this corpus falls outside the explicit categories in
`DIFFERENCES.md`. This is bounded evidence, not a claim of complete equivalence
for every possible input or environment.

The same candidate passed all seven list/kill lifecycle cases and all eight
baseline startup cases: readiness, failure, font retry, foreground, autokill,
background applications, generated startup and custom startup. The ninth startup
case confirms the documented Unicode correction in actual fake-Xvnc argv.
Evidence is in `lifecycle-release-report.json` and
`startup-release-report.json`.

All 14 candidate-only safety checks passed, including link rejection, preserving
unrelated processes, bounded cyclic/oversized/deep inputs, and literal shell
metacharacters reaching the fake server without execution. Evidence is in
`safety-release-report.json`; unsafe historical behavior was never invoked.

These tests use a compiled fake Xvnc and controlled helper scripts. Installed
package tests, real server authentication and startup, interactive first-run
specs, package linkage checks, and sanitizers are separate validation performed
by the main task. See README for remaining limits and reproduction commands.
Generated JSON reports are local ignored artifacts; keep them through review.
