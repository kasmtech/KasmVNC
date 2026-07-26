#!/bin/bash

setup_local_dev_mode() {
  local_dev_mode=1

  emulate_ci_variables
}

emulate_ci_variables() {
  CI_COMMIT_SHA="CI_COMMIT_SHA"
  CI_COMMIT_REF_NAME=$(git branch --show-current)
  S3_BUCKET="S3_BUCKET"
}

is_inside_ci() {
  [[ -n "$CI" ]]
}

is_distro_produces_deb_packages() {
  local os="$1"
  local os_codename="$2"

  ls builder/dockerfile*"${os}_${os_codename}"* | grep -qP '.deb.build$'
}

is_kasmvnc_package() {
  local package="$1";

  echo "$package" | grep -qP 'kasmvncserver(_|-)(doc-)?[0-9]'
}

detect_deb_package_arch() {
  local deb_package="$1"
  echo "$deb_package" | sed -e 's/.\+_\([^.]\+\)\.\(d\?\)deb/\1/'
}

find_main_deb_package() {
  local dbgsym_package="$1"

  echo "$dbgsym_package" | sed -e 's/-dbgsym//; s/ddeb/deb/'
}

fetch_xvnc_md5sum_from_main_deb_package() {
  local main_deb_package="$1"
  main_deb_package=$(realpath "$main_deb_package")

  local tmpdir=$(mktemp -d)
  cd "$tmpdir"
  dpkg-deb -e "$main_deb_package"
  cat DEBIAN/md5sums | grep bin/Xkasmvnc | cut -d' ' -f 1
}

fetch_xvnc_md5sum() {
  local dbgsym_package="$1"
  local main_deb_package

  main_deb_package=$(find_main_deb_package "$dbgsym_package");
  xvnc_md5sum=$(fetch_xvnc_md5sum_from_main_deb_package "$main_deb_package")
  if [[ -z "$xvnc_md5sum" ]]; then
    echo >&2 "ERROR: could not obtain Xvnc md5sum"
    exit 1
  fi
}

upload_dbgsym_package_to_crashpad_dir() {
  local dbgsym_package="$1"

  prepare_crashpad_s3_path_for_package "$dbgsym_package"
  upload_to_s3 "$dbgsym_package" "$crashpad_s3_path_for_package" "$S3_BUCKET";
}

prepare_crashpad_s3_path_for_package() {
  local dbgsym_package="$1"

  fetch_xvnc_md5sum "$dbgsym_package"
  crashpad_s3_path_for_package="${s3_crashpad_build_directory}/${xvnc_md5sum}/kasmvncserver-dbgsym.deb";
  echo
  echo "Dbgsym package upload S3 path (to crashpad dir): $crashpad_s3_path_for_package";
}

upload_dbgsym_package_for_distro_to_crashpad_dir() {
  local os="$1"
  local os_codename="$2"

  find_dbgsym_package "$os" "$os_codename"
  upload_dbgsym_package_to_crashpad_dir "$dbgsym_package"
  echo "1 dbgsym package was uploaded"
}

find_dbgsym_package() {
  local os="$1"
  local os_codename="$2"
  local distro_dir="builder/build/${os}_${os_codename}"
  local -a dbgsym_packages

  mapfile -d '' -t dbgsym_packages < <(
    find "$distro_dir" -type f -name '*dbgsym*deb' -print0
  )
  local number_of_dbgsym_packages_found="${#dbgsym_packages[@]}"
  check_only_one_dbgsym_package_was_found "$number_of_dbgsym_packages_found"

  dbgsym_package="${dbgsym_packages[0]}"
}

check_only_one_dbgsym_package_was_found() {
  local number_of_dbgsym_packages_found="$1"

  if [[ "$number_of_dbgsym_packages_found" -eq 1 ]]; then
    return
  fi

  if [[ "$number_of_dbgsym_packages_found" -eq 0 ]]; then
    echo >&2 "ERROR: no dbgsym packages found"
    return 1
  fi

  if [[ "$number_of_dbgsym_packages_found" -gt 1 ]]; then
    echo >&2 "ERROR: more than 1 dbgsym packages found"
    return 1
  fi
}

upload_regular_packages_for_distro_to_build_dir() {
  local os="$1"
  local os_codename="$2"
  local regular_package_upload_count=0

  for regular_package in $(find_regular_packages "$os" "$os_codename"); do
    upload_regular_package_to_build_dir "$regular_package"
    regular_package_upload_count=$((regular_package_upload_count += 1))
  done

  check_regular_packages_were_uploaded "$regular_package_upload_count"
  echo "$regular_package_upload_count regular packages were uploaded"
}

find_regular_packages() {
  local os="$1"
  local os_codename="$2"
  local distro_dir="builder/build/${os}_${os_codename}"

  find "$distro_dir" -type f \( -name '*.deb' -or -name '*.rpm' -or -name '*.apk' \) -not -name '*dbgsym*'
}

upload_regular_package_to_build_dir() {
  local regular_package="$1"

  echo
  prepare_package_in_build_dir_s3_path "$regular_package"
  upload_to_s3 "$regular_package" "$package_in_build_dir_s3_path" "$S3_BUCKET"
  show_package_download_link_in_ci "$package_in_build_dir_s3_path"

  save_package_s3_path_for_index_html "$package_in_build_dir_s3_path"
}

detect_release_version() {
  RELEASE_VERSION=$(.ci/next_release_version "$CI_COMMIT_REF_NAME")
}

