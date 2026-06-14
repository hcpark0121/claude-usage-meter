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
- **(A) WiFi**: 집에서. Mac 의 `usage-server.js` 를 30초마다 폴링.
- **(B) USB-C 유선**: 회사처럼 WiFi 인증이 막힌 곳. 보드를 USB-C 로 꽂고 `serial-sender.js` 실행.
  보드는 USB 로 들어온 데이터를 우선 사용(8초 내 신선하면 WiFi 무시).

> **왜 BLE 가 아니라 USB-C?** 이 보드(ESP32-S3)는 BLE 만 되고(클래식 SPP 없음),
> macOS 를 BLE 호스트로 쓰는 코드는 불안정하다. 어차피 전원 때문에 USB-C 를 꽂으니,
> **같은 케이블로 데이터까지 보내는 USB-C 유선이 가장 단순하고 안정적**이다.

---

## 화면 보는 법 (숫자 해석)

```
 ● CLAUDE CODE  opus                                  WiFi  14:00   ← 헤더(점 = 활동 표시)
   1,234,567                                  BURN                  ← 이번 5h 블록 누적 토큰(흰색, 실시간 틱업)
   TOKENS / THIS 5H BLOCK            SPENT $11.22      657 tok/min
   TIME    ▮▮▮▮▮▮▮▮▮▮▯▯▯▯▯▯▯▯▯▯▯▯  38%   2h 14m       ← 5h 중 시간 진행률 + 남은 시간
   USAGE   ▮▮▮▮▮▮▮▮▮▮▮▮▮▮▮▮▮▯▯▯▯▯  61%   FAST          ← 예산 대비 토큰 소비율 + 페이스 판정
```

