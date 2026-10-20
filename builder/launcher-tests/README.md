# Temporary launcher parity suite

Keep this temporary differential suite through the pre-merge review. The original
Perl launcher remains available for traditional distro builds after the portable
build switches to C++; removing the temporary suite does not authorize removing
that launcher or its traditional-build dependencies.
The suite is deliberately independent of the new C++ option metadata. It uses a
frozen inventory of all 100 Perl config keys, full-default fixtures, individual
absence/null/empty/type/boundary mutations, enabled environment overrides,
layering, interpolation, CLI overrides/duplicates, UTF-8 and YAML syntax probes.
An inventory value is a probe, not an assertion of validity: the Perl result is
recorded as the compatibility oracle. Broad input coverage is not a proof of
complete equivalence.

Run from the repository root, only in the dedicated disposable container:

```sh
builder/launcher-tests/run --oracle-only --report /src/builder/launcher-tests/oracle-report.json
builder/launcher-tests/run --report /src/builder/launcher-tests/parity-report.json
builder/launcher-tests/run --filter desktop --limit 30
```

The image is `kasmvnc-launcher-parity:local`, includes g++ and libyaml-dev for
compiling a candidate, and mounts the source at `/src`. Candidate defaults to
`/src/unix/vncserver-native/vncserver-native`; override with `--candidate`.
`--oracle` also allows oracle-against-oracle harness checks. Docker runs have no
network; fixtures and fake binaries live only in the container. Reports written
under `/src` survive its removal. Do not run on a persistent application
container: fixture setup replaces `/usr/bin/Xvnc`, `vncpasswd`, and
`kasmvncpasswd` with no-op helpers. These tests **do not start real Xvnc**.

For the seven session lifecycle cases (list empty/live/stale, kill missing/live,
screen suffix and stale), after building the image:

```sh
docker run --rm --network none -v "$PWD:/src" --entrypoint python3 \
  kasmvnc-launcher-parity:local /src/builder/launcher-tests/lifecycle.py \
  --report /src/builder/launcher-tests/lifecycle-report.json
```

Each side gets freshly reset state at identical absolute paths and runs serially.
The runner captures status, raw output, parsed argv, file content hashes/modes and
Xauthority existence/size. It normalizes Perl source-line suffixes and random
addresses in diagnostics, generated *independent* option ordering, and fixture
child PIDs. Duplicate same-name generated arguments (HTTP headers) and explicit
CLI duplicate order remain significant. No blanket expected-failure list or
accepted-difference filtering exists: JSON reports retain all mismatches.

The dry-run runner uses the common `-dry-run` interface, including its existing
filesystem side effects. Filesystem differences remain visible in the report.

Before merge, also run existing `builder/test-vncserver` package-installed mamba
specs with each launcher selected in the test image (retain the Perl warnings
check only for the Perl run). Those specs exercise DE/password prompts and real
startup prerequisites missing from this lean fixture. Add real Xvnc installed
DEB/RPM/tar smoke tests and service-path checks. This directory alone does not
cover first-run interactive input, helper failures, TLS permissions across real
users/groups, occupied displays, adversarial signal races, or real
endpoint/browser behavior. It does not recreate unsafe
shell-injection behavior or promise bug-for-bug compatibility on unsupported
YAML, integer overflow, or cyclic interpolation. Review those differences
explicitly and add bounded regression tests to production where appropriate.

`inventory.json` was transcribed from the Perl key declarations and reviewed for
optional grouped keys, enum choices, format-specific values and boundary cases.
If the production Perl schema changes before merge, update this independent
inventory deliberately. Do not generate it from the candidate.

The corpus also compares an override for each of the 90 generated Xvnc option
names, using the frozen inventory. `--filter option:` runs that subset quickly.
The default config source is exercised verbatim as well as through generated
fixtures. JSON reports include per-key counts and accepted explicit-value probes.
The candidate executable is copied once per config run, and its SHA256 is
recorded, so a concurrent rebuild cannot mix versions within a report.

Nine additional fake-server process scenarios cover startup/readiness, terminal
failure, retry with a safe font path, foreground/autokill/background application
launch, generated xstartup contents, a custom startup path and Unicode CLI argv:

```sh
docker run --rm --network none -v "$PWD:/src" --entrypoint python3 \
  kasmvnc-launcher-parity:local /src/builder/launcher-tests/startup.py \
  --report /src/builder/launcher-tests/startup-report.json
```

This compiles a real fixture executable named Xvnc that records actual argv and
stays alive until signaled; xdpyinfo is a controlled readiness helper. The Python
PID1 reaps only fixture-owned orphan Xvnc children, matching normal init behavior.
Startup scripts capture DISPLAY and VNCDESKTOP. These process tests exercise
launcher orchestration, **not** the real X server, desktop, browser or TLS stack.
Run lifecycle/startup suites without rebuilding their candidate concurrently.

Self-check any runner with `--candidate /src/unix/vncserver` before trusting a
new fixture. The first full 921-case characterization recorded 680 Perl-accepted
cases with at least one accepted explicit-value probe for each of 100 keys and
no timeouts. The later option and YAML additions expand that inventory; consult
the current JSON report for the tested version and counts. A passing oracle-only
run is characterization, not proof that the C++ replacement passes.

Sequence-shape probes cover single/empty/two-element lists, nested lists and
lists containing mappings for every key. These distinguish intended list-valued
settings from scalar validation and old Perl reference-stringification quirks.
The current corpus also covers the exclusive-session disconnect switch and
watermark/scrolling prerequisite interactions. Use `--filter sequence:` or
`--filter interaction:` for those subsets. Reports retain exact case input and
an aggregate corpus SHA256 in addition to the candidate SHA256.

Diagnostic comparison treats Perl `HASH(0x...)` and the native `<mapping>`
placeholder as equivalent descriptions of a wrong-type mapping. This does not
normalize argv or status differences. `recompare.py INPUT OUTPUT` can re-evaluate
saved raw captures after a comparison-only correction; it writes a separate
report identifying its source and never runs or substitutes a new candidate.

Candidate-only safety checks intentionally never execute the unsafe Perl oracle:

```sh
docker run --rm --network none -v "$PWD:/src" --entrypoint python3 \
  kasmvnc-launcher-parity:local /src/builder/launcher-tests/safety.py \
  --report /src/builder/launcher-tests/safety-report.json
```

They verify session-directory symlinks are rejected for launch/list/kill;
password/PID symlinks and hardlinks preserve their targets; unsafe PID reads do
not signal the fixture server; stale unrelated PIDs remain alive; recursive
environment interpolation and excessive YAML size/depth terminate promptly; and
shell metacharacters reach the fake Xvnc literally without executing a command.
These safety checks assert the intended new contract independently of parity.
