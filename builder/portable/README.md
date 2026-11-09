# Portable KasmVNC packages

These packages contain the same native application build in DEB, RPM and tar
formats. They require glibc 2.28 or newer; musl requires a separate build.
Alpine uses the separate [portable musl APK profile](../musl/README.md).
The EL8 builder targets the x86-64 CPU baseline. ARM64 is built natively. Desktop C libraries, compiler runtimes, OpenSSL 3, GPU
loaders and drivers, timezone data, XKB tools/data and the desktop remain host dependencies.

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

Tar dependencies include the desktop/compiler libraries listed below, glibc,
libGL, libgbm, bash, tzdata, xauth, xdpyinfo,
xkbcomp, XKB data, the openssl command, and the system `libssl.so.3` and
`libcrypto.so.3` libraries. Group creation uses groupadd/getent. Mesa DRI and VAAPI
drivers are supplied by the host. Their default search checks the native Debian
multiarch directory (`/usr/lib/x86_64-linux-gnu/dri` or
`/usr/lib/aarch64-linux-gnu/dri`), then `/usr/lib64/dri` and `/usr/lib/dri`.
Explicit `LIBGL_DRIVERS_PATH` and `LIBVA_DRIVERS_PATH` retain their upstream
override behavior for custom driver locations. These paths do not guarantee
compatibility with every host driver; validate the intended GPU and distro.

All shipped ELF executables are checked for architecture, PIE, unexpected
dynamic dependencies, search paths, and a maximum glibc ABI requirement of 2.28.
The adjacent checksum and provenance files identify the build inputs and ELF
boundary. Installed documentation includes dependency license notices and the
builder RPM inventory (build provenance only). The separate CycloneDX SBOM
and advisory scan reports are described below. Embedded dependencies require rebuilding KasmVNC for security fixes;
host package updates do not patch them. The default FFmpeg adapter mode below
uses host FFmpeg and encoder libraries. A public release still requires review
of corresponding-source distribution and all embedded dependency licenses.

## System desktop and compiler libraries

Portable packages use host libpng, FreeType, pixman, AT-SPI, GLib, D-Bus,
libunwind and X11 client/font libraries. Glibc builds additionally require
libsystemd and **libcrypt.so.1**; EL9/Fedora satisfy the latter with
`libxcrypt-compat`, not the different `libcrypt.so.2` ABI. Musl builds use host
libbsd directly. Parent DSOs own their private dependency closure, including
font compression, regex, ffi, capabilities and internationalization libraries.
We do not pin those private DSOs or embed copies of them.

C++ and exception handling use host `libstdc++.so.6` and `libgcc_s.so.1`.
The ELF audit rejects imports newer than `GLIBCXX_3.4.25`, `CXXABI_1.3.11`
and `GCC_4.5.0`. Optional static FFmpeg builds additionally permit the exact
`__divmodti4@GCC_7.0.0` import, verified in every target distro's runtime;
adapter builds do not permit this exception. EL8's GCC 14 toolset may contribute a small
`libstdc++_nonshared.a` compatibility supplement; selected members, builtin
helpers and CRT objects remain embedded and retain their source/SBOM evidence.
Musl C++ symbols are generally unversioned, so these version ceilings alone
cannot establish compatibility there.

The glibc profile supplies KasmVNC's own small executable entry and `atexit`
adapter and stat-family wrappers from `builder/portable/crt/`. It calls the host's
older `__libc_start_main` ABI (GLIBC_2.2.5 on AMD64; GLIBC_2.17 on ARM64)
and `__cxa_atexit`; an initialization callback preserves constructor arrays.
Stat wrappers call the host's versioned stat interfaces without embedding
`libc_nonshared.a`. The host still owns
libc initialization, threading, allocation and callback execution. GCC's
`crtbeginS.o` and `crtendS.o` remain compiler inputs. All five executable link
maps must exclude glibc's startup objects, selected `libc_nonshared.a` members
and `libc.a` members. Legacy `.init`/`.fini` fragments are rejected instead of
silently losing initialization or cleanup. The normal constructor/destructor
arrays and loader cleanup callback are preserved.

This startup policy applies only to dynamically linked portable glibc builds
on x86_64 and ARM64. It does not change the musl or traditional distro profiles.
The builder still uses glibc development headers. The product SBOM records
actual embedded link inputs; removing the glibc startup objects does not remove
the host libc requirement or imply that builder packages need no security
updates. Changes to the entry code require lifecycle tests on both architectures
and the existing package/runtime matrix, including compiler hardening checks.

