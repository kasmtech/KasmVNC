#!/usr/bin/env bash
# Invoked as the non-root test user; exercises the packaged native launcher.
set -euo pipefail
export HOME=/home/kasm_test DISPLAY=:1 XDG_RUNTIME_DIR=/home/kasm_test/runtime
# Do not let image overrides hide the portable server's DRI search-path bug.
if [[ -v LIBGL_DRIVERS_PATH ]]; then
  echo 'GLX validation requires LIBGL_DRIVERS_PATH to be unset' >&2
  exit 1
fi
mkdir -p "$HOME/.vnc" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
touch "$HOME/.vnc/.de-was-selected"
printf 'portableTest42\nportableTest42\n' | kasmvncpasswd -u kasm_test -wo
openssl req -x509 -nodes -days 1 -newkey rsa:2048 \
  -keyout "$HOME/.vnc/test.pem" -out "$HOME/.vnc/test.pem" -subj /CN=localhost
# The launcher validates YAML TLS paths before translating server flags.
cat > "$HOME/.vnc/kasmvnc.yaml" <<'CONFIG'
network:
  ssl:
    pem_certificate: /home/kasm_test/.vnc/test.pem
    pem_key: /home/kasm_test/.vnc/test.pem
    require_ssl: true
CONFIG
cat > "$HOME/.vnc/xstartup" <<'STARTUP'
#!/usr/bin/env bash
export DISPLAY=:1
openbox &
xterm -title portable-input-test -geometry 80x24+10+10 -e /bin/bash --noprofile --norc &
xterm -geometry 40x10+600+400 -e /bin/bash -c 'while :; do date +%s.%N; sleep 0.1; done' &
wait
STARTUP
chmod +x "$HOME/.vnc/xstartup"
kasmvncserver :1 -geometry 1024x768 -depth 24 -interface 0.0.0.0 \
  -websocketPort 6901 -PublicIP 127.0.0.1 -sslOnly 1 \
  -cert "$HOME/.vnc/test.pem" -key "$HOME/.vnc/test.pem" \
  -KasmPasswordFile "$HOME/.kasmpasswd" -videoCodec h264 \
  -Log '*:stderr:100'
curl -kfsS --retry 30 --retry-delay 1 --retry-connrefused \
  -u kasm_test:portableTest42 https://127.0.0.1:6901/ -o "$HOME/index.html"
[[ -s "$HOME/index.html" ]]
[[ $(curl -ks -o /dev/null -w '%{http_code}' https://127.0.0.1:6901/) = 401 ]]
[[ $(curl -ks -u kasm_test:wrong -o /dev/null -w '%{http_code}' https://127.0.0.1:6901/) = 401 ]]
for tls in 1.2 1.3; do
  curl -kfsS --tlsv"$tls" --tls-max "$tls" -u kasm_test:portableTest42 \
    https://127.0.0.1:6901/ -o /dev/null
done
python3 /portable/openssl-runtime.py "$(pgrep -u "$(id -u)" -x 'Xvnc|Xkasmvnc')" > "$HOME/openssl-runtime.json"
xdpyinfo > "$HOME/xdpyinfo.txt"
# glxinfo can exit zero on missing GLX visuals: require an actual Mesa renderer.
LIBGL_ALWAYS_SOFTWARE=1 glxinfo -B > "$HOME/glxinfo.txt" 2>&1
grep -Eq '^OpenGL renderer string: .*(llvmpipe|softpipe)' "$HOME/glxinfo.txt"
# The current launcher intentionally exits 1 for -list, even on success.
kasmvncserver -list > "$HOME/sessions.txt" || [[ $? = 1 ]]
grep -q ':1' "$HOME/sessions.txt"
python3 /portable/focus.py > "$HOME/.vnc/focus.log" 2>&1 &
printf 'ready\n' > "$HOME/ready"
# Keep container running; orchestrator stops through the launcher after browser test.
tail -f /dev/null
