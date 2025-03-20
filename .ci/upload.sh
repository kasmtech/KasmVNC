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

function upload_to_s3() {
  local package="$1";
  local upload_filename="$2";
  local s3_bucket="$3";

  # Transfer to S3
  python3 amazon-s3-bitbucket-pipelines-python/s3_upload.py "${s3_bucket}" "$package" "${upload_filename}";
  # Use the Gitlab API to tell Gitlab where the artifact was stored
  export S3_URL="https://${s3_bucket}.s3.amazonaws.com/${upload_filename}";
};

function prepare_to_run_scripts_and_s3_uploads() {
  export DEBIAN_FRONTEND=noninteractive;
  apt-get update;
  apt-get install -y ruby2.7 git wget;
  apt-get install -y python3 python3-pip python3-boto3 curl pkg-config libxmlsec1-dev;
  git clone https://bitbucket.org/awslabs/amazon-s3-bitbucket-pipelines-python.git;
};

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
