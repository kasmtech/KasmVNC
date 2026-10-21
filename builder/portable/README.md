# Portable KasmVNC packages

These packages contain the same native application build in DEB, RPM and tar
formats. They require glibc 2.34 or newer; these files do not run on musl or EL8.
Alpine uses the separate [portable musl APK profile](../musl/README.md).
The x86_64 build uses EL9 distro archives, so its supported CPU baseline is
x86-64-v2. ARM64 is built natively. GPU loaders and drivers, timezone data,
XKB tools/data, and the desktop remain host dependencies.

Install the DEB/RPM with the distro package manager, allowing it to resolve
dependencies. The package name remains `kasmvncserver`, replacing an existing
distro-specific KasmVNC package. The portable launcher is C++20; traditional distro builds retain Perl.
DEB uses the existing command alternatives and ssl-cert certificate; RPM uses
the existing `kasmvnc-cert` group and `/etc/pki/tls/private/kasmvnc.pem` certificate.
The package does not add desktop users to privileged certificate groups.

The tar archive has filesystem-root-relative paths. Install its runtime
dependencies first, stop existing sessions, preserve `/etc/kasmvnc/kasmvnc.yaml`,
then extract as root at `/`, excluding that configuration file when upgrading.
Run `/usr/lib/kasmvncserver/initialize-tls` as root to generate the RPM-style
certificate if needed. Configure a certificate readable by your desktop user
or add that user to `kasmvnc-cert`. Do not overlay a package-managed installation
with the tar archive; use its DEB/RPM instead. Tar installation has no package
manager tracking or automatic uninstaller; retain its file listing.

Tar dependencies include glibc, libGL, libgbm, bash, tzdata, xauth, xdpyinfo,
xkbcomp, XKB data and openssl. Group creation uses groupadd/getent. Mesa DRI and VAAPI
drivers are supplied by the host. Their default search checks the native Debian
multiarch directory (`/usr/lib/x86_64-linux-gnu/dri` or
`/usr/lib/aarch64-linux-gnu/dri`), then `/usr/lib64/dri` and `/usr/lib/dri`.
Explicit `LIBGL_DRIVERS_PATH` and `LIBVA_DRIVERS_PATH` retain their upstream
override behavior for custom driver locations. These paths do not guarantee
compatibility with every host driver; validate the intended GPU and distro.

All shipped ELF executables are checked for architecture, PIE, unexpected
dynamic dependencies, search paths, and a maximum glibc ABI requirement of 2.34.
The adjacent checksum and provenance files identify the build inputs and ELF
boundary. Installed documentation includes dependency license notices and the
builder RPM inventory (build provenance only). The separate CycloneDX SBOM
and advisory scan reports are described below. Embedded dependencies require rebuilding KasmVNC for security fixes;
host package updates do not patch them. A public release still requires review
of corresponding-source distribution and all embedded dependency licenses.

## Portable CI and legacy fallback

The default pipeline builds glibc DEB/RPM/tar packages and separate musl APKs
for amd64 and arm64. Native distro builds remain for Ubuntu Focal, Oracle 8
and openSUSE 15. The 31 glibc cases and eight Alpine 3.21–3.24 cases are mandatory.
Functional and vncserver specification tests consume the portable amd64 DEB.
Set `BUILD_FULL_LEGACY=true` to additionally build/test the other non-Alpine
traditional distros. `BUILD_PORTABLE=false` disables both portable profiles,
including Alpine CI, and restores the non-Alpine traditional matrix and Jammy
test artifacts. Old Alpine recipes remain available locally for comparison;
per-release Alpine build jobs have been retired.
`BUILD_DISTROS_REGEX` still filters legacy builds; it does not narrow the portable
test matrix. Its special `none` value disables portable builds as before.

This reduces feature-branch jobs from 101 to 59 and package builds from 34 to 10
compared with the combined portable/legacy pipeline after Nix was removed.
Compare total job duration (runner time), pipeline wall time, and artifact bytes
against the prior successful pipeline; counts alone do not predict time savings.
Public rollout remains subject to manual host/kernel/GPU validation.

Separate build-ID debug symbols are published in DEB, RPM and tar formats, with
checksums and a `.debug.json` mapping. See `DEBUGGING.md` for exact-build matching
and the existing crashpad lookup.

## SBOM, source inputs and advisory vulnerability reports

Each architecture publishes the following alongside its runtime and debug
packages under the same S3 commit directory:

* `.cdx.json`: CycloneDX 1.6 JSON for embedded native components, vendored code
  and the web client's actually bundled npm dependencies and identified vendored
  DES/base64 code. The same application
  inventory applies to the DEB, RPM and tarball; packaging metadata differs.
