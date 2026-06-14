#!/usr/bin/env node
// Claude Code 사용량을 USB-C 시리얼로 ESP32 에 직접 보낸다.
// WiFi 가 막힌 환경(사내망 등)에서 사용. 보드를 USB-C 로 연결하기만 하면 됨.
// 외부 npm 의존성 없음 — macOS 의 /dev/cu.usbmodem* 장치에 직접 쓴다.
//
//   node serial-sender.js [포트경로]
//   PORT 미지정 시 /dev/cu.usbmodem* 자동 탐색.
//
// 환경변수: BUDGET_TOKENS(1000만), REFRESH_MS(3000)
const fs = require("fs");
const { execFileSync } = require("child_process");
const { getUsage } = require("./usage");

const BUDGET_TOKENS = parseInt(process.env.BUDGET_TOKENS || "10000000", 10);
const REFRESH_MS = parseInt(process.env.REFRESH_MS || "3000", 10);

const [major] = process.versions.node.split(".").map(Number);
if (major < 22) console.warn(`[경고] node ${process.versions.node}. 'nvm use 24' 권장.`);

function findPort() {
  if (process.argv[2]) return process.argv[2];
  const dir = "/dev";
  const hit = fs
    .readdirSync(dir)
    .filter((f) => f.startsWith("cu.usbmodem") || f.startsWith("cu.usbserial") || f.startsWith("cu.wchusbserial"))
    .sort();
  if (!hit.length) return null;
  return `${dir}/${hit[0]}`;
}

let stream = null;
let portPath = null;

function openPort() {
  portPath = findPort();
  if (!portPath) {
    process.stdout.write("보드(USB 시리얼)를 찾는 중... USB-C 로 연결되어 있나요?\r");
    return false;
  }
  try {
    // native USB-CDC 는 보레이트 무시하지만, 라인 디시플린 설정용으로 호출
    try { execFileSync("stty", ["-f", portPath, "115200", "raw", "-echo"]); } catch (_) {}
    stream = fs.createWriteStream(portPath);
    stream.on("error", (e) => {
      console.error(`\n[포트 오류] ${e.message} — 재연결 시도`);
      try { stream.destroy(); } catch (_) {}
      stream = null;
    });
    console.log(`\n포트 연결: ${portPath}`);
    return true;
  } catch (e) {
    console.error(`[열기 실패] ${e.message}`);
    stream = null;
    return false;
  }
}

async function tick() {
  if (!stream && !openPort()) return;
  let usage;
  try {
    usage = await getUsage(BUDGET_TOKENS);
  } catch (e) {
    process.stderr.write(`[ccusage 오류] ${e.message || e}\n`);
    return;
  }
  const line = JSON.stringify(usage) + "\n";
  try {
    stream.write(line);
    process.stdout.write(
      `[${new Date().toLocaleTimeString()}] -> ${portPath}  tokens=${usage.tokens} $${usage.cost} ${usage.pct}% ${usage.tpm}/min   \r`
    );
  } catch (e) {
    console.error(`\n[쓰기 실패] ${e.message}`);
    stream = null;
  }
}

console.log(`serial-sender 시작 (budget=${BUDGET_TOKENS}, ${REFRESH_MS}ms 주기)`);
openPort();
tick();
setInterval(tick, REFRESH_MS);