색 체계(검정 배경 + 3색): **흰색** = 핵심 숫자/값, **회색** = 라벨, **그린(#34C759)** = 포인트(점·번레이트·USAGE 막대).
숫자는 **고정폭 폰트**, 막대는 **세그먼트형**. 좌상단 **점은 사용량에 따라 깜빡임 속도가 달라진다**(쉴 때 4초 → 풀가동 0.36초).

### 핵심: TIME vs USAGE 두 막대로 "페이스"를 본다
5시간 블록은 rate limit 윈도우이고 **끝나면 리셋**된다. 그래서 중요한 건 *지금 얼마나 썼나*보다
**"시간이 지나는 속도보다 토큰을 빨리 쓰고 있나"** 다.

- 두 막대는 **같은 스케일(0~100%)**. **USAGE 막대가 TIME 막대보다 길면** = 시간보다 빨리 소진 중 → **리셋 전에 한도 도달 위험**.
- USAGE 막대 위 **흰 세로선(par 마커)** = 지금 시간 진행률 위치. 채워진 끝이 **이 선을 넘으면** 페이스 초과.
- 오른쪽 판정 한 단어 + 막대 색:
  - **EASY**(회색) — 시간보다 느리게 소비. 여유.
  - **ON PACE**(흰색) — 시간과 비슷한 속도. 적정.
  - **FAST**(빨강, 막대도 빨강) — 시간보다 빠름. **속도 줄이거나 리셋까지 대기 고려.**

| 항목 | 의미 |
|------|------|
| **큰 숫자 (TOKENS)** | 이번 **5시간 블록** 누적 토큰. 입력+출력+**캐시** 포함이라 크다(캐시 read 가 대부분). **정직 모드**: 가짜 상승 없이 갱신 때만 진짜 값으로 이동. |
| **TIME 바** | 5시간 중 **시간 진행률**(= (300−남은분)/300). 오른쪽은 남은 시간. |
| **USAGE 바** | **예산 대비 토큰 소비율**(`토큰/BUDGET_TOKENS`, 기본 1천만). 공식 한도가 아니라 **직접 정한 연료통** — 본인 플랜의 윈도우 한도에 맞춰 `BUDGET_TOKENS` 를 맞추면 페이스 판정이 정확해진다. |
| **BURN** | 현재 **분당 토큰 처리 속도**. 좌상단 점 깜빡임 속도가 이에 비례. |
| **SPENT $** | 이번 블록 추정 비용(USD). |
| **헤더 14:00** | 이 블록이 **리셋되는 시각**. |

> 행동 요령: **USAGE 막대가 TIME 막대를 앞지르면(FAST) 속도를 줄인다.** 뒤처지면(EASY) 마음껏 써도 됨.
> BURN 으로 "지금 얼마나 세게 밟고 있나", 두 막대 차이로 "이대로 가면 리셋 전에 터질까"를 본다.

> ⚠️ **이 값은 ccusage 추정치이지 Claude Code 의 공식 "Plan usage limits"(세션/주간 %)가 아니다.**
> ccusage 는 로컬 로그에서 토큰·비용을 단가로 추정하고, 5h 블록 리셋도 메시지 타임스탬프로 추론한다.
> 공식 세션/주간 % 와 리셋 시각은 Anthropic 서버가 주는 별도 값(로컬 미저장)이라 정확히 일치하지 않는다.
> USAGE % 는 **공식 한도가 아니라 직접 정한 `BUDGET_TOKENS` 대비** 임에 유의.

---

## 현재 상태 (이 Mac/보드는 이미 셋업됨)
- 서버: launchd 서비스 `com.hayden.claude-meter` 등록 → 로그인 시 자동 실행
- 펌웨어: 보드(`/dev/cu.usbmodem101`)에 플래시 완료
- 집에서는 WiFi `hcp` 로 자동 동작

---

## 모드 A — WiFi (집)

### 서버 (launchd, 이미 등록됨)
```bash
launchctl list | grep claude-meter                              # 상태
launchctl kickstart -k gui/$(id -u)/com.hayden.claude-meter     # 재시작
launchctl unload ~/Library/LaunchAgents/com.hayden.claude-meter.plist  # 중지
tail -f server/server.log                                       # 로그
```
최초 설치 시: `cd server && nvm use 24 && npm install` 후 plist 를 `launchctl load`.

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
#    예산 조절:      BUDGET_TOKENS=5000000 node serial-sender.js
```
실행하면 매 3초 보드로 사용량을 보내고, 보드 화면이 `USB` 소스로 바뀐다.
> 첫 연결 때 보드가 한 번 리부트할 수 있다(정상). 그 뒤 계속 표시.
> 로그인 시 자동 실행하려면 launchd 로 등록(아래 `serial-sender.js` 를 ProgramArguments 로).

> **펌웨어는 같은 것 하나면 된다.** 회사망에선 WiFi `hcp` 접속이 실패하지만,
> USB 데이터가 들어오면 그쪽을 쓰므로 문제없다.

---

## 펌웨어 빌드 / 플래시

PlatformIO 는 저장소 루트 `.pio-venv/` 에 있다.
```bash
cd firmware
cp src/config.example.h src/config.h          # 최초 1회
../.pio-venv/bin/pio run -t upload --upload-port /dev/cu.usbmodem101
```
시리얼 로그(이 환경은 TTY 없어 pyserial 로):
```bash
../.pio-venv/bin/python - <<'PY'
import serial,time,sys
s=serial.Serial(); s.port="/dev/cu.usbmodem101"; s.baudrate=115200; s.timeout=0.3
s.dtr=False; s.rts=False; s.open()
t=time.time()
while time.time()-t<12:
    d=s.read(256)
    if d: sys.stdout.write(d.decode('utf-8','replace')); sys.stdout.flush()
PY
```
정상: `amoled init=OK ... 536x240`, `[WiFi]`/`[USB] tokens=... shown=...`(shown 이 점점 증가).

---

## 문제 해결

| 증상 | 해결 |
|------|------|
| 화면 방향 반대 | 기본 `amoled.setRotation(2)`(USB 케이블 기준 180° 회전). 반대로 하려면 `setRotation(0)` 으로 바꿔 재플래시. |
| 화면 완전 까망 | Plus 는 RM67162 **SPI** 변종. QSPI 드라이버로는 안 나옴(현재 코드는 `amoled.begin()` 사용 — 정상). |
| `데이터 대기 중` 계속 | (A) launchd 서버·WiFi 확인. (B) `serial-sender.js` 가 보드 포트를 찾았는지 확인. |
| USB 송신 `포트 못 찾음` | `ls /dev/cu.usbmodem*` 로 포트 확인 후 인자로 지정. USB-C 가 데이터 지원 케이블인지 확인. |
| 게이지 항상 낮음 | `BUDGET_TOKENS` 를 줄인다(plist/환경변수). |
| 서버 `Object is not disposable` | Node 22 미만. `nvm use 24`. |

---

## 구조 메모
- `server/usage.js` — ccusage 로 활성 블록을 평평한 JSON 으로 (공유 모듈).
- `server/usage-server.js` — HTTP 서버(모드 A).
- `server/serial-sender.js` — USB-C 시리얼 송신(모드 B, 외부 의존성 없음).
- `firmware/src/main.cpp` — 듀얼 전송 수신 + 렌더링. 7세그 숫자는 번레이트로 외삽해 실시간 틱업.
- `firmware/lib/LilyGo_AMOLED/` — 패널 드라이버(Plus SPI), LVGL/TFT_eSPI 예제 의존성 제거한 사본.
- 그리기: Arduino_GFX `Arduino_Canvas`(PSRAM 프레임버퍼) → `amoled.pushColors()`.