* `.sources.tar.gz` and `.sources.json`: exact source inputs and their hashes,
  native link evidence, build revision, host requirements and known limitations.
* `.vulnerabilities.json` and `.vulnerability-summary.json`: Grype findings,
  scanner/database metadata and advisory severity counts.

GitLab also receives `gl-sbom.cdx.json` (a byte-identical SBOM copy) and
`gl-dependency-scanning-report.json` as typed CI reports for every glibc/musl
architecture. These stable-name files are GitLab artifacts only; the original
revision-named files above remain the published, checksummed S3 evidence.
The adapter converts the existing Grype results without rescanning. Stable IDs
exclude build paths, architecture and commit names; package versions and advisory
IDs remain distinct. Exact duplicate matches are collapsed, conflicting duplicates
fail conversion. Findings remain advisory; scanner/conversion errors fail the job.
GitLab Ultimate can display the reports in pipeline Security and MR security
views; useful MR new/resolved comparisons also require reports on the target
branch. Project vulnerability tracking uses default-branch results.

Validate the adapter in a dedicated container with
`builder/portable/test-gitlab-dependency-report`. The test downloads the pinned
GitLab 15.2.3 schema and verifies its SHA256 before validating generated reports.
Optional positional arguments are local Grype JSON reports to validate as well.
This adapter and its tests live outside the dependency-image contexts, so changes
here do not invalidate the expensive dependency caches.

The existing `.sha256` manifest covers these artifacts too. Source archives are
uploaded to S3 but excluded from GitLab's build artifact ZIP so every distro test
need not download them. The matrix permits only that specific source archive to
be absent; all other listed artifacts still require checksum verification.

Native inventory comes from GNU ld's selected archive-member evidence for all
five executables, including compiler runtimes and CRT objects. It maps packaged
archives to their installed RPM versions and source RPMs, rather than treating
`rpm -qa` as a list of shipped software. Unknown native inputs or missing source
fail packaging. Source RPMs must have the exact expected identity and a trusted
RPM signature. They retain the distribution's patches/specs; a plain upstream
release would not be the corresponding source for a backported distro archive.
Source-built inputs retain their release archives or exact Git snapshots,
license notices, and hashes. KasmVNC's tracked source archive supplies its
patches and build scripts; the private functional-test repository is omitted.
The web build separately records rendered module identities and hashes every
shipped asset and exported license notice, and native packaging verifies that
handoff before publishing. Runtime packages install notices under
`/usr/share/doc/kasmvncserver/third-party-licenses`, including vendored license
headers and source files when upstream has no standalone license text.

Host glibc, Bash, timezone data, desktop tools/data and GPU loaders/drivers remain host
requirements, not builder-versioned runtime components in this SBOM. Scan the
final deployed image separately to cover them. A linked library entry does not
mean every upstream feature is enabled; archive/member selection and compiler
optimization can remove unused code.

Grype 0.120.1 is pinned by architecture-specific SHA256. Vulnerability findings
are initially advisory and do not fail the build. Failure to produce the SBOM,
retrieve/validate required sources, update/check the vulnerability database, or
execute the scanner does fail the build. Review findings for enabled features,
reachability and distribution backports; neither an upstream-version match nor
an empty report establishes exploitability or safety. Reports record the DB
used, so later scans may differ without a binary change.

Read the `limitations` fields before any public release. Some vendored sources
have no independently verified upstream release number and may not match
vulnerability databases. The existing QOI WebAssembly asset currently has
unverified corresponding source/license provenance; it is explicitly recorded
by SHA256, not silently represented as audited source. Included source material
and notices support compliance review, but do not certify that every legal
obligation has been satisfied. In particular, the source bundle is not claimed
to close a recorded source gap or to make the build bit-for-bit reproducible.

Portable artifact generation requires committed tracked sources and a revision
matching `HEAD`, so Docker's build inputs cannot silently differ from the
published KasmVNC source snapshot. The source bundle preserves dependency
inputs, but `builder/build-portable` still uses online repositories. Reproducing
the recorded inputs requires restoring the included archives and Git revisions
(in particular the resolved `libyuv` revision), not fetching floating branches.
CRT-only entries such as `glibc-devel` or a GCC compiler package describe
embedded startup objects; they do not mean all of libc or GCC is linked in.

### Reusing compiled dependencies

The glibc portable jobs use an independently cached GCC-runtime parent image
and an application-dependency child image per architecture. Child images live in
`$CI_REGISTRY_IMAGE/portable-deps` with tags
`portable-deps-amd64-<sha256>` and `portable-deps-arm64-<sha256>`. Their key covers
**every filename, Git executable bit and byte** in
`builder/scripts/portable-dependencies/`, the architecture, and the resolved
GCC-runtime parent digest/key. The parent is described below.

