const { chromium } = require('/opt/portable-browser/node_modules/playwright');
const fs = require('fs');
(async () => {
  const browser = await chromium.launch({executablePath: '/usr/bin/chromium', args: ['--no-sandbox']});
  const page = await browser.newPage({viewport: {width: 1280, height: 900}, ignoreHTTPSErrors: true,
    httpCredentials: {username: 'kasm_test', password: 'portableTest42'}});
  // Observe the browser's real DTLS/data-channel connection without reading
  // private noVNC state or replacing its transport implementation.
  await page.addInitScript(() => {
    const OriginalPeer = window.RTCPeerConnection;
    window.portableRTC = {states: [], messages: 0, bytes: 0, channels: []};
    window.RTCPeerConnection = class extends OriginalPeer {
      constructor(...args) {
        super(...args);
        this.addEventListener('connectionstatechange', () => {
          window.portableRTC.states.push(this.connectionState);
        });
      }
      createDataChannel(...args) {
        const channel = super.createDataChannel(...args);
        const record = {label: channel.label, state: channel.readyState};
        window.portableRTC.channels.push(record);
        channel.addEventListener('open', () => {record.state = channel.readyState;});
        channel.addEventListener('close', () => {record.state = channel.readyState;});
        channel.addEventListener('message', event => {
          window.portableRTC.messages++;
          window.portableRTC.bytes += event.data.byteLength ?? event.data.size ?? event.data.length;
        });
        return channel;
      }
    };
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
    const imageCodec = labels.find(s => s.includes('JPEG/WEBP'));
    if (!imageCodec) throw new Error('Image mode unavailable for the UDP transport check');
    await page.locator('#noVNC_setting_stream_mode').selectOption({label: imageCodec}, {force: true});
    // Use the same public integration message that Workspaces sends.
    await page.evaluate(() => window.postMessage({action: 'enable_webrtc'}, window.location.origin));
    await page.waitForFunction(() => window.portableRTC.states.includes('connected') &&
      window.portableRTC.channels.some(c => c.label === 'webudp' && c.state === 'open') &&
      window.portableRTC.messages >= 3 && window.portableRTC.bytes > 1000, null, {timeout: 30000});
    const udp = await page.evaluate(() => window.portableRTC);
    fs.writeFileSync('/results/udp.json', JSON.stringify(udp, null, 2));
    await page.locator('html.noVNC_connected').waitFor({timeout: 5000});
    await page.screenshot({path: '/results/browser.png'});
    fs.writeFileSync('/results/browser.json', JSON.stringify({pass: true, codec, frames, bytes, bytesAfterSelection: bytes-before, udp}, null, 2));
  } catch (e) {
    fs.writeFileSync('/results/browser.json', JSON.stringify({pass: false, error: String(e), frames, bytes}, null, 2));
    await page.screenshot({path: '/results/browser-failed.png'}).catch(() => {});
    process.exitCode = 1;
  } finally {
    const udp = await page.evaluate(() => window.portableRTC).catch(() => null);
    fs.writeFileSync('/results/udp.json', JSON.stringify(udp, null, 2));
    fs.writeFileSync('/results/browser.log', messages.join('\n'));
    await browser.close();
  }
})();
