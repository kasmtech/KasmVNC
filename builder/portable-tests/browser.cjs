const { chromium } = require('/opt/portable-browser/node_modules/playwright');
const fs = require('fs');
(async () => {
  const browser = await chromium.launch({executablePath: '/usr/bin/chromium', args: ['--no-sandbox']});
  const page = await browser.newPage({viewport: {width: 1280, height: 900}, ignoreHTTPSErrors: true,
    httpCredentials: {username: 'kasm_test', password: 'portableTest42'}});
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
    await page.keyboard.type('printf portable-input-ok > /home/kasm_test/input-proof', {delay: 20});
    await page.keyboard.press('Enter');
    await page.waitForTimeout(1000);
    await page.screenshot({path: '/results/browser.png'});
    fs.writeFileSync('/results/browser.json', JSON.stringify({pass: true, codec, frames, bytes, bytesAfterSelection: bytes-before}, null, 2));
  } catch (e) {
    fs.writeFileSync('/results/browser.json', JSON.stringify({pass: false, error: String(e), frames, bytes}, null, 2));
    await page.screenshot({path: '/results/browser-failed.png'}).catch(() => {});
    process.exitCode = 1;
  } finally {
    fs.writeFileSync('/results/browser.log', messages.join('\n'));
    await browser.close();
  }
})();
