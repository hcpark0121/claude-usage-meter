/**
 * Claude 토큰 사용량 미터기 — 비주얼/동적 버전
 * LILYGO T-Display AMOLED Plus (ESP32-S3, 1.91" RM67162 SPI, 536x240)
 *
 * 데이터 전송: 두 가지를 동시에 지원한다.
 *   1) USB-C 시리얼  : Mac 의 serial-sender.js 가 JSON 한 줄씩 보냄 (사내망/WiFi 불가 시)
 *   2) WiFi/HTTP    : usage-server.js 의 /usage 를 폴링 (가정용)
 *   → 최근에 들어온 쪽 데이터를 쓴다. 시리얼이 신선하면 시리얼 우선.
 *
 * 화면: 7-세그먼트 대형 토큰 수치(번레이트로 실시간 틱업) + 버스트 이퀄라이저 +
 *       그라데이션 게이지 바. 캔버스(PSRAM)에 그린 뒤 amoled.pushColors 로 전송.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LilyGo_AMOLED.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "config.h"

LilyGo_Class amoled;
Arduino_DataBus *dummyBus = new Arduino_ESP32QSPI(6, 47, 18, 7, 48, 5);
Arduino_GFX *dummyOut = new Arduino_RM67162(dummyBus, 17, 1);
Arduino_Canvas *canvas = nullptr;
int W = 536, H = 240;

// ---------- 팔레트 (절제된 3색 체계) ----------
// 주컬러   = 흰색 계열 (핵심 숫자/값)
// 보조컬러 = 차분한 회색 (라벨/부가정보)
// 포인트   = 코랄 (활동 표시: 점·번레이트·게이지에만 한정)
const uint8_t AR = 52, AG = 199, AB = 89;    // accent = 포스포 그린(#34C759)
const uint8_t WR = 229, WG = 84, WB = 75;    // warn = 경고 빨강(#E5544B)
// RM67162(SPI)는 빅엔디안 RGB565 를 기대하는데 ESP32 프레임버퍼는 리틀엔디안.
// → 모든 색을 미리 바이트 스왑해 둬야 패널에서 올바른 색으로 나온다.
// (흰/회색은 스왑해도 비슷해 보였지만, 채도 높은 색이 분홍/엉뚱한 색으로 깨졌었음)
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t c = canvas->color565(r, g, b);
  return (uint16_t)((c >> 8) | (c << 8));
}
uint16_t scaleColor(uint8_t r, uint8_t g, uint8_t b, float k) {
  return rgb565((int)(r * k), (int)(g * k), (int)(b * k));
}
struct Pal {
  uint16_t bg, surface, text, label, faint, accent, segOff;
} pal;
void initPalette() {
  pal.bg = rgb565(0, 0, 0);          // 순수 검정
  pal.surface = rgb565(28, 30, 34);  // 꺼진 세그먼트 칸(어두운 회색)
  pal.text = rgb565(232, 234, 236);  // 주: 선명한 오프화이트
  pal.label = rgb565(110, 118, 129); // 보조: 차분한 회색
  pal.faint = rgb565(50, 54, 60);
  pal.accent = rgb565(AR, AG, AB);   // 포인트: 그린
  pal.segOff = pal.surface;
}

// ---------- 상태 ----------
struct Usage {
  bool valid = false;
  long tokens = 0;
  float cost = 0, projCost = 0;
  long budget = 10000000L, projTokens = 0, tpm = 0;
  int pct = 0, remainingMin = 0;
  String models = "", windowEnd = "";
  String source = "";  // "USB" / "WiFi"
  // 진짜 구독 한도(statusLine 캡처). real=true 면 이 값들을 우선 사용.
  bool real = false, realStale = false;
  int sessionPct = 0, sessionResetMin = 0;
  int weekPct = 0, weekResetMin = 0, ctxPct = -1;
} u;

// 애니메이션
const int BLOCK_MIN = 300;  // 5시간 블록 = 300분
double dispTokens = 0;    // 화면에 보이는(부드럽게 보간되는) 토큰
double dispPct = 0;       // 주 사용률(real=세션%, else=예산%)
double dispTimePct = 0;   // 시간 진행률(부드러움)
double dispWeek = 0, dispCtx = 0;  // 주간/컨텍스트 %(real 모드)
double authTokens = 0;    // 마지막 권위값
unsigned long authMs = 0; // 권위값 수신 시각
unsigned long lastFrame = 0;
unsigned long lastSerialMs = 0, lastWifiMs = 0, lastPoll = 0;

String serialBuf;

// ---------- 유틸 ----------
String commafy(long n) {
  String s = String(n), out = "";
  int c = 0;
  for (int i = s.length() - 1; i >= 0; i--) {
    out = String(s[i]) + out;
    if (++c % 3 == 0 && i > 0) out = "," + out;
  }
  return out;
}
String fmtShort(long n) {
  if (n >= 1000000) return String(n / 1000000.0, 1) + "M";
  if (n >= 1000) return String(n / 1000.0, 1) + "K";
  return String(n);
}
// 사용량(tpm)에 따른 깜빡임 주기(ms): 단계 간 간격을 크게 벌려 느림/빠름이 확실히 구분되게.
int blinkPeriod(long tpm) {
  if (tpm <= 0) return 4000;      // 거의 정지: 아주 느린 호흡
  if (tpm < 300) return 2600;
  if (tpm < 1500) return 1400;
  if (tpm < 8000) return 760;
  return 360;                     // 풀가동: 빠른 점멸
}
// 낮게 머물다 확 밝아지는 '점멸' 곡선 (템포가 더 잘 읽힘)
float pulseLevel(int period) {
  float ph = (millis() % period) / (float)period;
  float s = 0.5f + 0.5f * sinf(2 * PI * ph);
  s = s * s;
  return 0.06f + 0.94f * s;
}

// ---------- 큰 숫자 (고정폭 기본 폰트, faux-bold) ----------
// 박스(x,y,maxW,maxH)에 맞는 최대 정수 배율로 좌측 정렬해서 그린다.
void drawBigNumber(const String &s, int x, int y, int maxW, int maxH, uint16_t color) {
  int len = s.length();
  int size = 8;
  while (size > 1 && (len * 6 * size > maxW || 8 * size > maxH)) size--;
  int th = 8 * size;
  int yy = y + (maxH - th) / 2;
  canvas->setTextSize(size);
  canvas->setTextColor(color);
  // faux-bold: 1px 겹쳐 두 번 그려 굵게
  canvas->setCursor(x, yy);
  canvas->print(s);
  canvas->setCursor(x + 1, yy);
  canvas->print(s);
}

// ---------- 컴포넌트 ----------
// 활동 표시 점: 사용량에 따라 깜빡임 속도가 달라진다(유일한 동적 장식).
void drawActivityDot(int cx, int cy, int r) {
  float lvl = pulseLevel(blinkPeriod(u.valid ? u.tpm : 0));
  canvas->fillCircle(cx, cy, r, scaleColor(AR, AG, AB, lvl));
}

void drawHeader() {
  drawActivityDot(16, 15, 6);
  canvas->setTextSize(2);
  canvas->setTextColor(pal.text);
  canvas->setCursor(32, 7);
  canvas->print("CLAUDE CODE");
  if (u.models.length()) {
    canvas->setTextColor(pal.label);
    canvas->print("  ");
    canvas->print(u.models);
  }
  // 우: 소스 + 블록 종료시각 (size 2)
  String right = u.source.length() ? u.source : "";
  if (u.windowEnd.length()) right += (right.length() ? "  " : "") + u.windowEnd;
  if (right.length()) {
    int tw = right.length() * 12;
    canvas->setTextColor(pal.label);
    canvas->setCursor(W - tw - 14, 7);
    canvas->print(right);
  }
}

// 세그먼트형 진행률 바. 칸이 채워지며 채워진 칸은 끝으로 갈수록 살짝 밝아진다.
// parPct>=0 이면 그 위치에 밝은 par(기준) 마커.
void drawBar(int x, int y, int w, int h, float pct, uint8_t r, uint8_t g, uint8_t b, int parPct) {
  const int N = 26;          // 칸 수
  const int gap = 3;
  float cw = (w + gap) / (float)N;
  int cellW = (int)cw - gap;
  if (cellW < 2) cellW = 2;
  uint16_t base = rgb565(r, g, b);
  uint16_t bright = rgb565(min(255, r + 120), min(255, g + 120), min(255, b + 120));
  for (int i = 0; i < N; i++) {
    int cx = x + (int)(i * cw);
    bool on = (i / (float)N) * 100.0f < pct;  // 시작 기준: pct>0 이면 첫 칸은 켜짐
    canvas->fillRoundRect(cx, y, cellW, h, 2, on ? base : pal.surface);
  }
  // 현재 usage(값) 선 — 값이 작아도 항상 보이는 밝은 세로선
  if (pct > 0.01f) {
    int vx = x + (int)(w * pct / 100.0f);
    if (vx < x + 2) vx = x + 2;
    if (vx > x + w - 2) vx = x + w - 2;
    canvas->fillRect(vx - 1, y - 1, 3, h + 2, bright);
  }
  // Time 진행 선 — 흰 선 + 위쪽 삼각형 (usage 선과 구분)
  if (parPct >= 0) {
    int px = x + (int)(w * parPct / 100.0f);
    if (px < x + 1) px = x + 1;
    if (px > x + w - 1) px = x + w - 1;
    canvas->fillRect(px - 1, y - 3, 3, h + 6, pal.text);
    canvas->fillTriangle(px - 5, y - 10, px + 5, y - 10, px, y - 3, pal.text);
  }
}

String fmtMin(int m) {
  if (m >= 60) return String(m / 60) + "h " + String(m % 60) + "m";
  return String(m) + "m";
}
String fmtDur(int m) {  // 일 단위까지 (주간 리셋 등)
  if (m >= 1440) return String(m / 1440) + "d " + String((m % 1440) / 60) + "h";
  return fmtMin(m);
}

// 진짜 구독 한도 화면: 세션% 히어로 + 세션 USAGE(시간 par) + 주간 바 (컨텍스트 제외)
void renderReal(uint16_t warnC) {
  float tp = (float)dispTimePct, kp = (float)dispPct;  // 세션 시간%, 세션 사용%
  String verdict; uint16_t vcol; bool barWarn;
  if (tp < 4 && kp < 4) { verdict = "START"; vcol = pal.label; barWarn = false; }
  else if (kp >= 90) { verdict = "LIMIT!"; vcol = warnC; barWarn = true; }
  else {
    float ratio = kp / (tp < 1 ? 1 : tp);
    if (ratio > 1.25f) { verdict = "FAST"; vcol = warnC; barWarn = true; }
    else if (ratio < 0.8f) { verdict = "EASY"; vcol = pal.label; barWarn = false; }
    else { verdict = "ON PACE"; vcol = pal.text; barWarn = false; }
  }
  uint16_t amber = rgb565(240, 172, 84);
  uint16_t sc = kp >= 90 ? warnC : (kp >= 70 ? amber : pal.accent);

  // 히어로: 세션 사용 % — 살짝 아래로 내려 상단 여백 확보, 적당한 크기
  drawBigNumber(String((int)round(kp)) + "%", 22, 46, 156, 58, sc);
  // SESSION 정보 (히어로 숫자와 세로 중앙 정렬)
  canvas->setTextSize(2);
  canvas->setTextColor(pal.label);
  canvas->setCursor(198, 48);
  canvas->print("SESSION");
  canvas->setTextColor(pal.text);
  canvas->setCursor(198, 72);
  canvas->print("resets " + fmtDur(u.sessionResetMin));
  if (u.realStale) {
    canvas->setTextSize(1);
    canvas->setTextColor(amber);
    canvas->setCursor(198, 98);
    canvas->print("STALE - paused (send a message)");
  } else {
    canvas->setTextSize(2);
    canvas->setTextColor(pal.label);
    canvas->setCursor(198, 96);
    canvas->print("BURN ");
    canvas->setTextColor(pal.accent);
    canvas->print(fmtShort(u.tpm));
    canvas->setTextColor(pal.label);
    canvas->print("/min");
  }

  int bx = 96, bw = 268, bh = 24;
  // 세션 USAGE 바 (사용% + 시간 par 마커)
  canvas->setTextSize(2);
  canvas->setTextColor(pal.label);
  canvas->setCursor(16, 140);
  canvas->print("USAGE");
  drawBar(bx, 136, bw, bh, kp, barWarn ? WR : AR, barWarn ? WG : AG, barWarn ? WB : AB, (int)round(tp));
  canvas->setTextColor(vcol);
  canvas->setCursor(bx + bw + 10, 134);
  canvas->print(verdict);
  canvas->setTextSize(1);
  canvas->setTextColor(pal.label);
  canvas->setCursor(bx + bw + 10, 153);
  canvas->print("time " + String((int)round(tp)) + "%");

  // 주간 바
  bool wWarn = u.weekPct >= 85;
  canvas->setTextSize(2);
  canvas->setTextColor(pal.label);
  canvas->setCursor(16, 190);
  canvas->print("WEEK");
  drawBar(bx, 186, bw, bh, (float)dispWeek, wWarn ? WR : AR, wWarn ? WG : AG, wWarn ? WB : AB, -1);
  canvas->setTextColor(wWarn ? warnC : pal.text);
  canvas->setCursor(bx + bw + 10, 184);
  canvas->print(String(u.weekPct) + "%");
  if (u.weekResetMin > 0) {
    canvas->setTextSize(1);
    canvas->setTextColor(pal.label);
    canvas->setCursor(bx + bw + 10, 203);
    canvas->print("in " + fmtDur(u.weekResetMin));
  }
}

// ---------- 합성 ----------
void render() {
  canvas->fillScreen(pal.bg);
  drawHeader();

  if (!u.valid) {
    canvas->setTextSize(3);
    canvas->setTextColor(pal.text);
    canvas->setCursor(30, 96);
    canvas->print("WAITING FOR DATA");
    canvas->setTextSize(2);
    canvas->setTextColor(pal.label);
    canvas->setCursor(30, 134);
    canvas->print("USB-C or WiFi...");
    return;
  }
  uint16_t warnC = rgb565(WR, WG, WB);
  if (u.real) { renderReal(warnC); return; }

  // 히어로: 큰 고정폭 숫자 (흰색)
  long shown = (long)(dispTokens + 0.5);
  drawBigNumber(commafy(shown), 18, 28, 360, 56, pal.text);
  // 서브라벨 + 비용(우측)
  canvas->setTextSize(2);
  canvas->setTextColor(pal.label);
  canvas->setCursor(20, 92);
  canvas->print("TOKENS / THIS 5H BLOCK");
  String spent = "SPENT $" + String(u.cost, 2);
  canvas->setCursor(W - spent.length() * 12 - 14, 92);
  canvas->print(spent);

  // 번레이트 (우상단, 포인트 컬러)
  canvas->setTextSize(2);
  canvas->setTextColor(pal.label);
  canvas->setCursor(392, 26);
  canvas->print("BURN");
  canvas->setTextSize(3);
  canvas->setTextColor(pal.accent);
  canvas->setCursor(392, 46);
  canvas->print(fmtShort(u.tpm));
  canvas->setTextSize(1);
  canvas->setTextColor(pal.label);
  canvas->setCursor(392, 72);
  canvas->print("tok/min");

  // ----- 페이스 비교: 시간 진행률 vs 토큰 소비 진행률 -----
  float tp = (float)dispTimePct, kp = (float)dispPct;
  String verdict; uint16_t vcol; bool barWarn;
  if (u.tpm <= 0 && kp < 1) { verdict = "IDLE"; vcol = pal.label; barWarn = false; }
  else if (tp < 5) { verdict = "START"; vcol = pal.label; barWarn = false; }
  else {
    float ratio = kp / (tp < 1 ? 1 : tp);
    if (ratio > 1.25f || kp >= 90) { verdict = "FAST"; vcol = warnC; barWarn = true; }
    else if (ratio < 0.8f) { verdict = "EASY"; vcol = pal.label; barWarn = false; }
    else { verdict = "ON PACE"; vcol = pal.text; barWarn = false; }
  }

  int bx = 86, bw = 276, bh = 30;
  // TIME 바 (회색)
  canvas->setTextSize(2);
  canvas->setTextColor(pal.label);
  canvas->setCursor(20, 141);
  canvas->print("TIME");
  drawBar(bx, 134, bw, bh, tp, 110, 116, 128, -1);
  canvas->setTextColor(pal.text);
  canvas->setCursor(bx + bw + 10, 141);
  canvas->print(String((int)round(tp)) + "%");
  canvas->setTextColor(pal.label);
  canvas->setCursor(bx + bw + 62, 141);
  canvas->print(fmtMin(u.remainingMin));

  // USAGE 바 (포인트/경고) + par(시간) 마커
  canvas->setTextColor(pal.label);
  canvas->setCursor(20, 193);
  canvas->print("USAGE");
  drawBar(bx, 186, bw, bh, kp, barWarn ? WR : AR, barWarn ? WG : AG, barWarn ? WB : AB, (int)round(tp));
  canvas->setTextColor(barWarn ? warnC : pal.text);
  canvas->setCursor(bx + bw + 10, 193);
  canvas->print(String((int)round(kp)) + "%");
  canvas->setTextColor(vcol);
  canvas->setCursor(bx + bw + 62, 193);
  canvas->print(verdict);
}

// ---------- 데이터 수신 ----------
void applyJson(JsonDocument &doc, const char *src) {
  if (!doc["ok"].as<bool>()) return;
  u.valid = true;
  u.source = src;
  u.tokens = doc["tokens"] | 0L;
  u.cost = doc["cost"] | 0.0f;
  u.budget = doc["budget"] | 10000000L;
  u.pct = doc["pct"] | 0;
  u.remainingMin = doc["remainingMin"] | 0;
  u.tpm = doc["tpm"] | 0L;
  u.projTokens = doc["projTokens"] | 0L;
  u.projCost = doc["projCost"] | 0.0f;
  u.windowEnd = (const char *)(doc["windowEnd"] | "");
  u.models = "";
  for (JsonVariant m : doc["models"].as<JsonArray>()) {
    if (u.models.length()) u.models += "+";
    u.models += m.as<String>();
  }
  // 진짜 구독 한도(statusLine)
  u.real = doc["real"] | false;
  u.realStale = doc["realStale"] | false;
  u.sessionPct = doc["sessionPct"] | 0;
  u.sessionResetMin = doc["sessionResetMin"] | 0;
  u.weekPct = doc["weekPct"] | 0;
  u.weekResetMin = doc["weekResetMin"] | 0;
  u.ctxPct = doc["ctxPct"] | -1;
  // 권위값 갱신
  authTokens = u.tokens;
  authMs = millis();
  if (dispTokens == 0) dispTokens = u.tokens;
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (serialBuf.length() > 2) {
        JsonDocument doc;
        if (!deserializeJson(doc, serialBuf)) {
          applyJson(doc, "USB");
          lastSerialMs = millis();
        }
      }
      serialBuf = "";
    } else if (c != '\r' && serialBuf.length() < 2048) {
      serialBuf += c;
    }
  }
}

void fetchWiFi() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  http.begin(SERVER_URL);
  if (http.GET() == 200) {
    String p = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, p)) {
      applyJson(doc, "WiFi");
      lastWifiMs = millis();
    }
  }
  http.end();
}

void animate(float dt) {
  float k = min(1.0f, dt * 4.0f);
  long elapsed = (long)(millis() - authMs);
  if (elapsed < 0) elapsed = 0;
  double elapMin = elapsed / 60000.0;

  // 토큰(정직 모드: 가짜 상승 없이 진짜 값으로만)
  double target = u.valid ? authTokens : 0;
  dispTokens += (target - dispTokens) * k;

  double targetPct, targetTime;
  if (u.real) {
    // 진짜 구독 한도: 세션 사용% 와 세션 시간 진행%(리셋까지 실시간 차감)
    targetPct = u.sessionPct;
    double rem = u.sessionResetMin - elapMin;
    if (rem < 0) rem = 0;
    targetTime = (BLOCK_MIN - rem) / (double)BLOCK_MIN * 100.0;
    dispWeek += (u.weekPct - dispWeek) * k;
    dispCtx += ((u.ctxPct < 0 ? 0 : u.ctxPct) - dispCtx) * k;
  } else {
    // 폴백: ccusage 예산 기준
    targetPct = u.budget > 0 ? (dispTokens / u.budget) * 100.0 : 0;
    double liveRemain = u.remainingMin - elapMin;
    if (liveRemain < 0) liveRemain = 0;
    targetTime = u.valid ? (BLOCK_MIN - liveRemain) / (double)BLOCK_MIN * 100.0 : 0;
  }
  if (targetPct < 0) targetPct = 0; if (targetPct > 100) targetPct = 100;
  if (targetTime < 0) targetTime = 0; if (targetTime > 100) targetTime = 100;
  dispPct += (targetPct - dispPct) * k;
  dispTimePct += (targetTime - dispTimePct) * k;
}

void splash() {
  canvas->fillScreen(pal.bg);
  canvas->setTextSize(3);
  canvas->setTextColor(pal.text);
  canvas->setCursor(118, 92);
  canvas->print("CLAUDE");
  canvas->setTextSize(2);
  canvas->setTextColor(pal.label);
  canvas->setCursor(150, 128);
  canvas->print("token meter");
  drawActivityDot(W / 2 - 6, 160, 5);
  amoled.pushColors(0, 0, W, H, canvas->getFramebuffer());
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Claude meter boot ===");

  bool ok = amoled.begin();
  if (!ok) ok = amoled.beginAMOLED_191_SPI();
  W = amoled.width();
  H = amoled.height();
  // USB 케이블 방향에 맞춰 180도 회전(가로 유지). 다시 뒤집으려면 0 으로.
  amoled.setRotation(2);
  W = amoled.width();
  H = amoled.height();
  Serial.printf("amoled init=%s rotation=%d %dx%d\n", ok ? "OK" : "FAIL", amoled.getRotation(), W, H);

  canvas = new Arduino_Canvas(W, H, dummyOut, 0, 0);
  canvas->begin(GFX_SKIP_OUTPUT_BEGIN);
  initPalette();
  amoled.setBrightness(SCREEN_BRIGHTNESS);
  splash();

  // WiFi 는 비차단으로 시작(있으면 사용, 없으면 USB 시리얼만 써도 됨)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  lastFrame = millis();
}

void loop() {
  readSerial();

  unsigned long now = millis();
  // WiFi 폴링: 평소 POLL_INTERVAL_MS 주기, 아직 데이터 없으면 1.5초마다 빠르게 시도.
  // 단, 시리얼(USB)이 최근 8초 내 들어왔으면 WiFi 는 건드리지 않음.
  bool due = (now - lastPoll >= POLL_INTERVAL_MS) || (!u.valid && now - lastPoll > 1500);
  if (due) {
    if (now - lastSerialMs > 8000) fetchWiFi();
    lastPoll = now;
  }
  // 시리얼/WiFi 둘 다 오래 끊기면 대기 화면.
  // 주의: fetchWiFi() 가 블로킹되는 동안 millis() 가 흘러 lastWifiMs 가 위의 now 보다
  // 커질 수 있으므로, 신선한 millis() 와 부호 있는 비교로 underflow 를 피한다.
  unsigned long ref = lastSerialMs > lastWifiMs ? lastSerialMs : lastWifiMs;
  if (u.valid && ref != 0 && (long)(millis() - ref) > 90000) u.valid = false;

  // 프레임 (~30fps 목표, 실제는 push 속도에 좌우)
  float dt = (now - lastFrame) / 1000.0f;
  if (dt >= 0.033f) {
    animate(dt);
    render();
    amoled.pushColors(0, 0, W, H, canvas->getFramebuffer());
    lastFrame = now;

    static unsigned long lastLog = 0;
    if (now - lastLog > 5000) {
      Serial.printf("[%s] tokens=%ld shown=%.0f pct=%.1f tpm=%ld wifi=%d\n",
                    u.source.length() ? u.source.c_str() : "wait", u.tokens, dispTokens,
                    dispPct, u.tpm, WiFi.status());
      lastLog = now;
    }
  }
}
