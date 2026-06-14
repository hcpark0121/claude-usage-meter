#!/usr/bin/env node
// Claude Code 사용량 헬퍼 서버 (WiFi/HTTP)
//   GET /usage  -> 사용량 JSON (ccusage 캐시 + 진짜 한도는 매 요청 신선하게)
//   GET /health -> { ok: true }
// 환경변수: PORT(8722), BUDGET_TOKENS(1000만), REFRESH_MS(ccusage 갱신 주기, 15000)
const http = require("http");
const os = require("os");
const { getCcusage, readRealLimits } = require("./usage");

const PORT = parseInt(process.env.PORT || "8722", 10);
const BUDGET_TOKENS = parseInt(process.env.BUDGET_TOKENS || "10000000", 10);
const REFRESH_MS = parseInt(process.env.REFRESH_MS || "15000", 10);

const [major] = process.versions.node.split(".").map(Number);
if (major < 22) console.warn(`[경고] node ${process.versions.node}. ccusage 는 node 22+ 권장.`);

// ccusage 는 주기적으로 캐시 (스폰 비용이 있어 매 요청마다 돌리지 않음)
let ccuCache = { ok: false, error: "starting" };
async function refreshCcu() {
  try {
    ccuCache = await getCcusage(BUDGET_TOKENS);
  } catch (e) {
    ccuCache = { ...ccuCache, ok: false, error: String(e.message || e) };
    process.stderr.write(`[ccusage error] ${e.message || e}\n`);
  }
}

const server = http.createServer((req, res) => {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Content-Type", "application/json");
  if (req.url.startsWith("/health")) return res.end(JSON.stringify({ ok: true }));
  if (req.url.startsWith("/usage")) {
    // 진짜 한도(statusLine)는 매 요청마다 파일에서 신선하게 읽어 합친다 → 보드가 자주 폴링하면 거의 실시간
    return res.end(JSON.stringify({ ...ccuCache, ...readRealLimits() }));
  }
  res.statusCode = 404;
  res.end(JSON.stringify({ ok: false, error: "not found" }));
});

server.listen(PORT, () => {
  const ips = [];
  const nets = os.networkInterfaces();
  for (const name of Object.keys(nets))
    for (const ni of nets[name]) if (ni.family === "IPv4" && !ni.internal) ips.push(ni.address);
  console.log(`usage-server listening on :${PORT}`);
  console.log(`  LAN URL(s): ${ips.map((ip) => `http://${ip}:${PORT}/usage`).join("  ") || "(no LAN ip)"}`);
  console.log(`  ccusage refresh=${REFRESH_MS}ms, real-limits=per-request, budget=${BUDGET_TOKENS}`);
  refreshCcu();
  setInterval(refreshCcu, REFRESH_MS);
});
