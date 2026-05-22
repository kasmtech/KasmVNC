#!/bin/bash

as_output_owner() {
  sudo -u "#$output_owner_uid" "$@"
}

is_under_sudo() {
  [[ -n "$SUDO_UID" ]]
}

is_sudo_invoker_root() {
  [[ "$SUDO_UID" -eq 0 ]]
}

is_root() {
  [[ "$effective_uid" -eq 0 ]]
}

is_ci() {
  [[ -n "$CI" ]]
}

use_effective_uid_and_gid_for_output_owner() {
  output_owner_uid="$effective_uid"
  output_owner_gid="$effective_gid"
}

use_uid_and_gid_of_sudo_invoker_for_output_owner() {
  output_owner_uid="$SUDO_UID"
  output_owner_gid="$SUDO_GID"
}

use_first_non_system_uid_and_gid_for_output_owner() {
  output_owner_uid=1000
  output_owner_gid=1000

  if host_output_owner_user_exists; then
    return
  fi

  create_host_output_owner_user_and_group
}

host_output_owner_user_exists() {
  getent passwd "$output_owner_uid" >/dev/null
}

create_host_output_owner_user_and_group() {
  groupadd -g "$output_owner_gid" build
  useradd -m -u "$output_owner_uid" -g "$output_owner_gid" build
}

fail_if_sudo_invoker_is_root() {
  if ! is_sudo_invoker_root; then
    return
  fi

  echo >&2 "ERROR: sudo invoker is root. Please call sudo from a non-root user."
  exit 1
}

use_non_root_uid_and_gid_for_output_owner_or_fail() {
  if is_ci; then
    use_first_non_system_uid_and_gid_for_output_owner
    return
  fi

  if is_under_sudo; then
    fail_if_sudo_invoker_is_root

    use_uid_and_gid_of_sudo_invoker_for_output_owner
    return
  fi

  fail_if_there_is_no_way_to_use_non_root
}

fail_if_there_is_no_way_to_use_non_root() {
  echo >&2 "ERROR: please don't run this script as root, use sudo if needed."
  exit 1
}

select_consistent_output_owner_uid_and_gid() {
  if ! is_root; then
    use_effective_uid_and_gid_for_output_owner
    return
  fi

  use_non_root_uid_and_gid_for_output_owner_or_fail
}

effective_uid="$(id -u)"
effective_gid="$(id -g)"
