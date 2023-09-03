#!/bin/bash

set -e

xstartup_script=~/.vnc/xstartup
de_was_selected_file="$HOME/.vnc/.de-was-selected"

debug() {
  if [ -z "$debug" ]; then return; fi

  echo "$@"
}

enable_debug() {
  debug=1
}

process_cli_options() {
  while [ $# -gt 0 ]; do
    local option="$1"
    shift

    case "$option" in
      --help|-h)
        show_help
        exit
        ;;
      -d|--debug)
        enable_debug
        ;;
      -y|--assume-yes)
        assume_yes=1
        ;;
      -s|--select-de)
        action=select-de
        if [[ -n "$1" && "${1:0:1}" != "-" ]]; then
          selected_de="$1"
          assume_yes_for_xstartup_overwrite=1
          if [ "$selected_de" = "manual" ]; then
            selected_de="$manual_xstartup_choice"
          fi
          shift
        fi
        ;;
      *)
        echo >&2 "Unsupported argument: $option"
        exit 1
    esac
  done
}

show_help() {
  cat >&2 <<-USAGE
Usage: $(basename "$0") [options]
  -d, --debug      Debug output
  -y, --assume-yes Automatic "yes" to prompts
  -s, --select-de  Select desktop environent to run
  --help           Show this help
USAGE
}

add_uppercase_desktop_environment_keys() {
  local de_cmd

  for de in "${!all_desktop_environments[@]}"; do
    de_cmd=${all_desktop_environments[$de]};
    all_desktop_environments[${de^^}]="$de_cmd"
  done
}

manual_xstartup_choice="Manually edit xstartup"

process_cli_options "$@"

declare -A all_desktop_environments=(
  [Cinnamon]="exec cinnamon-session"
  [Mate]="XDG_CURRENT_DESKTOP=MATE exec dbus-launch --exit-with-session mate-session"
  [LXDE]="exec lxsession"
  [Lxqt]="exec startlxqt"
  [KDE]="exec startkde"
  [Gnome]="XDG_CURRENT_DESKTOP=GNOME exec dbus-launch --exit-with-session /usr/bin/gnome-session"
  [XFCE]="exec xfce4-session")

readarray -t sorted_desktop_environments < <(for de in "${!all_desktop_environments[@]}"; do echo "$de"; done | sort)

all_desktop_environments[$manual_xstartup_choice]=""
sorted_desktop_environments+=("$manual_xstartup_choice")
add_uppercase_desktop_environment_keys

detected_desktop_environments=()
declare -A numbered_desktop_environments

print_detected_desktop_environments() {
  declare -i i=1

  echo "Please choose Desktop Environment to run:"
  for detected_de in "${detected_desktop_environments[@]}"; do
    echo "[$i] $detected_de"
    numbered_desktop_environments[$i]=$detected_de
    i+=1
  done
}

detect_desktop_environments() {
  for de_name in "${sorted_desktop_environments[@]}"; do
    if [[ "$de_name" = "$manual_xstartup_choice" ]]; then
      detected_desktop_environments+=("$de_name")
      continue;
    fi

    local executable=${all_desktop_environments[$de_name]}
    executable=($executable)
    executable=${executable[-1]}

    if detect_desktop_environment "$de_name" "$executable"; then
      detected_desktop_environments+=("$de_name")
    fi
  done
}

ask_user_to_choose_de() {
  while : ; do
    print_detected_desktop_environments
    read -r de_number_to_run
    if [[ -z "$de_number_to_run" ]]; then
      continue
    fi
    de_name_from_number "$de_number_to_run"
    if [[ -n "$de_name" ]]; then
      break;
    fi

    echo "Incorrect number: $de_number_to_run"
    echo
  done
}

remember_de_choice() {
  touch "$de_was_selected_file"
}

de_was_selected_on_previous_run() {
  [[ -f "$de_was_selected_file" ]]
}

detect_desktop_environment() {
  local de_name="$1"
  local executable="$2"

  if command -v "$executable" &>/dev/null; then
    return 0
  fi

  return 1
}

did_user_forbid_replacing_xstartup() {
  grep -q -v KasmVNC-safe-to-replace-this-file "$xstartup_script"
}

de_cmd_from_name() {
  de_cmd=${all_desktop_environments[${de_name^^}]}
}

de_name_from_number() {
  local de_number_to_run="$1"

  de_name=${numbered_desktop_environments[$de_number_to_run]}
}

warn_xstartup_will_be_overwriten() {
  if [[ -n "$assume_yes" || -n "$assume_yes_for_xstartup_overwrite" ]]; then
    return 0
  fi

  if [ ! -f "$xstartup_script" ]; then
    return 0
  fi

  echo -n "WARNING: $xstartup_script will be overwritten y/N?"
  read -r do_overwrite_xstartup
  if [[ "$do_overwrite_xstartup" = "y" || "$do_overwrite_xstartup" = "Y" ]]; then
    return 0
  fi

  return 1
}

setup_de_to_run_via_xstartup() {
  warn_xstartup_will_be_overwriten
  generate_xstartup "$de_name"
}

generate_xstartup() {
  local de_name="$1"

  de_cmd_from_name

  cat <<-SCRIPT > "$xstartup_script"
#!/bin/sh
set -x
$de_cmd
SCRIPT
  chmod +x "$xstartup_script"
}

user_asked_to_select_de() {
  [[ "$action" = "select-de" ]]
}

user_specified_de() {
  [ -n "$selected_de" ]
}

check_de_name_is_valid() {
  local selected_de="$1"
  if [[ "$selected_de" = "$manual_xstartup_choice" ]]; then
    return 0;
  fi

  local de_cmd=${all_desktop_environments["${selected_de^^}"]:-}
  if [ -z "$de_cmd" ]; then
    echo >&2 "'$selected_de': not supported Desktop Environment"
    return 1
  fi
}

de_installed() {
  local de_name="${1^^}"

  for de in "${detected_desktop_environments[@]}"; do
    if [ "${de^^}" = "$de_name" ]; then
      return 0
    fi
  done

  return 1
}

check_de_installed() {
  local de_name="$1"

  if ! de_installed "$de_name"; then
    echo >&2 "'$de_name': Desktop Environment not installed"
    return 1
  fi
}

if user_asked_to_select_de || ! de_was_selected_on_previous_run; then
  if user_specified_de; then
    check_de_name_is_valid "$selected_de"
  fi

  detect_desktop_environments
  if user_specified_de; then
    check_de_installed "$selected_de"
    de_name="$selected_de"
  else
    ask_user_to_choose_de
  fi

  debug "You selected $de_name desktop environment"
  if [[ "$de_name" != "$manual_xstartup_choice" ]]; then
    setup_de_to_run_via_xstartup
  fi
  remember_de_choice
fi
