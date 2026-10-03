// Serves one directory for FBI's Remote Install: streams files with large read buffers,
// long idle timeouts for slow 3DS Wi-Fi, Range requests, and a progress log (<dir>/serve.log).
//
// Over plain HTTP, FBI downloads with the 3DS's HTTP service and gives up after 15 s without
// data, so a Wi-Fi stall kills the install whatever the server does. Given a certificate, the
// server speaks HTTPS with only ECDHE + AEAD ciphers, which the 3DS's HTTP service can't
// negotiate; FBI then falls back to its libcurl, which has no stall timeout.
//
// usage: node serve-cia.js <bind ip> <port> <directory> [cert.pem key.pem]
const http = require("http");
const https = require("https");
const fs = require("fs");
const path = require("path");

const [ip, port, dir, certPath, keyPath] = process.argv.slice(2);
const logPath = path.join(dir, "serve.log");
const IDLE_TIMEOUT = 6 * 60 * 60 * 1000;    // ms a connection may stall before it is dropped
const CHUNK = 4 << 20;
const PROGRESS_INTERVAL = 10000;

function log(req, text) {
  const t = new Date().toTimeString().slice(0, 8);
  fs.appendFileSync(logPath, `${t} ${req.socket.remoteAddress} ${text}\n`);
}

function handler(req, res) {
  const name = decodeURIComponent(req.url.split("?")[0]).replace(/^\/+/, "");
  const file = path.resolve(dir, name);
  if (!file.startsWith(path.resolve(dir) + path.sep) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
    log(req, `${req.method} ${req.url} 404`);
    res.writeHead(404, { "Content-Length": 0 });
    return res.end();
  }
  const size = fs.statSync(file).size;
  let start = 0, end = size - 1, status = 200;
  const headers = { "Content-Type": "application/octet-stream", "Accept-Ranges": "bytes" };
  const m = /^bytes=(\d*)-(\d*)/.exec(req.headers.range || "");
  if (m) {
    if (m[1]) {
      start = +m[1];
      if (m[2]) end = Math.min(+m[2], size - 1);
    } else {
      start = Math.max(0, size - +m[2]);
    }
    if (start > end) {
      res.writeHead(416, { "Content-Range": `bytes */${size}`, "Content-Length": 0 });
      return res.end();
    }
    status = 206;
    headers["Content-Range"] = `bytes ${start}-${end}/${size}`;
  }
  const total = end - start + 1;
  headers["Content-Length"] = total;
  res.writeHead(status, headers);
  log(req, `${req.method} ${req.url} ${status} ${total >> 20} MB`);
  if (req.method === "HEAD") return res.end();

  req.socket.setNoDelay(true);
  req.socket.setKeepAlive(true, 30000);
  const t0 = Date.now();
  let read = 0, lastSent = 0, stalledSince = t0;
  // bytes handed to the socket, not counting what is still queued in Node
  const sent = () => read - res.writableLength;
  const stream = fs.createReadStream(file, { start, end, highWaterMark: CHUNK });
  stream.on("data", (chunk) => { read += chunk.length; });
  const timer = setInterval(() => {
    const now = Date.now(), s = sent();
    const rate = Math.round((s - lastSent) / 1024 / (PROGRESS_INTERVAL / 1000));
    const avg = Math.round(s / 1024 / ((now - t0) / 1000));
    if (s > lastSent) stalledSince = now;
    const stall = now - stalledSince >= PROGRESS_INTERVAL ? `, stalled ${Math.round((now - stalledSince) / 1000)} s` : "";
    log(req, `${req.url}: ${s >> 20} / ${total >> 20} MB, ${rate} KB/s (avg ${avg} KB/s)${stall}`);
    lastSent = s;
  }, PROGRESS_INTERVAL);
  stream.pipe(res);
  res.on("finish", () => log(req, `${req.url}: done, ${total >> 20} MB in ${Math.round((Date.now() - t0) / 1000)} s`));
  res.on("close", () => {
    clearInterval(timer);
    if (!res.writableFinished) {
      log(req, `${req.url}: dropped after ${sent() >> 20} MB`);
      stream.destroy();
    }
  });
}

