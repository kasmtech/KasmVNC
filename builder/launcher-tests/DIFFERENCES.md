# Explicit compatibility differences

The differential runner does not silently waive differences. It reports all
status, argv, filesystem and diagnostic mismatches and retains raw evidence.
The portable launcher intentionally differs in these reviewed categories:

- UTF-8 text supplied through command arguments or environment variables reaches
  Xvnc correctly. The Perl wrapper can encode those already-UTF-8 bytes twice;
  the native launcher does not reproduce its mojibake. Unicode supplied directly
  through YAML remains covered by ordinary parity cases.
- Nested mapping elements in setting sequences, and sequences used in compound
  Log/UnixRelay scalar fields, are rejected. Perl can turn these malformed values
  into process-address strings such as `HASH(0x...)` or `ARRAY(0x...)`, which do not
  define meaningful stable server arguments. Rejection is deliberate, rather
  than treating memory addresses as a portable interface.
- Validation and malformed-YAML diagnostics use the native parser's wording;
  interpreter stack traces and Perl uninitialized-value warnings are not copied.
  The harness equates `HASH(0x...)` and `<mapping>` only in diagnostics, never in
  argv or exit status.
- Unsafe filesystem links, unrelated/stale process IDs, recursive interpolation
  and excessive YAML input are bounded or rejected. Literal shell metacharacters
  are passed as arguments without interpretation. Candidate-only safety tests
  assert these behaviors; unsafe Perl behavior is not executed as an oracle.

These categories are not a blanket exception for normal configuration, command
precedence, supported list-valued settings, installed paths or lifecycle
behavior. Remaining mismatches must be inspected and assigned a concrete cause.
Use the latest report's candidate and corpus hashes when documenting results.
Traditional distro builds retain the original Perl launcher; these changes apply
to the native launcher used by the portable build.