Builds use baseline distro development headers and public shared APIs; the
mandatory runtime matrix tests the same packaged binaries on every target.
A compatible SONAME alone is insufficient. Final link maps reject the converted
static archives, and audits require the expected shared libraries on all five
executables. `host-libraries.json` in installed documentation lists the exact
SONAMEs and DEB/APK providers; `elf-audit.json` records the actual imports.
RPM automatically requires the ELF SONAME and symbol capabilities, avoiding
provider-name differences across EL, Fedora and openSUSE. Tar users must install
these providers themselves; the package manager does this for DEB/RPM/APK.

**Application zlib stays private/static.** The measured Ubuntu 22.04 system
zlib cost approximately 22–24% more compression time on several production
Tight/ZRLE stream workloads; this is not a whole-session FPS comparison. Its
symbols are hidden so distro PNG/font DSOs continue to use their own system
zlib. Previously embedded performance libraries such as WebP and JPEG remain
embedded, as does the native launcher's YAML library. These retained inputs
still require KasmVNC rebuilds for applicable security fixes.

Update host packages and restart sessions to apply fixes to shared libraries.
For immutable Workspaces containers, refresh the image and recreate containers;
updating only the Docker host does not update libraries inside the container.
The application SBOM identifies required host components without assigning
builder versions to deployed runtimes. Scan the actual deployed image for the
host libraries and their transitive closure.

## EL8 build and runtime requirements

GCC Toolset 14 retains C++20 and the current Xorg 21.1.25. Pinned xorgproto
2024.1 headers supply newer protocol definitions missing from EL8; they are a
build input only. Their archive, checksum and license notices accompany the
source bundle and provenance. No older Xorg or C++ language level is required.

Oracle/Rocky/AlmaLinux 8 need EPEL's OpenSSL 3 runtime and their development
repository (CodeReady Builder or PowerTools) for desktop dependencies. Video
also needs a compatible host FFmpeg, tested with RPM Fusion's FFmpeg 4.4.
The required minimum is a complete runtime dependency set, not glibc alone.
The compiler is newer than the host C++ runtime; link audits limit imported
symbols to GCC 8's runtime ABI and record any toolset compatibility objects.

## System OpenSSL 3

Both portable profiles dynamically link the distribution's OpenSSL 3 runtime.
The same application binary can use supported OpenSSL 3.x minor releases;
there is no per-minor adapter and no bundled TLS fallback. The four existing
libc/architecture builds remain separate. OpenSSL 1.1 and 4 are different ABIs
and cannot substitute for the required `.so.3` libraries.

Compilation uses the pinned OpenSSL 3.0 SDK in `builder/openssl3/`, independently
of the cached static dependencies. That SDK is a build input only. All five
packaged executables are audited for OpenSSL dependencies, imported symbols and
library search paths. Xkasmvnc must use `libssl.so.3` and `libcrypto.so.3`;
kasmvncpasswd must use `libcrypto.so.3`. Helpers without OpenSSL calls need no
extra dependency. Imports must belong to the original `OPENSSL_3.0.0` ABI;
unversioned imports are compared with the verified SDK's baseline exports.
Final link maps and unstripped symbols also reject embedded OpenSSL code,
including hidden definitions from renamed archives.

DEB requires `libssl3` or `libssl3t64`. RPM uses ELF capabilities, satisfied by
EL/Fedora `openssl-libs` and openSUSE `libopenssl3`. Alpine explicitly requires
`libssl3` and `libcrypto3`. Package managers resolve these dependencies; tar
installations must supply them first. The openssl command remains necessary
for certificate setup.

The host library supplies its providers, configuration and crypto policy.
Do not set module paths to the build SDK or weaken host policy. OpenSSL ABI
compatibility does not freeze allowed algorithms: UDP/WebRTC certificate signing
uses SHA-256 and propagates signing failures, including policy rejections.
Using a system FIPS provider is not a certification of KasmVNC.

Update the distro's OpenSSL packages and restart Xvnc sessions to apply TLS
security fixes. Immutable Workspaces images still need package refresh and
container recreation. This removes the embedded OpenSSL update obligation;
other static libraries still need a KasmVNC rebuild. Follow distro advisories
and support lifecycles, including backports. The SDK's older ABI baseline is
not a recommendation to deploy an unsupported upstream runtime.

