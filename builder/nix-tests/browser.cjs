const { chromium } = require('playwright');
const fs = require('fs');
(async () => {
  const [url, mode, name] = process.argv.slice(2);
  const browser = await chromium.launch({channel: 'chrome', args: ['--no-sandbox']});
  const page = await browser.newPage({viewport: {width: 1280, height: 900}, ignoreHTTPSErrors: true, httpCredentials: process.env.TEST_PASSWORD ? {username: 'kasm_user', password: process.env.TEST_PASSWORD} : undefined});
  const messages = [];
  let frames = 0, bytes = 0;
  page.on('console', m => messages.push(m.text()));
  page.on('pageerror', e => messages.push('PAGEERROR: ' + e.message));
  page.on('websocket', ws => ws.on('framereceived', event => {
    frames++; bytes += event.payload.length;
  }));
  try {
    await page.goto(url + '/?autoconnect=true&resize=remote&logging=debug');
    await page.locator('html.noVNC_connected').waitFor({timeout: 30000});
    const choices = page.locator('#noVNC_setting_stream_mode option');
    await choices.nth(1).waitFor({state: 'attached', timeout: 30000});
    const labels = await choices.allTextContents();
    const codec = labels.find(t => mode === 'gpu' ? t.includes('H.264') && t.includes('NVENC') : t.includes('SW H.264'));
    if (!codec) throw new Error('Required codec unavailable: ' + JSON.stringify(labels));
    await page.locator('#noVNC_setting_stream_mode').selectOption({label: codec}, {force:true});
    const before = bytes;
    await page.waitForTimeout(5000);
    if (bytes - before < 1000) throw new Error('No sustained framebuffer traffic after codec selection');
    await page.screenshot({path: `/results/${name}.png`});
    fs.writeFileSync(`/results/${name}-browser.json`, JSON.stringify({pass:true, codec, labels, frames, bytes, bytesAfterSelection:bytes-before},null,2));
  } catch (error) {
    try { await page.screenshot({path: `/results/${name}-failed.png`}); } catch (_) {}
    fs.writeFileSync(`/results/${name}-browser.json`, JSON.stringify({pass:false,error:String(error),frames,bytes},null,2));
    process.exitCode = 1;
  } finally {
    fs.writeFileSync(`/results/${name}-browser.log`, messages.join('\n'));
    await browser.close();
  }
})();
