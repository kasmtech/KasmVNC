VNC_PORT=8443
core_dumps_dir_inside_container="/core_dumps"
core_dumps_dir_on_host="run_test/core_dumps"
core_dumps_dir_volume_option="-v ${PWD}/${core_dumps_dir_on_host}:/${core_dumps_dir_inside_container}"

detect_build_dir() {
  if [ -n "$CI" ]; then
    build_dir=output
  else
    build_dir=builder/build
  fi
}

detect_interactive() {
  if [ -z "$run_test" ]; then
    interactive=-it
  fi
}

detect_package_format() {
  package_format=rpm
  if ls builder/dockerfile*"$os"* | grep -q .deb.build; then
    package_format=deb
  elif ls builder/dockerfile*"$os"* | grep -q .apk.build; then
    package_format=apk
  fi
}
