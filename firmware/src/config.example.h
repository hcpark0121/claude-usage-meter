// 이 파일을 config.h 로 복사한 뒤 값을 채우세요:
//   cp src/config.example.h src/config.h
// config.h 는 .gitignore 에 들어있어 커밋되지 않습니다.
#pragma once

// --- WiFi ---
#define WIFI_SSID     "여기에_와이파이_이름"
#define WIFI_PASSWORD "여기에_와이파이_비밀번호"

// --- Mac 헬퍼 서버 주소 ---
// Mac 에서 `npm start` 했을 때 출력된 LAN URL 의 IP/포트를 적으세요.
// 예: http://192.168.0.35:8722/usage
#define SERVER_URL "http://192.168.0.35:8722/usage"

// --- 동작 설정 ---
#define POLL_INTERVAL_MS 30000  // 서버 폴링 주기 (ms)
#define SCREEN_BRIGHTNESS 200   // 0~255
