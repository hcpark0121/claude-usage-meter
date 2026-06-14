# Claude 사용량 미터기

LILYGO **T-Display AMOLED Plus** (ESP32-S3, 1.91" RM67162, 536×240) 에
**Claude Code 구독 한도(5시간 세션 % · 7일 주간 %)**를 책상에서 실시간으로 띄우는 데스크 미터기.

> 제품: https://ko.aliexpress.com/item/1005008722521317.html
> 개요·제작기: [docs/SHARE.md](docs/SHARE.md)

![Claude 사용량 미터기](images/meter-front.jpg)

```
~/.claude 로그 + statusLine 훅   (ccusage=토큰/비용 추정, statusLine=진짜 한도%)
                         ┌─ (A) WiFi/HTTP ──► usage-server.js :8722  (집)
                  ───────┤
                         └─ (B) USB-C 시리얼 ─► serial-sender.js     (회사/WiFi 불가)
                                    │
                                    ▼
                ESP32 AMOLED  ──  세션%(대형) + USAGE↔TIME 페이스 + 주간% + 번레이트
```

**전송 방식 2가지를 동시에 지원** — 펌웨어는 하나로 양쪽 다 된다:
- **(A) WiFi**: 집에서. Mac 의 `usage-server.js` 를 ~8초마다 폴링.
- **(B) USB-C 유선**: 회사처럼 WiFi 인증이 막힌 곳. 보드를 USB-C 로 꽂고 `serial-sender.js` 실행.
  보드는 USB 로 들어온 데이터를 우선 사용(8초 내 신선하면 WiFi 무시).

> **왜 BLE 가 아니라 USB-C?** 이 보드(ESP32-S3)는 BLE 만 되고(클래식 SPP 없음),
> macOS 를 BLE 호스트로 쓰는 코드는 불안정하다. 어차피 전원 때문에 USB-C 를 꽂으니,
> **같은 케이블로 데이터까지 보내는 USB-C 유선이 가장 단순하고 안정적**이다.

---

## 화면 보는 법 (숫자 해석)

```
 ● CLAUDE CODE  opus                                       WiFi
       57%          SESSION
                    resets 4h 02m
                    BURN 1.2k/min
   USAGE  ▮▮▮▮▮▮▮▮▮▮▮│··△········  FAST     ← 세션 사용%(초록) + 시간 위치(흰 삼각형) + 판정
   WEEK   ▮▮▮▮│·················  20%  in 1d ← 주간 한도
```

색 체계(검정 배경 + 3색): **흰색** = 핵심 숫자/값, **회색** = 라벨, **그린(#34C759)** = 포인트(점·번레이트·USAGE 막대).
좌상단 **점 깜빡임 속도 = 현재 사용 속도**(쉴 때 4초 → 풀가동 0.36초).

| 항목 | 의미 |
|------|------|
| **큰 % (SESSION)** | **5시간 세션 한도** 사용률 — Claude 의 공식 "Plan usage limits" 와 같은 **진짜 값**. 60%↑ 주황, 90%↑ 빨강. `resets` = 리셋까지 남은 시간. |
| **USAGE 바** | 세션 사용%(초록) + **흰 삼각형 = 지금 시간 진행 위치**. 채움이 삼각형을 **넘으면 = 시간보다 빨리 소비**. 판정: `EASY` / `ON PACE` / **`FAST`**(빨강). |
| **WEEK 바** | **7일 주간 한도** 사용률 + 리셋까지 남은 기간. |
| **BURN** | **분당 토큰 처리 속도**(ccusage 추정). 점 깜빡임 속도와 연동. |
| **STALE** | 한도 데이터가 5분 넘게 안 들어옴(Claude Code 비활동). 마지막 값 표시 중이라는 표시. |

> **핵심 행동요령:** 세션·주간 한도는 끝나면 0%로 리셋되는 rate limit 윈도우다.
> USAGE 막대가 시간(흰 삼각형)을 **앞지르면(FAST) 속도를 줄이거나 리셋까지 대기**, 뒤처지면(EASY) 마음껏.

---

## 데이터 소스 (중요 — 두 갈래)

| 데이터 | 출처 | 비고 |
|--------|------|------|
| **세션/주간 한도 % (진짜)** | **Claude Code statusLine 훅** | Claude Code 가 상태줄에 넘기는 JSON(`rate_limits.five_hour/seven_day`)을 `~/.claude/meter-raw.json` 에 저장 → 서버가 읽음. **공식 지원 훅이라 OAuth 토큰·미공개 API 불필요.** |
| **번레이트 · 토큰 · 비용** | **ccusage** (로컬 로그 추정) | 보조 정보(BURN)용. |

> 처음엔 ccusage 의 "예산 대비 토큰" 추정만 썼는데, 그건 *추정*이라 앱의 공식 한도(세션 %)와 안 맞았다.
> 그래서 **진짜 % 는 statusLine 훅에서** 받고, **ccusage 는 번레이트 보조로만** 쓰도록 바꿨다.

### statusLine 훅 설정 (진짜 한도를 받으려면 필수)
1. 훅 스크립트를 `~/.claude/` 에 둔다 (Claude Code 가 접근 가능한 경로여야 함):
   ```bash
   cp server/meter-statusline.sh ~/.claude/meter-statusline.sh
   chmod +x ~/.claude/meter-statusline.sh
   ```
2. `~/.claude/settings.json` 에 `statusLine` 추가:
   ```json
   { "statusLine": { "type": "command", "command": "sh ~/.claude/meter-statusline.sh" } }
   ```
3. **Claude Code 를 (재)시작**하고 메시지를 한 번 주고받으면 한도 값이 채워진다.
   (`rate_limits` 는 "첫 API 응답 후에만" 나오므로 메시지 1회 필요.)

> ⚠️ **갱신은 "활동 기반"** — Claude Code 가 상태줄을 다시 그릴 때(메시지·작업 중)만 갱신.
> 놀 땐 멈추지만 값은 그대로 정확(사용량% 는 쓸 때만 변하니까). 5분 넘으면 화면에 `STALE`.
> ⚠️ **터미널 `claude` 는 확실히 동작.** 데스크톱 앱은 샌드박스라 훅 실행 여부가 환경 따라 다를 수 있어
> 스크립트를 `~/.claude/` 안에 둔다. (그래도 안 되면 터미널 `claude` 사용.)

---

## 빠른 시작 요약
1. **서버**: `cd server && npm install` → `node usage-server.js` (또는 launchd 상시 실행)
2. **statusLine 훅**: 위 [데이터 소스](#데이터-소스-중요--두-갈래) 절대로 `~/.claude/` 에 설정
3. **펌웨어**: `config.h` 에 WiFi·서버 IP 입력 후 PlatformIO 로 플래시
4. Claude Code 에서 메시지 한 번 → 보드에 세션/주간 % 표시

---

## 모드 A — WiFi (집)

### 서버 (launchd 로 상시 실행)
`server/com.claude-meter.plist.example` 의 경로/Node 버전을 채워
`~/Library/LaunchAgents/com.claude-meter.plist` 로 복사 후:
```bash
launchctl load ~/Library/LaunchAgents/com.claude-meter.plist     # 등록·시작
launchctl list | grep claude-meter                               # 상태
launchctl kickstart -k gui/$(id -u)/com.claude-meter             # 재시작
tail -f server/server.log                                        # 로그
```
> 서버는 ccusage(번레이트)만 캐시(~15초)하고, **진짜 한도는 매 요청마다 `meter-raw.json` 에서 신선하게** 읽는다.
> 최초 설치: `cd server && nvm use 24 && npm install` (Node 22+ 필요).

### 펌웨어 WiFi 설정
`firmware/src/config.h` 의 `WIFI_SSID/WIFI_PASSWORD/SERVER_URL`. (Mac IP: `ipconfig getifaddr en0`)

---

## 모드 B — USB-C 유선 (회사 랩탑 등 WiFi 불가)

**보드를 다른(회사) 랩탑으로 옮겨 그 랩탑의 Claude Code 사용량을 보고 싶을 때.**
WiFi 설정 불필요 — USB-C 케이블만 꽂으면 된다.

회사 랩탑(macOS 기준)에서:
```bash
# 1) 이 저장소의 server/ 폴더를 회사 랩탑으로 복사
# 2) 의존성 설치 (Node 22+ 필요)
cd server && nvm use 24 && npm install
# 3) 보드를 USB-C 로 연결
# 4) 송신기 실행 (포트 자동 탐색)
node serial-sender.js
#    특정 포트 지정: node serial-sender.js /dev/cu.usbmodem101
```
실행하면 매 3초 보드로 사용량을 보내고, 보드 화면이 `USB` 소스로 바뀐다.
> 첫 연결 때 보드가 한 번 리부트할 수 있다(정상). 그 뒤 계속 표시.
> 로그인 시 자동 실행하려면 launchd 로 등록(아래 `serial-sender.js` 를 ProgramArguments 로).

> **펌웨어는 같은 것 하나면 된다.** 회사망에선 WiFi 접속이 실패하더라도,
> USB 데이터가 들어오면 그쪽을 쓰므로 문제없다.

---

## 펌웨어 빌드 / 플래시

PlatformIO 필요 (`pip install platformio` 또는 VS Code 확장). 보드 정의·라이브러리는 `firmware/` 에 포함.
```bash
cd firmware
cp src/config.example.h src/config.h          # 최초 1회: WIFI_SSID/PASSWORD/SERVER_URL 입력
pio run -t upload --upload-port /dev/cu.usbmodem101
pio device monitor                            # 시리얼 로그(115200)
```
정상 로그: `amoled init=OK ... rotation=2 536x240`, 이어서 `[WiFi]`/`[USB] ... pct=…`.

---

## 문제 해결

| 증상 | 해결 |
|------|------|
| 화면 방향 반대 | 기본 `amoled.setRotation(2)`(USB 케이블 기준 180° 회전). 반대로 하려면 `setRotation(0)` 으로 바꿔 재플래시. |
| 화면 완전 까망 | Plus 는 RM67162 **SPI** 변종. QSPI 드라이버로는 안 나옴(현재 코드는 `amoled.begin()` 사용 — 정상). |
| `데이터 대기 중` 계속 | (A) launchd 서버·WiFi 확인. (B) `serial-sender.js` 가 보드 포트를 찾았는지 확인. |
| **세션/주간 % 가 안 뜸** | statusLine 훅 미설정/미동작. `~/.claude/settings.json` 의 `statusLine` 확인 → Claude Code 재시작 → 메시지 1회. `cat ~/.claude/meter-raw.json` 에 `rate_limits` 가 있는지 확인. |
| **`STALE` 표시** | Claude Code 가 5분 넘게 비활동. 메시지를 주고받으면 갱신됨. 데스크톱 앱이면 터미널 `claude` 로도 시도. |
| USB 송신 `포트 못 찾음` | `ls /dev/cu.usbmodem*` 로 포트 확인 후 인자로 지정. USB-C 가 데이터 지원 케이블인지 확인. |
| 서버 `Object is not disposable` | Node 22 미만. `nvm use 24`. |

---

## 구조 메모
- `server/meter-statusline.sh` — Claude Code statusLine 훅. 받은 JSON 원본을 `~/.claude/meter-raw.json` 으로 저장(의존성 없는 셸).
- `server/usage.js` — ① `readRealLimits()`: `meter-raw.json` 에서 진짜 한도% 파싱(신선) ② `getCcusage()`: ccusage 로 번레이트/토큰/비용. 둘을 합쳐 JSON.
- `server/usage-server.js` — HTTP 서버(모드 A). ccusage 는 캐시, 진짜 한도는 매 요청 신선.
- `server/serial-sender.js` — USB-C 시리얼 송신(모드 B, 외부 의존성 없음).
- `firmware/src/main.cpp` — 듀얼 전송 수신 + 렌더링. `real` 이면 세션/주간 한도 화면, 아니면 ccusage 폴백. 숫자는 정직 모드(외삽 없음).
- `firmware/lib/LilyGo_AMOLED/` — 패널 드라이버(Plus SPI), LVGL/TFT_eSPI 예제 의존성 제거한 사본.
- 그리기: Arduino_GFX `Arduino_Canvas`(PSRAM 프레임버퍼) → `amoled.pushColors()` (빅엔디안 바이트 스왑 주의).
