# Debian's Chromium includes the software H.264 decoder. Playwright's bundled
# Chromium omits it, which would prevent exercising KasmVNC's H.264 encoder.
FROM debian:trixie-slim
ENV PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates chromium nodejs npm fonts-liberation \
    && rm -rf /var/lib/apt/lists/* \
    && npm install --prefix /opt/portable-browser --omit=dev playwright@1.56.1
COPY browser.cjs /opt/portable-browser/browser.cjs
RUN mkdir /results
ENTRYPOINT ["node", "/opt/portable-browser/browser.cjs"]
