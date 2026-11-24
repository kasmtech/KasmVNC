const { chromium } = require('/opt/portable-browser/node_modules/playwright');
const fs = require('fs');
(async () => {
  const browser = await chromium.launch({executablePath: '/usr/bin/chromium', args: ['--no-sandbox']});
  const page = await browser.newPage({viewport: {width: 1280, height: 900}, ignoreHTTPSErrors: true,
    httpCredentials: {username: 'kasm_test', password: 'portableTest42'}});
  // Observe the browser's real WebRTC media path (ICE/DTLS-SRTP video track)
  // without reading private noVNC state or replacing its transport.
  await page.addInitScript(() => {
    const OriginalPeer = window.RTCPeerConnection;
    window.portableRTC = {states: [], tracks: [], peers: []};
    window.RTCPeerConnection = class extends OriginalPeer {
      constructor(...args) {
        super(...args);
        window.portableRTC.peers.push(this);
        this.addEventListener('connectionstatechange', () => {
          window.portableRTC.states.push(this.connectionState);
        });
        this.addEventListener('track', event => {
          window.portableRTC.tracks.push({kind: event.track.kind, state: event.track.readyState});
        });
      }
    };
  });
  // Summarize inbound RTP video across peers; peer objects are not serializable.
  const rtcSummary = () => page.evaluate(async () => {
    const rtc = window.portableRTC;
    let bytesReceived = 0, framesDecoded = 0, packetsReceived = 0;
    for (const pc of rtc.peers) {
      try {
        (await pc.getStats()).forEach(r => {
          if (r.type === 'inbound-rtp' && r.kind === 'video') {
            bytesReceived += r.bytesReceived || 0;
            framesDecoded += r.framesDecoded || 0;
            packetsReceived += r.packetsReceived || 0;
          }
        });
      } catch (_) {}
    }
    return {states: rtc.states, tracks: rtc.tracks, bytesReceived, framesDecoded, packetsReceived};
  });
  const messages = [];
  let frames = 0, bytes = 0;
  page.on('console', m => messages.push(m.text()));
  page.on('pageerror', e => messages.push('PAGEERROR: ' + e.message));
  page.on('websocket', ws => ws.on('framereceived', e => {frames++; bytes += e.payload.length;}));
  try {
    await page.goto('https://127.0.0.1:6901/?autoconnect=true&resize=remote&logging=debug');
    await page.locator('html.noVNC_connected').waitFor({timeout: 45000});
    const capability = await page.evaluate(async () =>
      (await VideoDecoder.isConfigSupported({codec: 'avc1.42E01E', codedWidth: 640, codedHeight: 480})).supported);
    if (!capability) throw new Error('Test browser lacks required software H.264 decoder');
    const options = page.locator('#noVNC_setting_stream_mode option');
    await options.nth(1).waitFor({state: 'attached', timeout: 30000});
    const labels = await options.allTextContents();
    const codec = labels.find(s => s.includes('SW H.264'));
    if (!codec) throw new Error('Software H.264 unavailable: ' + JSON.stringify(labels));
    await page.locator('#noVNC_setting_stream_mode').selectOption({label: codec}, {force: true});
    const before = bytes;
    await page.waitForTimeout(5000);
    if (bytes - before < 1000) throw new Error('No sustained encoded framebuffer traffic');
    const canvas = page.locator('#noVNC_container canvas').first();
    await canvas.click({position: {x: 180, y: 120}});
    // X/Openbox focus changes are asynchronous to browser pointer delivery.
    // Observe the real remote input target before sending the first key.
    let focus;
    const clickedAt = Date.now();
    for (let attempt = 0; attempt < 100; attempt++) {
      try { focus = JSON.parse(fs.readFileSync('/probe/focus.json', 'utf8')); } catch (_) {}
      if (focus?.focused && focus.observedAt >= clickedAt) break;
      await page.waitForTimeout(100);
    }
    fs.writeFileSync('/results/input-focus.json', JSON.stringify(focus ?? null, null, 2));
    if (!focus?.focused || focus.observedAt < clickedAt) throw new Error('Remote terminal did not acquire keyboard focus');
    await page.keyboard.type('printf portable-input-ok > /home/kasm_test/input-proof', {delay: 20});
    await page.keyboard.press('Enter');
    await page.waitForTimeout(1000);
    // WebRTC carries only video codecs (RTP); keep SW H.264 selected.
    // The standalone client exposes this through Settings. Workspaces' parent
    // message listener is only registered when the client is inside an iframe.
    if (!(await page.locator('#noVNC_control_bar.noVNC_open').count())) {
      await page.locator('#noVNC_control_bar_handle').click();
    }
    await page.locator('#noVNC_settings_button').click();
    await page.locator('label:has(#noVNC_setting_enable_webrtc)').click();
    if (!(await page.locator('#noVNC_setting_enable_webrtc').isChecked())) {
      throw new Error('WebRTC setting did not enable');
    }
    let udp;
    const rtcDeadline = Date.now() + 30000;
    for (;;) {
      udp = await rtcSummary();
      if (udp.states.includes('connected') && udp.tracks.some(t => t.kind === 'video') &&
          udp.framesDecoded >= 3 && udp.bytesReceived > 1000) break;
      if (Date.now() > rtcDeadline) throw new Error('WebRTC video did not flow: ' + JSON.stringify(udp));
      await page.waitForTimeout(500);
    }
    fs.writeFileSync('/results/udp.json', JSON.stringify(udp, null, 2));
    await page.locator('html.noVNC_connected').waitFor({timeout: 5000});
    await page.screenshot({path: '/results/browser.png'});
    fs.writeFileSync('/results/browser.json', JSON.stringify({pass: true, codec, frames, bytes, bytesAfterSelection: bytes-before, udp}, null, 2));
  } catch (e) {
    fs.writeFileSync('/results/browser.json', JSON.stringify({pass: false, error: String(e), frames, bytes}, null, 2));
    await page.screenshot({path: '/results/browser-failed.png'}).catch(() => {});
    process.exitCode = 1;
  } finally {
    const udp = await rtcSummary().catch(() => null);
    fs.writeFileSync('/results/udp.json', JSON.stringify(udp, null, 2));
    fs.writeFileSync('/results/browser.log', messages.join('\n'));
    await browser.close();
  }
})();