Symlinks are rejected. Docker receives a canonical tar context with 0644/0755
file modes and normalized owner/timestamps, so runner checkout umasks do not
change cache keys or image input permissions. Add dependency inputs inside their
own context; do not read application files or introduce untracked build-argument
overrides. Application commits, web assets, branch names and timestamps do not
change the keys. Compatibility wrappers keep existing build entry points working.

A native GitLab `resource_group` serializes the cache lookup and publication for
each architecture across branches. While holding that lock, a registry manifest
HTTP 404 triggers a build and push; authentication, rate limits and network/server
errors fail the job. Existing images are pulled by manifest digest. The image is
published only after all dependency compiles and mandatory validation complete.
Both cold and warm builds then create a fresh application image from the current
checkout, and generate the SBOM, source bundle, debug symbols and vulnerability
scan for that application revision. No KasmVNC application source or frontend is
baked into the shared dependency image. Local builds use the same context and
Docker layer cache without publishing anything.

The image contains compiled GCC unwind runtime, desktop/runtime libraries,
FFmpeg/codecs and the JPEG/TBB/cpuid/fmt/libyuv/libstatgrab helper dependencies.
Xorg and KasmVNC are built together for each application commit because the Xorg
patches integrate application code. Sources and license evidence are retained
inside the image for later source bundles; the Grype database is refreshed for
each final package scan, independent of the cache.

Both portable profiles use Xorg 21.1.25. Maintain this source pin independently
of the libc compatibility baseline: targeting older libc does not require an
older X server. Updating Xorg rebuilds the application but does not invalidate
the dependency image cache.

The Oracle Linux base is pinned by multiarchitecture digest. RPM repositories
are resolved when the image is created; this is **not** a bit-reproducible RPM
snapshot. Update `builder/scripts/portable-gcc-runtime/repository-refresh.txt` deliberately to refresh RPMs, and update
the base digest/source pins when needed for security maintenance. A deleted image
rebuilt under the same recipe key can resolve newer RPMs. The published image
digest and exact RPM inventory in each package's provenance identify what was
actually used. The recipe key identifies the inputs we control, not a guarantee
that external repositories never change.

To bound registry storage, configure GitLab's project-wide tag cleanup policy
with the reserved regex `^portable-deps-(amd64|arm64)-[0-9a-f]{64}$`, an age
threshold (for example 30 days), and a recent-tag retention count (for example
10). The count is shared across matched architectures; it does not guarantee a
minimum for each architecture. These tags live in the dedicated `portable-deps`
repository and the prefix avoids selecting unrelated image tags elsewhere in
the project. Before cleanup, preserve builders used by supported releases with
additional `portable-release-<version>-<arch>` tags, excluded from that regex,
and configure protection rules for those release tags.

Configure this with the GitLab administrator; this change does not delete images
or alter the project-wide cleanup policy. There are no per-commit or per-pipeline
tags and no second registry layer-cache repository. Deleted development images
rebuild on demand. Keep published source bundles and release provenance in S3
independently of builder-image cleanup. Storage is reclaimed according to the
registry's garbage-collection policy, not necessarily immediately when a tag is
removed.

### Independent glibc unwind-runtime cache

The patched GCC runtime and its pinned Oracle Linux build environment use a
separate `portable-gcc-runtime` registry image, keyed by architecture and every
file in `builder/scripts/portable-gcc-runtime`. The main glibc dependency image
inherits its immutable digest; both the parent digest and recipe key are included
in the child cache context and packaged provenance. Unrelated runtime/codec recipe
changes therefore reuse libgcc. Musl never resolves or builds this image.

The first run populates the new cache. Later toolchain hits avoid compiling GCC
again, even if other dependencies change. Authentication/network errors fail
closed; only a registry manifest 404 permits rebuilding. Bump the toolchain's
`repository-refresh.txt` to refresh its RPM environment deliberately. Source,
patches, runtime notices and the toolchain RPM inventory remain available to the
source/SBOM pipeline. No libc ABI, compiler baseline or linkage policy changed.

Container validation: `python3 builder/portable/test-gcc-cache.py` and
`python3 builder/portable/test-dependency-cache.py` in a dedicated test container.

### Application link cost

Portable builds omit the loadable `libvnc` Xorg extension: the DEB, RPM, tar and
APK packages ship the standalone `Xkasmvnc` executable and never install that
extension. Traditional builds retain their existing targets. This avoids a
second link of the static codec archives (including their embedded LTO work)
competing with the executable link for CPU and memory.

The static desktop linker reports wall time, child CPU time and peak child RSS
in the build log. Peak RSS is the largest individual child process, not summed
memory across concurrent LTO workers. These measurements distinguish the final
link from compilation, audit, compression and source-bundle generation without
changing compiler optimization flags or the runtime dependency allowlist.
