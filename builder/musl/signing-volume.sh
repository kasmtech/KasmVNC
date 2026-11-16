# Sourced by build-portable after its EXIT cleanup trap is installed.
# Caller owns signing_directory, signing_helper and signing_volume cleanup.
prepare_signing_volume() {
  local packaging_image=$1
  # A volume works with remote/DinD daemons too. Secrets never enter Docker
  # Config.Env, image layers, build contexts, or command arguments.
  signing_directory=$(mktemp -d /tmp/kasm-apk-signing.XXXXXX)
  python3 builder/musl/signing-keys.py "$signing_directory/keys"
  unset KASMVNC_ALPINE_PRIVATE_KEY KASMVNC_ALPINE_PUBLIC_KEY
  signing_volume=$(docker volume create)
  signing_helper=$(docker create --entrypoint /bin/true \
    --mount "type=volume,src=$signing_volume,dst=/run/kasm-signing" "$packaging_image")
  docker cp "$signing_directory/keys/." "$signing_helper:/run/kasm-signing/"
  docker rm "$signing_helper" >> "$log" 2>&1
  signing_helper=
  rm -rf "$signing_directory"
  signing_directory=

}

cleanup_signing_volume() {
  local failed=0
  if [[ -n "$signing_helper" ]]; then
    docker rm -f "$signing_helper" >> "$log" 2>&1 || failed=1
  fi
  if [[ -n "$signing_volume" ]]; then
    docker volume rm "$signing_volume" >> "$log" 2>&1 || failed=1
  fi
  if [[ -n "$signing_directory" ]]; then
    rm -rf "$signing_directory" || failed=1
  fi
  if [[ $failed != 0 ]]; then
    echo 'Failed to remove APK signing resources; inspect build log and clean up the signing volume' >&2
  fi
  return "$failed"
}