The source bundle records the SDK's exact source and manifest under `build-only/`
and `openssl-build-sdk.json`. Provenance identifies the build SDK; the SBOM
records OpenSSL as a required host dependency without assigning the builder's
version to the installed runtime. Scan the deployed image to assess its actual
OpenSSL package. See [OpenSSL's ABI policy](https://openssl-library.org/policies/general/versioning-policy/).

## FFmpeg runtime adapters

`builder/build-portable --ffmpeg-mode adapters` produces one application binary
with isolated adapters for libavcodec families 58 through 63. `--libc musl`
selects the separate Alpine build; it does not make a glibc binary run on musl.
Local and CI builds default to adapter mode for both architectures and libcs.
`--ffmpeg-mode static` retains the separate static-codec experiment.

| Adapter | Host runtime libraries | Example target |
|---|---|---|
| FFmpeg 4.4 | libavcodec.so.58, libavutil.so.56, libswscale.so.5 | Ubuntu 22.04, EL8 with RPM Fusion |
| FFmpeg 5.1 | libavcodec.so.59, libavutil.so.57, libswscale.so.6 | Debian 12 |
| FFmpeg 6.1 | libavcodec.so.60, libavutil.so.58, libswscale.so.7 | Ubuntu 24.04, Alpine 3.21/3.22 |
| FFmpeg 7.1 | libavcodec.so.61, libavutil.so.59, libswscale.so.8 | Debian 13, EL9 with the relevant codec repository |
| FFmpeg 8.0 | libavcodec.so.62, libavutil.so.60, libswscale.so.9 | Ubuntu 26.04, Alpine 3.23/3.24 |
| FFmpeg 9.0 | libavcodec.so.63, libavutil.so.61, libswscale.so.10 | Kali rolling |

File-based benchmarks additionally need the matching libavformat family (58–63).
Install runtime libraries from the host distribution's supported repositories;
the adapter package intentionally does not bundle FFmpeg or its encoder libraries.
The host FFmpeg build must provide the requested encoders. For example,
`ffmpeg-free` may omit software H.264/HEVC encoders. A matching library SONAME
alone does not establish codec availability, GPU support or performance parity.

Absent or unsupported FFmpeg disables video; the other desktop encodings remain
available. Matching FFmpeg patch releases within a supported ABI family can be
updated without rebuilding KasmVNC. Older minor releases than the listed header
baselines and future ABI majors require separate qualification. Runtime
FFmpeg security updates are supplied by the host package manager; restart Xvnc
sessions after library updates. Other embedded dependencies still require a
KasmVNC rebuild.

Headers are pinned and prepared in a separate cheap Docker layer, independently
of the existing dependency image cache. Source bundles include the six exact
header source archives and their hashes. The SBOM distinguishes build headers
from runtime libraries and records FFmpeg as an external requirement; cached
static FFmpeg/x264/x265/SVT archives are not part of adapter-mode artifacts.
See [adapter build details](../ffmpeg-adapters/README.md).

## Portable CI

The default pipeline has four native package producers: glibc and musl on
amd64 and arm64. The glibc build targets glibc 2.28+, using Oracle Linux 8 and
GCC Toolset 14; C++20 and Xorg 21.1.25 are retained. Musl targets 1.2.5+.
There are no legacy package producers or legacy package test consumers.
`BUILD_WORKSPACE_LEGACY` and `BUILD_FULL_LEGACY` have been removed; setting
those obsolete variables cannot re-enable native distro builds.
`BUILD_PORTABLE` has also been removed. `BUILD_DISTROS_REGEX=none` disables
portable packaging; it never falls back to legacy builds. Other regex values
do not narrow the portable runtime matrix.

The required matrix covers 39 glibc and eight Alpine cases (47 distro/architecture
cases, 86 format runs), including Oracle/Rocky/AlmaLinux 8 and Leap 15.6 on both
architectures. Minimal functional and launcher specification tests consume the
portable amd64 DEB. Standard functional qualification adds Oracle Linux 8 with
an asserted glibc 2.28 floor and Kali with glibc 2.43 or newer, each on both kernel
hosts. Existing userspace and Noble/Oracle 9 GPU configurations remain, giving
16 Standard configurations. The functional runner fails if a boundary image no
longer meets its asserted glibc version. Containers still share their host kernel.

Downstream Workspaces installers must use portable asset names and install the
host dependencies. The former optional Noble image jobs consumed native distro
packages and have been removed. Local distro recipes remain for development;
they are not release pipeline products.

The required MR Standard functional job verifies and snapshots the DEB, RPM,
APK, checksum manifests, provenance and pinned APK public key from its own
pipeline. Every host uses that frozen candidate.

Separate build-ID debug symbols are published in DEB, RPM and tar formats, with
checksums and a `.debug.json` mapping. See `DEBUGGING.md` for exact-build matching
and the existing crashpad lookup.

## SBOM, source inputs and advisory vulnerability reports

Each architecture publishes the following alongside its runtime and debug
packages under the same S3 commit directory:

* `.cdx.json`: CycloneDX 1.6 JSON for embedded native components, required host
  OpenSSL, desktop and compiler libraries (with no invented installed versions), vendored code
  and the web client's actually bundled npm dependencies and identified vendored
  DES/base64 code. The same application
  inventory applies to the DEB, RPM and tarball; packaging metadata differs.
* `.sources.tar.gz` and `.sources.json`: exact source inputs and their hashes,
  native link evidence, build revision, host requirements and known limitations.
* `.vulnerabilities.json` and `.vulnerability-summary.json`: every Grype finding,
  scoped applicability assessments, scanner/database metadata and severity counts.
* `.grype.raw.json`: unchanged scanner evidence; use the adjacent assessment
  snapshot for applicability. This is an audit input, not an assessed report.
* `.assessments.json`: regenerated review history and current lifecycle state.
* `.openvex.json`: the same decisions in OpenVEX 0.2 format, bound to the exact
  revision, architecture and libc. SBOM `vulnerabilities[].analysis` records
  current CycloneDX VEX decisions; metadata also retains inactive review history.
* `.vex-applied.json`: the pinned Grype consumer's result using that OpenVEX.
  Exceptions remain visible in `ignoredMatches`; the remaining matches are open.

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

### Persistent security assessments

`builder/portable/security-assessments.json` is the committed review registry.
Each entry names the CVE, exact component versions, architectures/libcs, rationale,
reviewer/date, upstream references, and SHA256s of the reviewed source/recipes.
Change those decisions through normal code review, together with their evidence.
Never broaden a version or refresh an evidence hash without reviewing the claim.

Every successful scan regenerates `.assessments.json`. Matching decisions become
`active`; missing findings become `not_detected` and the previous decision becomes
inactive. A version/scope/evidence change produces `needs_review` and `in_triage`,
not an inherited exception. New findings are `unreviewed`. Absence alone is not
proof of remediation. The registry preserves review history; pipeline snapshots
update observations automatically without a CI bot rewriting Git or racing parallel
architectures. A finding that returns can reuse a decision only within its exact
reviewed scope and unchanged evidence. cJSON caller absence is rechecked for new
source files as well as existing files. Snapshots are checksummed and published
with each revision, so historical and current observations remain available.

GitLab retains every detected finding with the identical assessment in its
description, solution and structured details. `not_affected`/`false_positive`
findings carry its standard informational false-positive flag. These are scanner
assessments, **not GitLab dismissal state**: the dependency report schema cannot
set dismissal state. Raw detection totals therefore remain equal across GitLab
and the annotated Grype report, with applicability counts reported separately.
Resolved/backported and unresolved findings retain their distinct assessments.
No findings are removed or severities rewritten. Inactive records are carried in
the SBOM/snapshot/OpenVEX, not fabricated as currently detected GitLab findings.

Installed packages include the same SBOM, `security-assessments.json`,
`security.openvex.json`, and `SECURITY-REVIEW.md` under
`/usr/share/doc/kasmvncserver`. CI runs `builder/portable/test-security-reports`
before publishing: pinned CycloneDX, OpenVEX and GitLab schema checks plus an
assessment-by-assessment consistency check. `test-security-assessments.py`
exercises disappearance, reappearance, changed versions/evidence, new callers,
and unknown findings. The two unresolved Xorg issues are not exceptions.

Embedded generic component PURLs retain their upstream names/versions and add
`kasmvnc_build`, `arch`, and `libc` qualifiers; `kasmvnc:upstream-purl` preserves
the unqualified identifier. This lets Grype match OpenVEX to our exact embedded
variant despite its CycloneDX reader discarding application-level identity.
CI verifies that Grype moves exactly the reviewed exceptions to `ignoredMatches`,
and that its remaining plus ignored findings equal the complete raw scan.

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

Host desktop and compiler libraries, OpenSSL 3, glibc, Bash, timezone data,
desktop tools/data and GPU loaders/drivers remain host requirements, not builder-versioned runtime components in this SBOM. Scan the
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

Older cached images may contain unused static desktop/GCC archives. They are
not shipped merely because they exist in the builder: selected final link inputs
determine the SBOM. Current builds use system desktop/compiler libraries and
retain the JPEG/TBB/cpuid/fmt/libyuv/libstatgrab helper dependencies, private zlib,
WebP/YAML and optional static FFmpeg/codecs.
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
source/SBOM pipeline. Portable application links no longer select the cached private unwind runtime.
Its manifest remains cached build provenance, separately labelled from the
actual embedded compatibility/startup inputs and host compiler-runtime requirements.

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