const server = certPath
  ? https.createServer({
      cert: fs.readFileSync(certPath),
      key: fs.readFileSync(keyPath),
      minVersion: "TLSv1.2",
      ciphers: "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-RSA-AES256-GCM-SHA384",
      honorCipherOrder: true,
    }, handler)
  : http.createServer(handler);

server.on("tlsClientError", (err, socket) => {
  // expected once per download: the 3DS HTTP service fails the handshake, then FBI retries with curl
  const t = new Date().toTimeString().slice(0, 8);
  fs.appendFileSync(logPath, `${t} ${socket.remoteAddress} TLS handshake failed (${err.code || err.message})\n`);
});
server.keepAliveTimeout = IDLE_TIMEOUT;
server.headersTimeout = IDLE_TIMEOUT + 1000;
server.requestTimeout = 0;                   // no cap on how long one download may take
server.timeout = IDLE_TIMEOUT;
server.listen(+port, ip);

// The game's log from the 3DS (source/log.cpp sends each line as a UDP packet to port + 1):
// build/device-log.txt, a header per run
const dgram = require("dgram");
const deviceLog = path.join(dir, "device-log.txt");
const udp = dgram.createSocket("udp4");
udp.on("message", (msg, from) => {
  const line = msg.toString("latin1");
  if (line.includes("log: also sent to"))
    fs.appendFileSync(deviceLog, `\n==== ${new Date().toISOString()} run from ${from.address}\n`);
  fs.appendFileSync(deviceLog, line + "\n");
});
udp.bind(+port + 1, ip);

// ---- Development builds (source/devupdate.cpp), plain HTTP on port + 2:
//   /dev/version    build id of build/dev/mw3ds-dev.cia (tools/make-dev.ps1)
//   /dev/manifest   "size crc32 path" per file of out/data
//   /dev/data/<p>   a file of out/data (Range requests resume)
//   /dev/code.cia   the code-only CIA the game installs over itself
//   POST /dev/pack  body: paths, one per line; answer: per file a line "size crc32 path", then its bytes
const zlib = require("zlib");
// the game data dev builds sync from: out/data (Balmora area) or, with MW3DS_DATA=out/world, the island
const dataDir = path.resolve(dir, "..", process.env.MW3DS_DATA || path.join("out", "data"));
const devDir = path.join(dir, "dev");
const crcCache = new Map();          // relative path -> { size, mtime, crc }

function manifest() {
  const lines = [];
  const walk = (d, rel) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const full = path.join(d, e.name), r = rel ? rel + "/" + e.name : e.name;
      if (e.isDirectory()) walk(full, r);
      else if (e.isFile() && !e.name.startsWith(".")) {
        const st = fs.statSync(full);
        let c = crcCache.get(r);
        if (!c || c.size !== st.size || c.mtime !== st.mtimeMs) {
          c = { size: st.size, mtime: st.mtimeMs, crc: zlib.crc32(fs.readFileSync(full)) >>> 0 };
          crcCache.set(r, c);
        }
        lines.push(`${c.size} ${c.crc.toString(16).padStart(8, "0")} ${r}`);
      }
    }
  };
  walk(dataDir, "");
  return lines.join("\n") + "\n";
}

