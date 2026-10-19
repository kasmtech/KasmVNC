#!/usr/bin/env bash
# Invoked as the non-root test user; exercises the packaged Perl launcher.
set -euo pipefail
export HOME=/home/kasm_test DISPLAY=:1 XDG_RUNTIME_DIR=/home/kasm_test/runtime
mkdir -p "$HOME/.vnc" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
touch "$HOME/.vnc/.de-was-selected"
printf 'portableTest42\nportableTest42\n' | kasmvncpasswd -u kasm_test -wo
openssl req -x509 -nodes -days 1 -newkey rsa:2048 \
  -keyout "$HOME/.vnc/test.pem" -out "$HOME/.vnc/test.pem" -subj /CN=localhost
# The Perl wrapper validates YAML TLS paths before translating server flags.
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
xdpyinfo > "$HOME/xdpyinfo.txt"
# The current launcher intentionally exits 1 for -list, even on success.
kasmvncserver -list > "$HOME/sessions.txt" || [[ $? = 1 ]]
grep -q ':1' "$HOME/sessions.txt"
printf 'ready\n' > "$HOME/ready"
# Keep container running; orchestrator stops through the launcher after browser test.
tail -f /dev/null
