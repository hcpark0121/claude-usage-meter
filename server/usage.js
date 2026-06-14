// ccusage 로 활성 5시간 블록을 읽어 ESP32 가 쓰기 쉬운 평평한 객체로 만든다.
// usage-server.js(WiFi/HTTP) 와 serial-sender.js(USB-C) 가 공유한다.
const { execFile } = require("child_process");
const fs = require("fs");
const path = require("path");

const ccusageEntry = path.join(__dirname, "node_modules", "ccusage", "dist", "index.js");
const hasLocal = fs.existsSync(ccusageEntry);
const CMD = hasLocal ? process.execPath : "npx";
const BASE_ARGS = hasLocal ? [ccusageEntry] : ["-y", "ccusage@latest"];

function runCcusage() {
  return new Promise((resolve, reject) => {
    execFile(
      CMD,
      [...BASE_ARGS, "blocks", "--active", "--json"],
      { timeout: 60000, maxBuffer: 16 * 1024 * 1024 },
      (err, stdout) => {
        if (err) return reject(err);
        try {
          resolve(JSON.parse(stdout));
        } catch (e) {
          reject(e);
        }
      }
    );
  });
}

function shorten(model) {
  const m = model.match(/(opus|sonnet|haiku|fable)/i);
  return m ? m[1].toLowerCase() : "";
}

function round2(n) {
  return Math.round(n * 100) / 100;
}

function hhmm(iso) {
  if (!iso) return "";
  const d = new Date(iso);
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
}

// statusLine 훅이 저장한 '진짜' 구독 한도를 읽어 합친다.
// ~/.claude/meter-raw.json = Claude Code statusLine JSON 원본(meter-statusline.sh 가 기록).
// 신선도는 파일 수정시각(mtime)으로 판단.
function readRealLimits() {
  try {
    const p = path.join(process.env.HOME, ".claude", "meter-raw.json");
    const stat = fs.statSync(p);
    const j = JSON.parse(fs.readFileSync(p, "utf8"));
    const rl = j.rate_limits || {};
    const cw = j.context_window || {};
    const now = Math.floor(Date.now() / 1000);
    const minOf = (ts) => (ts ? Math.max(0, Math.round((ts - now) / 60)) : null);
    const out = { real: false };
    if (rl.five_hour && typeof rl.five_hour.used_percentage === "number") {
      out.real = true;
      out.sessionPct = Math.round(rl.five_hour.used_percentage);
      out.sessionResetMin = minOf(rl.five_hour.resets_at);
    }
    if (rl.seven_day && typeof rl.seven_day.used_percentage === "number") {
      out.weekPct = Math.round(rl.seven_day.used_percentage);
      out.weekResetMin = minOf(rl.seven_day.resets_at);
    }
    if (typeof cw.used_percentage === "number") out.ctxPct = Math.round(cw.used_percentage);
    out.realAgeSec = Math.max(0, Math.round((Date.now() - stat.mtimeMs) / 1000));
    out.realStale = out.realAgeSec > 300;  // 5분 넘게 갱신 없으면 stale
    return out;
  } catch (e) {
    return { real: false };
  }
}

// ccusage 부분만 (토큰/비용/번레이트). 비교적 느리게 갱신해도 됨.
async function getCcusage(budget) {
  const data = await runCcusage();
  const block = (data.blocks || []).find((b) => b.isActive) || data.blocks?.[0];
  if (!block) {
    return { ok: true, active: false, tokens: 0, cost: 0, budget, pct: 0, tpm: 0, remainingMin: 0, models: [], updated: Date.now() };
  }
  const tokens = block.totalTokens || 0;
  const pct = Math.max(0, Math.min(100, Math.round((tokens / budget) * 100)));
  return {
    ok: true,
    active: !!block.isActive,
    tokens,
    cost: round2(block.costUSD || 0),
    budget,
    pct,
    remainingMin: block.projection?.remainingMinutes ?? 0,
    tpm: Math.round(block.burnRate?.tokensPerMinute || 0),
    projTokens: block.projection?.totalTokens ?? 0,
    projCost: round2(block.projection?.totalCost || 0),
    windowEnd: hhmm(block.endTime),
    models: [...new Set((block.models || []).map(shorten).filter(Boolean))],
    updated: Date.now(),
  };
}

// 전체 (ccusage + 진짜 한도). serial-sender 등 1회성 호출용.
async function getUsage(budget) {
  const c = await getCcusage(budget);
  return { ...c, ...readRealLimits() };
}

module.exports = { getUsage, getCcusage, readRealLimits };