show_file_link_in_ci() {
  local link_name="$1"
  local file_url="$2"

  if [[ -n "$local_dev_mode" ]]; then
    return
  fi

  curl --request POST --header "PRIVATE-TOKEN:${GITLAB_API_TOKEN}" -w '\n' "${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/statuses/${CI_COMMIT_SHA}?state=success&name=${link_name}&target_url=${file_url}"
}

show_package_download_link_in_ci() {
  local package_s3_path="$1"
  local upload_link_name_in_ci

  upload_link_name_in_ci=$(basename $package_s3_path | sed 's#kasmvncserver_##' | sed -r 's#_([0-9]{1,3}\.){2}[0-9]{1,2}_\S+?([a-f0-9]{6})##' | sed -r 's#\.(deb|rpm|tgz)##')
  show_file_link_in_ci "$upload_link_name_in_ci" "$UPLOADED_FILE_S3_URL"
}

check_regular_packages_were_uploaded() {
  local regular_package_upload_count="$1"

  if [[ "$regular_package_upload_count" -gt 0 ]]; then
    return
  fi

  echo >&2 "ERROR: no regular packages were uploaded"
  exit 1
}

save_package_s3_path_for_index_html() {
  local package_s3_path="$1"
  local package_filename

  package_filename=$(basename "$package_s3_path")
  mkdir -p "$uploaded_s3_paths_dir"
  echo "$package_s3_path" > "$uploaded_s3_paths_dir/$package_filename"
}

upload_packages() {
  local os="$1"
  local os_codename="$2"

  prepare_to_run_scripts_and_s3_uploads
  if is_distro_produces_deb_packages "$os" "$os_codename"; then
    upload_dbgsym_package_for_distro_to_crashpad_dir "$os" "$os_codename"
  else
    echo "No dbgsym packages for non-deb distros will be uploaded"
  fi
  upload_regular_packages_for_distro_to_build_dir "$os" "$os_codename"
}

detect_alpine_doc_package() {
  is_alpine_doc_package=
  if [[ $package =~ kasmvncserver-doc ]]; then
    is_alpine_doc_package=1
  fi
}

prepare_package_in_build_dir_s3_path() {
  local package="$1";

  prepare_package_s3_name "$package"
  package_in_build_dir_s3_path="$s3_build_directory/$package_s3_name"

  echo "Regular package upload S3 path (to build dir): $package_in_build_dir_s3_path"
}

prepare_package_s3_name() {
  local package="$1";

  if ! is_kasmvnc_package "$package"; then
    package_s3_name=$(basename "$package")
    return
  fi

  .ci/detect_os_arch_package_format "$package" > /tmp/os_arch_package_format;
  source /tmp/os_arch_package_format;
  detect_release_branch

  detect_release_version
  detect_revision "$package" "$OS_ARCH"
  if [ -n "$REVISION" ]; then
    REVISION="_${REVISION}"
  fi

  detect_alpine_doc_package

  if [ -n "$RELEASE_BRANCH" ]; then
    package_s3_name="kasmvncserver${is_alpine_doc_package:+_doc}_${PACKAGE_OS}_${RELEASE_VERSION}${REVISION}_${OS_ARCH}.${PACKAGE_FORMAT}";
  else
    SANITIZED_BRANCH="$(echo $CI_COMMIT_REF_NAME | sed 's/\//_/g')";
    package_s3_name="kasmvncserver${is_alpine_doc_package:+_doc}_${PACKAGE_OS}_${RELEASE_VERSION}_${SANITIZED_BRANCH}_${CI_COMMIT_SHA:0:6}${REVISION}_${OS_ARCH}.${PACKAGE_FORMAT}";
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
  curl --request POST --header "PRIVATE-TOKEN:${GITLAB_API_TOKEN}" -w '\n' "${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/statuses/${CI_COMMIT_SHA}?state=${state}&name=${report_name}&target_url=${report_url}"
}

prepare_kasmvnc_built_packages_to_replace_workspaces_image_packages() {
  cp -r ../output/ubuntu_jammy output/
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

  if [[ -n "$local_dev_mode" ]]; then
    echo "local dev run (no network): upload_to_s3 \"$file_to_upload\" \"$s3_url_for_file\" \"$s3_bucket\""
    return
  fi

  # Transfer to S3
  aws s3 cp "$file_to_upload" \
    "s3://${s3_bucket}/${s3_url_for_file}" \
    --metadata-directive REPLACE \
    --content-type "$(file --mime-type -b \"$file_to_upload\")"
  export UPLOADED_FILE_S3_URL="https://${s3_bucket}.s3.amazonaws.com/${s3_url_for_file}";
};

function prepare_to_run_scripts_and_s3_uploads() {
  if [[ -n "$local_dev_mode" ]]; then
    return
  fi

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

make_package_index_html() {
  readarray -t uploaded_files < <(cat "$uploaded_s3_paths_dir"/*)
  make_package_index_html_from_s3_paths "${uploaded_files[@]}"
}

make_package_index_html_from_s3_paths() {
  local body=""
  local bname

  for package_s3_path in "$@"; do
    bname=$(basename "$package_s3_path")
    body="${body}<a href=/$package_s3_path>$bname</a><br>"
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

upload_package_index_to_build_dir_for_debugging() {
  local package_index="$1"

  upload_to_s3 "$package_index" "$s3_build_directory/index.html" "$S3_BUCKET"
  show_file_link_in_ci "index_for_debugging.html" "$UPLOADED_FILE_S3_URL"
}

if ! is_inside_ci; then
  setup_local_dev_mode
fi

s3_build_directory="kasmvnc/${CI_COMMIT_SHA}"
s3_crashpad_build_directory="kasmvnc/crashpad/${CI_COMMIT_SHA}"
uploaded_s3_paths_dir="output/uploaded_s3_paths"
