#!/bin/bash
# Run in a disposable container; no SaaS operations or credentials.
set -euo pipefail
cd /src
export CI=true CI_COMMIT_SHA=abcdef0123456789
source .ci/helpers.sh
local_dev_mode=
prepare_to_run_scripts_and_s3_uploads() { :; }
aws() {
  local args=("$@")
  [[ "$1 $2" == 's3 cp' ]]
  test -s "$3"
  [[ "${args[-2]}" == --content-type ]]
  [[ "${args[-1]}" == application/x-xz ]]
  printf '%s\n' "${4#s3://test/}" >> /tmp/uploads
}
show_file_link_in_ci() { :; }
mkdir -p builder/build/nix_x86_64-linux
printf test | xz > builder/build/nix_x86_64-linux/kasmvncserver_nix_1.5.1_test_abcdef_x86_64-linux.tar.xz
S3_BUCKET=test
upload_nix_packages
grep -Fx 'kasmvnc/abcdef0123456789/kasmvncserver_nix_1.5.1_test_abcdef_x86_64-linux.tar.xz' /tmp/uploads
test -s output/uploaded_s3_paths/kasmvncserver_nix_1.5.1_test_abcdef_x86_64-linux.tar.xz

# Exercise the real MIME detection with a filename containing spaces too.
printf test | xz > '/tmp/package with spaces.tar.xz'
upload_to_s3 '/tmp/package with spaces.tar.xz' 'kasmvnc/test/with-spaces.tar.xz' test
grep -Fx 'kasmvnc/test/with-spaces.tar.xz' /tmp/uploads
