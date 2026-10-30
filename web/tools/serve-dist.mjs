// The built site over HTTP on 127.0.0.1, at a free port, for the browser
// checks (smoke-browser.mjs, input-browser.mjs).

import fs from "node:fs";
import http from "node:http";
import path from "node:path";

const TYPES = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".css": "text/css",
                ".svg": "image/svg+xml", ".json": "application/json", ".png": "image/png", ".wasm": "application/wasm" };

// requests: the paths asked for, in order.
export async function serveDist(site) {
  const requests = [];
  const server = http.createServer((request, response) => {
    requests.push(decodeURIComponent(new URL(request.url, "http://host").pathname));
    let file = path.join(site, decodeURIComponent(new URL(request.url, "http://host").pathname));
    if (file.endsWith("/")) file += "index.html";
    fs.readFile(file, (error, data) => {
      if (error) {
        response.writeHead(404).end();
        return;
      }
      response.writeHead(200, { "content-type": TYPES[path.extname(file)] ?? "application/octet-stream" }).end(data);
    });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  return { url: `http://127.0.0.1:${server.address().port}/`, requests, close: () => server.close() };
}
