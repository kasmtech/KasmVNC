#!/bin/bash

function prepare_upload_filename() {
  local package="$1";

  .ci/detect_os_arch_package_format "$package" > /tmp/os_arch_package_format;
  source /tmp/os_arch_package_format;
  detect_release_branch
  if [ -n "$RELEASE_BRANCH" ]; then
    export upload_filename="kasmvncserver_${PACKAGE_OS}_${RELEASE_VERSION}_${OS_ARCH}.${PACKAGE_FORMAT}";
  else
    export SANITIZED_BRANCH="$(echo $CI_COMMIT_REF_NAME | sed 's/\//_/g')";
    export upload_filename="kasmvncserver_${PACKAGE_OS}_${RELEASE_VERSION}_${SANITIZED_BRANCH}_${CI_COMMIT_SHA:0:6}_${OS_ARCH}.${PACKAGE_FORMAT}";
  fi
};

function upload_to_s3() {
  local package="$1";
  local upload_filename="$2";

  # Transfer to S3
  python3 amazon-s3-bitbucket-pipelines-python/s3_upload.py "${S3_BUCKET}" "$package" "${S3_BUILD_DIRECTORY}/${upload_filename}";
  # Use the Gitlab API to tell Gitlab where the artifact was stored
  export S3_URL="https://${S3_BUCKET}.s3.amazonaws.com/${S3_BUILD_DIRECTORY}/${upload_filename}";
  export BUILD_STATUS="{\"key\":\"doc\", \"state\":\"SUCCESSFUL\", \"name\":\"${upload_filename}\", \"url\":\"${S3_URL}\"}";
  curl --request POST --header "PRIVATE-TOKEN:${GITLAB_API_TOKEN}" "${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/statuses/${CI_COMMIT_SHA}?state=success&name=build-url&target_url=${S3_URL}";
};
function prepare_to_run_scripts_and_s3_uploads() {
  export DEBIAN_FRONTEND=noninteractive;
  apt-get update;
  apt-get install -y ruby2.7 git;
  apt-get install -y python3 python3-pip python3-boto3 curl pkg-config libxmlsec1-dev;
  git clone https://bitbucket.org/awslabs/amazon-s3-bitbucket-pipelines-python.git;
};

detect_release_branch() {
  if echo $CI_COMMIT_REF_NAME | grep -Pq '^release/([\d.]+)$'; then
    export RELEASE_BRANCH=1;
  fi
}
