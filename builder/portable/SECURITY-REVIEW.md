# Portable dependency security review — 2026-10-23

Scope: Grype reports from pipeline 2973265299, commit b26dcae5,
for portable glibc and musl packages on amd64 and arm64. The glibc reports
contain 28 findings each; musl has the same set except the four systemd
findings and the private GCC runtime finding (23 each).

Raw Grype results remain advisory and unfiltered. No findings are suppressed,
no versions are invented to hide backports, and no GitLab findings are dismissed
by this change. Host OS/driver packages require separate deployment scanning.

## Changes

* FreeType 2.14.3, XZ 5.8.3, libXfont2 2.0.9, cJSON 1.7.19.
* Also update libX11 1.8.14, libXi 1.8.4, libXtst 1.2.6 and libXrender
  0.9.13 for the October 2026 X.Org client-library security fixes.
* Backport zlib df84af25dc1942490e1d1c899a07619152a46148 for CVE-2026-85091.
* Backport FFmpeg 2c2f6e96e31795ae95a8f8323a493ffbc493111f and PR 24593
  for CVE-2026-107676/107678. The latter retains all runtime/test changes,
  excluding master-only API version/changelog edits when applying to 9.0.2.
* Preserve upstream patch files alongside corresponding sources and record
  their hashes in the SBOM. Content-keyed dependency caches include patches.
* Run deterministic HDR serialization and bounded-stack PSSH destruction
  regression tests during portable packaging, on both architectures/libcs.
  Both tests fail on original 9.0.2 and pass with the backports.

## Finding review

"Not affected" is package-specific, not a statement about a host installation.
"Not reachable" requires revisiting if new application callers are introduced.
Backported fixes can remain in version-based scanner results.

| Finding | Package | Severity | Assessment | Evidence / action |
|---|---|---|---|---|
| CVE-2025-57052 | cjson | Critical | Updated / not affected | cJSON 1.7.15 to 1.7.19; affected cJSON_Utils.c is not vendored or built. |
| CVE-2026-67215 | cjson | High | Not affected | cJSON JSON Patch utilities are not vendored or built. |
| CVE-2026-67216 | cjson | High | Not reachable | cJSON_Compare remains in upstream 1.7.19, but KasmVNC has no callers; no suppression added. |
| CVE-2026-67217 | cjson | Medium | Not affected | cJSON JSON Patch utilities are not vendored or built. |
| CVE-2026-107675 | ffmpeg | Medium | Not affected | SFTP/libssh support is not enabled; network and autodetection disabled. |
| CVE-2026-107676 | ffmpeg | Medium | Patched | Backport upstream FFmpeg 2c2f6e96 HDR serializer fix; version remains 9.0.2. |
| CVE-2026-107677 | ffmpeg | Medium | Not affected | DASH demuxer requires libxml2, not enabled with autodetection disabled. |
| CVE-2026-107678 | ffmpeg | Medium | Patched | Backport FFmpeg PR 24593: iterative free and linear MOV pssh append; version remains 9.0.2. |
| CVE-2026-107696 | ffmpeg | High | Not affected | RTSP requires network support, explicitly disabled. |
| CVE-2026-23865 | freetype | Medium | Updated | FreeType 2.13.2 to 2.14.3, containing the 2.14.2 fix. |
| CVE-2023-4039 | gcc | Medium | Not affected | Compiler code-generation hardening issue, not embedded libgcc runtime. GCC 14.2 postdates the fix; no compiler shipped. |
| CVE-2026-56001 | libXfont2 | High | Updated | libXfont2 2.0.6 to 2.0.9. |
| CVE-2026-56002 | libXfont2 | High | Updated | libXfont2 2.0.6 to 2.0.9. |
| CVE-2026-56003 | libXfont2 | High | Updated | libXfont2 2.0.6 to 2.0.9. |
| CVE-2023-37769 | pixman | Medium | Not affected | Pixman stress-test floating-point exception; test executable is not shipped. Debian classifies no security impact. |
| CVE-2025-4598 | systemd | Medium | Not affected | systemd-coredump is not built or shipped. |
| CVE-2026-29111 | systemd | Medium | Not affected | systemd PID 1 is not shipped; only the static libsystemd client archive is built. |
| CVE-2026-40225 | systemd | Medium | Not affected | systemd-udevd is not built or shipped. |
| CVE-2026-40226 | systemd | Medium | Not affected | systemd-nspawn is not built or shipped. |
| CVE-2020-25697 | xorg-server | High | Unresolved design limitation | X11 clients do not authenticate servers. No upstream fix; session isolation remains important. |
| CVE-2022-3553 | xorg-server | Medium | Not affected | macOS XQuartz component is not built; fixed upstream since 21.1.4. |
| CVE-2023-5574 | xorg-server | High | Unresolved / needs targeted reproduction | Although Xvfb is disabled, KasmVNC inherits framebuffer code and exposes multiple protocol screens. Upstream MR 1189 changes shared screen teardown; cannot dismiss based on disabled Xvfb alone. Default launcher uses one screen. |
| CVE-2026-34000 | xorg-server | Critical | Already fixed | Xorg 21.1.25 includes 21.1.22 XKB geometry fix. |
| CVE-2026-34002 | xorg-server | Critical | Already fixed | Xorg 21.1.25 includes 21.1.22 XKB modifier fix. |
| CVE-2026-55999 | xorg-server | High | Already fixed | Xorg 21.1.25 includes 21.1.24 fix; advisory incorrectly says 21.2.24. |
| CVE-2026-56000 | xorg-server | High | Already fixed | Xorg 21.1.25 includes 21.1.24 GLX fix; advisory incorrectly says 21.2.24. |
| CVE-2026-34743 | xz | Medium | Updated | XZ 5.8.1 to 5.8.3. |
| CVE-2026-85091 | zlib | High | Patched | Backport upstream zlib df84af25; version remains 1.3.2. |