function sendFile(req, res, file) {
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
    res.writeHead(404, { "Content-Length": 0 });
    return res.end();
  }
  const size = fs.statSync(file).size;
  let start = 0, end = size - 1, status = 200;
  const headers = { "Content-Type": "application/octet-stream", "Accept-Ranges": "bytes" };
  const m = /^bytes=(\d*)-(\d*)/.exec(req.headers.range || "");
  if (m && m[1]) {
    start = +m[1];
    if (m[2]) end = Math.min(+m[2], size - 1);
    if (start > end) {
      res.writeHead(416, { "Content-Range": `bytes */${size}`, "Content-Length": 0 });
      return res.end();
    }
    status = 206;
    headers["Content-Range"] = `bytes ${start}-${end}/${size}`;
  }
  headers["Content-Length"] = Math.max(0, end - start + 1);
  res.writeHead(status, headers);
  if (size === 0 || req.method === "HEAD") return res.end();
  req.socket.setNoDelay(true);
  const stream = fs.createReadStream(file, { start, end, highWaterMark: 1 << 20 });
  stream.pipe(res);
  res.on("close", () => stream.destroy());
}

let devFiles = 0;
const devServer = http.createServer((req, res) => {
  const url = decodeURIComponent(req.url.split("?")[0]);
  if (url === "/dev/version") {
    const v = fs.existsSync(path.join(devDir, "version.txt")) ? fs.readFileSync(path.join(devDir, "version.txt"), "utf8").trim() : "";
    log(req, `dev: version asked (${v || "none"})`);
    res.writeHead(200, { "Content-Type": "text/plain", "Content-Length": Buffer.byteLength(v) });
    return res.end(v);
  }
  if (url === "/dev/manifest") {
    const t0 = Date.now(), text = manifest();
    log(req, `dev: manifest, ${text.split("\n").length - 1} files (${Date.now() - t0} ms)`);
    res.writeHead(200, { "Content-Type": "text/plain", "Content-Length": Buffer.byteLength(text) });
    return res.end(text);
  }
  if (url === "/dev/code.cia") {
    log(req, `dev: code CIA${req.headers.range ? " from " + req.headers.range : ""}`);
    return sendFile(req, res, path.join(devDir, "mw3ds-dev.cia"));
  }
  if (url === "/dev/pack" && req.method === "POST") {
    // A batch of data files in one stream: per file a line "size crc32 path", then its bytes
    let body = "";
    req.on("data", (d) => (body += d.toString("utf8")));
    req.on("end", () => {
      const names = body.split("\n").map((l) => l.trim()).filter(Boolean);
      const files = [];
      let length = 0;
      for (const n of names) {
        const file = path.resolve(dataDir, n);
        if (!file.startsWith(dataDir + path.sep) || !fs.existsSync(file)) continue;
        const data = fs.readFileSync(file);
        const head = Buffer.from(`${data.length} ${(zlib.crc32(data) >>> 0).toString(16).padStart(8, "0")} ${n}\n`, "utf8");
        files.push(head, data);
        length += head.length + data.length;
      }
      log(req, `dev: pack of ${files.length / 2} files, ${(length / 1048576).toFixed(1)} MB`);
      res.writeHead(200, { "Content-Type": "application/octet-stream", "Content-Length": length });
      req.socket.setNoDelay(true);
      const t0 = Date.now();
      res.on("finish", () => {
        const s = (Date.now() - t0) / 1000;
        log(req, `dev: pack sent in ${s.toFixed(1)} s (${Math.round(length / 1024 / Math.max(s, 0.001))} KB/s)`);
      });
      let i = 0;
      const pump = () => {
        while (i < files.length) {
          if (!res.write(files[i++])) return res.once("drain", pump);
        }
        res.end();
      };
      pump();
    });
    return;
  }
  if (url.startsWith("/dev/data/")) {
    const file = path.resolve(dataDir, url.slice(10));
    if (!file.startsWith(dataDir + path.sep)) {
      res.writeHead(404, { "Content-Length": 0 });
      return res.end();
    }
    if (++devFiles % 100 === 1) log(req, `dev: data file ${devFiles}: ${url.slice(10)}`);
    return sendFile(req, res, file);
  }
  res.writeHead(404, { "Content-Length": 0 });
  res.end();
});
devServer.keepAliveTimeout = IDLE_TIMEOUT;
devServer.requestTimeout = 0;
devServer.listen(+port + 2, ip);
