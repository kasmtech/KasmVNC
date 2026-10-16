#!/bin/bash
# Run in a disposable container; no SaaS operations or credentials.
set -euo pipefail
cd /src
export CI=true CI_COMMIT_SHA=abcdef0123456789
source .ci/helpers.sh
prepare_to_run_scripts_and_s3_uploads() { :; }
upload_to_s3() { printf '%s\n' "$2" >> /tmp/uploads; UPLOADED_FILE_S3_URL="https://example.invalid/$2"; }
show_file_link_in_ci() { :; }
mkdir -p builder/build/nix_x86_64-linux
printf test > builder/build/nix_x86_64-linux/kasmvncserver_nix_1.5.1_test_abcdef_x86_64-linux.tar.xz
S3_BUCKET=test
upload_nix_packages
grep -Fx 'kasmvnc/abcdef0123456789/kasmvncserver_nix_1.5.1_test_abcdef_x86_64-linux.tar.xz' /tmp/uploads
test -s output/uploaded_s3_paths/kasmvncserver_nix_1.5.1_test_abcdef_x86_64-linux.tar.xz