## Evidence and limits

* [X.Org security advisories](https://www.x.org/Development/Security/) identify
  the actual fixed releases: 21.1.22 and 21.1.24, already included in 21.1.25.
  The July advisory's `21.2.24` version in some CVE feeds is a typo.
* [FreeType fix](https://gitlab.freedesktop.org/freetype/freetype/-/commit/fc85a255849229c024c8e65f536fe1875d84841c),
  [XZ advisory](https://github.com/tukaani-project/xz/security/advisories/GHSA-x872-m794-cxhv),
  [zlib fix](https://github.com/madler/zlib/commit/df84af25dc1942490e1d1c899a07619152a46148).
* [FFmpeg HDR fix](https://github.com/FFmpeg/FFmpeg/commit/2c2f6e96e31795ae95a8f8323a493ffbc493111f)
  and [PSSH fix/review](https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/24593).
  `build-static-ffmpeg` disables network and autodetection, does not enable
  libssh/libxml2; confirm `CONFIG_LIBSSH_PROTOCOL`, `CONFIG_RTSP_DEMUXER` and
  `CONFIG_DASH_DEMUXER` remain zero when changing that recipe.
* cJSON utilities are not present in `common/network/CMakeLists.txt` or the
  vendor files. `cJSON_Compare` has no KasmVNC callers. Updating cJSON does not
  claim to fix the three still-open 2026 advisories in the upstream library.
* The systemd recipe builds only `libsystemd.a`; no PID 1, coredump, udev or
  nspawn executable is included.
* [Pixman classification](https://security-tracker.debian.org/tracker/CVE-2023-37769)
  identifies the stress-test issue as having no security impact.
* [GCC issue](https://security-tracker.debian.org/tracker/CVE-2023-4039) concerns
  AArch64 compiler stack protection, not libgcc. The portable package embeds
  GCC 14.2 runtime archives, not a compiler.
* [X11 authentication design limitation](https://security-tracker.debian.org/tracker/CVE-2020-25697)
  has no upstream fix. Browser TLS/authentication does not repair X11's
  internal client-to-server trust model.
* [Multiscreen teardown issue](https://security-tracker.debian.org/tracker/CVE-2023-5574)
  remains unresolved upstream. [Proposed upstream changes](https://gitlab.freedesktop.org/xorg/xserver/-/merge_requests/1189)
  affect shared fb/mi teardown. KasmVNC accepts `-screen` and shares that code;
  disabling Xvfb is not sufficient evidence of non-applicability. A targeted
  multiscreen reset/shutdown reproduction is needed before safely changing it.

Local validation evidence and per-pipeline monitoring records are retained in
the VNC-680 task's `validation/security-remediation` directory. A green build
or a smaller raw finding count is not a claim that every finding is resolved.
