#!/bin/bash

is_kasmvnc() {
  local package="$1";

  echo "$package" | grep -qP 'kasmvncserver(_|-)(doc-)?[0-9]'
}

detect_deb_package_arch() {
  local deb_package="$1"
  echo "$deb_package" | sed -e 's/.\+_\([^.]\+\)\.\(d\?\)deb/\1/'
}

find_deb_package() {
  local dbgsym_package="$1"

  echo "$dbgsym_package" | sed -e 's/-dbgsym//; s/ddeb/deb/'
}

fetch_xvnc_md5sum() {
  local deb_package="$1"
  deb_package=$(realpath "$deb_package")

  local tmpdir=$(mktemp -d)
  cd "$tmpdir"
  dpkg-deb -e "$deb_package"
  cat DEBIAN/md5sums | grep bin/Xkasmvnc | cut -d' ' -f 1
}

detect_alpine_doc_package() {
  is_alpine_doc_package=
  if [[ $package =~ kasmvncserver-doc ]]; then
    is_alpine_doc_package=1
  fi
}

function prepare_upload_filename() {
  local package="$1";

  if ! is_kasmvnc "$package"; then
    export upload_filename="$package"
    return
  fi

  .ci/detect_os_arch_package_format "$package" > /tmp/os_arch_package_format;
  source /tmp/os_arch_package_format;
  detect_release_branch

  detect_revision "$package" "$OS_ARCH"
  if [ -n "$REVISION" ]; then
    REVISION="_${REVISION}"
  fi

  detect_alpine_doc_package

  if [ -n "$RELEASE_BRANCH" ]; then
    export upload_filename="kasmvncserver${is_alpine_doc_package:+_doc}_${PACKAGE_OS}_${RELEASE_VERSION}${REVISION}_${OS_ARCH}.${PACKAGE_FORMAT}";
  else
    export SANITIZED_BRANCH="$(echo $CI_COMMIT_REF_NAME | sed 's/\//_/g')";
    export upload_filename="kasmvncserver${is_alpine_doc_package:+_doc}_${PACKAGE_OS}_${RELEASE_VERSION}_${SANITIZED_BRANCH}_${CI_COMMIT_SHA:0:6}${REVISION}_${OS_ARCH}.${PACKAGE_FORMAT}";
  fi
};

list_files_in_directory() {
  local dir="$1"
  find "$1" -mindepth 1
}

upload_directory_to_s3() {
  local dir_to_upload="$1"
  local s3_directory="$2";
  local s3_bucket="$3";

  for file_to_upload in $(list_files_in_directory "$dir_to_upload"); do
    upload_to_s3 "$file_to_upload" "$s3_directory/$file_to_upload" "$s3_bucket"
  done
}

prepare_functional_tests_source_and_cd_into_it() {
  cd kasmvnc-functional-tests
  mkdir output && chown 1000:1000 output
  mkdir report && chown 1000:1000 report
}

upload_report_to_s3() {
  s3_tests_directory="kasmvnc/${CI_COMMIT_SHA}/tests"
  upload_directory_to_s3 report "$s3_tests_directory" "$S3_BUCKET"
}

put_report_into_ci_pipeline() {
  local functional_tests_exit_code="$1"
  local report_name="Functional%20test%20report"
  local report_url="https://${S3_BUCKET}.s3.amazonaws.com/${s3_tests_directory}/report/index.html"
  local state="success"
  if [ "$functional_tests_exit_code" -ne 0 ]; then
    state="failed"
  fi
  curl --request POST --header "PRIVATE-TOKEN:${GITLAB_API_TOKEN}" "${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/statuses/${CI_COMMIT_SHA}?state=${state}&name=${report_name}&target_url=${report_url}"
}

prepare_kasmvnc_built_packages_to_replace_workspaces_image_packages() {
  cp -r ../output/jammy output/
}

prepare_to_run_functional_tests() {
  install_packages_needed_for_functional_tests
  prepare_functional_tests_source_and_cd_into_it
  prepare_kasmvnc_built_packages_to_replace_workspaces_image_packages
  heed_debug_variable_and_toggle_debug_in_functional_tests
}

heed_debug_variable_and_toggle_debug_in_functional_tests() {
  if [ -z "$CI" ]; then
    return
  fi

  if [ "$DEBUG" = "true" ]; then
    export KASMVNC_FUNC_TESTS_DEBUG=1
  fi
}

install_packages_needed_for_functional_tests() {
  prepare_to_run_scripts_and_s3_uploads
  apt-get install -y tree docker.io
}

is_build_this_distro() {
  local distro="$1"
  [[ "$BUILD_DISTROS_REGEX" = 'all' ]] || [[ "$distro" =~ $BUILD_DISTROS_REGEX ]]
}

function upload_to_s3() {
  local file_to_upload="$1";
  local s3_url_for_file="$2";
  local s3_bucket="$3";

  # Transfer to S3
  aws s3 cp "$file_to_upload" \
    "s3://${S3_BUCKET}/${s3_url_for_file}" \
    --metadata-directive REPLACE \
    --content-type "$(file --mime-type -b \"$file_to_upload\")"
  # Use the Gitlab API to tell Gitlab where the artifact was stored
  export S3_URL="https://${s3_bucket}.s3.amazonaws.com/${s3_url_for_file}";
};

function prepare_to_run_scripts_and_s3_uploads() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y ruby wget curl file awscli git
}

detect_release_branch() {
  if echo $CI_COMMIT_REF_NAME | grep -Pq '^release/([\d.]+)$'; then
    export RELEASE_BRANCH=1;
  fi
}

detect_revision() {
  local package="$1"
  local arch="$2"

  REVISION=

  if ! echo "$package" | grep -q '+'; then
    return
  fi

  REVISION=$(echo "$package" | sed "s/_${arch}.\+//" | sed 's/.\++//')
}

make_index_html() {
  local body=""
  local bname

  for f in "$@"; do
    bname=$(basename "$f")
    body="${body}<a href=/$f>$bname</a><br>"
  done

  cat <<EOF
<!doctype html>
<html lang=en>
<head>
	<meta charset=utf-8>
	<title>KasmVNC preview build</title>
</head>
<body>
$body
</body>
</html>
EOF
}
