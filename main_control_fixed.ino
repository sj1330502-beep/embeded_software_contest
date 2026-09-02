/*
 * =============================================================
 *  발열기기 방치 자동감지·차단 시스템 - V7.1
 *  (시간 기반 UI + 지능형 회복 + 무한 재부팅 방지 + 0.35A 전류설정 복구)
 * =============================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// ---------- 텔레그램 설정 ----------
const char* TELEGRAM_TOKEN = "8915633386:AAGhlYc-u8mpY1ejj2LKL7nbXirmKpEHnog";
const char* CHAT_ID        = "8724618109";

WiFiClientSecure telegramClient;
UniversalTelegramBot bot(TELEGRAM_TOKEN, telegramClient);

void sendTelegramAlert(String message) {
  bot.sendMessage(CHAT_ID, message, "");
}

// ---------- WiFi 설정 ----------
const char* WIFI_SSID     = "BSstudy";
const char* WIFI_PASSWORD = "bbosong0424";

WebServer server(80);

// ---------- 핀 설정 ----------
#define PIN_RADAR      26
#define PIN_CURRENT    34
#define PIN_SSR        25
#define PIN_BUZZER     33

// =============================================================
//  시스템 파라미터
// =============================================================
float LOOP_INTERVAL_SEC        = 1.0;
float CURRENT_ON_THRESHOLD     = 0.35;   // ★ 켜짐 판정 기준 0.35A로 변경 적용
float SCORE_MAX                = 100.0;

float targetCutoffSeconds      = 300.0;  // 자동 차단 시간 (기본 300초 = 5분)
float targetRecoverySeconds    = 45.0;   // 안전 회복 속도
float warningBeforeSeconds     = 30.0;   // 차단 30초 전 경고

int   BUZZER_INTERVAL_SLOW_MS  = 800;
int   BUZZER_INTERVAL_FAST_MS  = 100;
float RESET_ON_DETECT_RATIO    = 0.85;   // 사람 감지 시 경고선 밑으로 훅 떨어지는 비율

// ---- ACS712 (30A 센서) 회로 스펙 ----
const float ADC_REF_VOLTAGE = 3.3;
const int   ADC_RESOLUTION  = 4095;
const float VOLTAGE_DIVIDER_RATIO = (10.0 + 20.0) / 20.0;
const float ACS712_SENSITIVITY = 0.066; 
const float ACS712_ZERO_VOLTAGE = 2.5;

// =============================================================
//  상태 변수
// =============================================================
float riskScore = 0.0;
bool relayIsOn = true;
bool isShutdown = false;
float lastCurrent = 0.0;
bool lastPersonHere = false;

unsigned long lastLoopTime = 0;
unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

// =============================================================
//  센서 / 제어 함수
// =============================================================
float readCurrentAmps() {
  uint32_t start_time = millis();
  int count = 0;
  float sumV = 0;
  float sumSqV = 0;
  
  while((millis() - start_time) < 50) {
    float vAtPin = (analogRead(PIN_CURRENT) / (float)ADC_RESOLUTION) * ADC_REF_VOLTAGE;
    float vOriginal = vAtPin * VOLTAGE_DIVIDER_RATIO;
    sumV += vOriginal;
    sumSqV += (vOriginal * vOriginal);
    count++;
  }
  
  float meanV = sumV / count;
  float meanSqV = sumSqV / count;
  float variance = meanSqV - (meanV * meanV);
  if (variance < 0) variance = 0;
  
  float vRMS = sqrt(variance);
  float rawAmps = vRMS / ACS712_SENSITIVITY;
  
  if (rawAmps < 0.25) {
    rawAmps = 0.00;
  }
  
  static float filteredAmps = 0.0;
  filteredAmps = (0.5 * rawAmps) + (0.5 * filteredAmps);  // 0.3→0.5로 변경  
  return filteredAmps;
}

bool isPersonDetected() { return digitalRead(PIN_RADAR); }
void relayOn() { digitalWrite(PIN_SSR, HIGH); relayIsOn = true; }
void relayOff() { digitalWrite(PIN_SSR, LOW); relayIsOn = false; }
void buzzerOff() { digitalWrite(PIN_BUZZER, LOW); buzzerState = false; }

void buzzerPattern(int intervalMs) {
  if (intervalMs <= 0) { digitalWrite(PIN_BUZZER, HIGH); return; }
  unsigned long now = millis();
  if (now - lastBuzzerToggle > (unsigned long)intervalMs) {
    buzzerState = !buzzerState;
    digitalWrite(PIN_BUZZER, buzzerState ? HIGH : LOW);
    lastBuzzerToggle = now;
  }
}

float getWarningThreshold() {
  float incPerSec = SCORE_MAX / targetCutoffSeconds;
  float threshold = SCORE_MAX - (warningBeforeSeconds * incPerSec);
  return constrain(threshold, 10.0, SCORE_MAX - 5.0); 
}

// 유령 재부팅 방지 및 지능형 회복 로직 적용
void updateRiskScore(bool deviceOn, bool personHere) {
  float incPerSec = SCORE_MAX / targetCutoffSeconds;
  float decPerSec = SCORE_MAX / targetRecoverySeconds;
  float warningThreshold = getWarningThreshold();

  static bool personWasHereLastTime = true;
  bool personJustReturned = (personHere && !personWasHereLastTime);

  // 1. 지능형 하강 (부저 즉시 해제)
  if (personJustReturned && riskScore > warningThreshold) {
    riskScore = warningThreshold * RESET_ON_DETECT_RATIO;
  }
  personWasHereLastTime = personHere;

  // 2. 점수 증감 메인 로직 (무한 재시작 방어)
  if (personHere) {
    riskScore -= decPerSec * LOOP_INTERVAL_SEC; // 사람 있으면 점수 깎임
  } else {
    // 사람 없을 때
    if (isShutdown) {
      // 🚨 차단 상태면 스스로 깎이지 않음 (100점 유지 -> 무한 릴레이 켜짐 방지!)
    } else {
      // 차단 안 된 상태에서 기기 켜짐 유무에 따라 점수 증감
      if (deviceOn) {
        riskScore += incPerSec * LOOP_INTERVAL_SEC;
      } else {
        riskScore -= decPerSec * LOOP_INTERVAL_SEC;
      }
    }
  }
  riskScore = constrain(riskScore, 0.0, SCORE_MAX);
}

void applyActions() {
  float warningThreshold = getWarningThreshold();
  static bool alertSent = false;
  
  if (riskScore >= warningThreshold && !alertSent && !isShutdown) {
    sendTelegramAlert("⚠️ [방치 경고] 발열기기가 켜진 상태로 자리를 비웠습니다. 곧 차단됩니다!");
    alertSent = true;
  }
  if (riskScore < warningThreshold) alertSent = false;

  if (riskScore >= SCORE_MAX && !isShutdown) {
    if (relayIsOn) {
      relayOff();
      sendTelegramAlert("🚨 [자동 차단] 장시간 방치로 전원이 차단되었습니다.");
    }
    isShutdown = true;
    buzzerOff();
    return;
  }

  if (isShutdown) {
    // 🚨 반드시 사람이 있어야만(lastPersonHere == true) 릴레이가 다시 켜짐
    if (!relayIsOn && lastPersonHere && riskScore <= warningThreshold) {
      relayOn();
      isShutdown = false;
      sendTelegramAlert("✅ [안전 복구] 사람이 감지되어 전원 차단이 해제되었습니다.");
    } else {
      return; // 사람 없으면 릴레이 꺼진 상태 무한 유지
    }
  }

  // 부저 패턴 제어
  if (riskScore >= warningThreshold) {
    float progress = (riskScore - warningThreshold) / (SCORE_MAX - warningThreshold);
    progress = constrain(progress, 0.0, 1.0);
    int interval = BUZZER_INTERVAL_SLOW_MS - (int)(progress * (BUZZER_INTERVAL_SLOW_MS - BUZZER_INTERVAL_FAST_MS));
    buzzerPattern(interval);
  } else {
    buzzerOff();
  }
}

// =============================================================
//  UI 웹 페이지
// =============================================================
const char* MAIN_PAGE = R"HTML(
<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>스마트 방치감지 콘센트 V7.1</title>
<style>
  :root {
    --bg-color: #0d1117; --card-bg: rgba(22, 27, 34, 0.85); --border-color: rgba(255, 255, 255, 0.1);
    --accent-green: #2ea043; --accent-warn: #d29922; --accent-danger: #f85149; --accent-cyan: #58a6ff;
    --text-main: #f0f6fc; --text-sub: #8b949e;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, sans-serif; }
  body { background: var(--bg-color); color: var(--text-main); padding: 16px; display: flex; justify-content: center; padding-bottom: 50px; }
  .container { width: 100%; max-width: 440px; }
  header { text-align: center; margin-bottom: 20px; }
  header h1 { font-size: 20px; font-weight: 700; color: var(--text-main); margin-bottom: 4px; }
  header p { font-size: 13px; color: var(--text-sub); }
  
  .card { background: var(--card-bg); border: 1px solid var(--border-color); border-radius: 16px; padding: 20px; margin-bottom: 16px; box-shadow: 0 8px 24px rgba(0,0,0,0.3); backdrop-filter: blur(8px); }
  .status-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; }
  .status-badge { padding: 4px 12px; border-radius: 20px; font-size: 13px; font-weight: bold; }
  
  .score-display { text-align: center; margin: 15px 0 5px; }
  .score-num { font-size: 52px; font-weight: 800; letter-spacing: -1px; }
  .countdown { font-size: 14px; font-weight: 600; margin-bottom: 15px; text-align: center; height: 20px; }
  
  .bar-container { background: #30363d; border-radius: 10px; height: 14px; overflow: hidden; margin-bottom: 20px; }
  .bar-fill { background: linear-gradient(90deg, #2ea043, #d29922, #f85149); height: 100%; width: 0%; transition: 0.4s; }
  
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
  .grid-item { background: rgba(255,255,255,0.03); border: 1px solid var(--border-color); border-radius: 12px; padding: 14px; text-align: center; position: relative; }
  .grid-item .title { font-size: 12px; color: var(--text-sub); margin-bottom: 6px; }
  .grid-item .value { font-size: 20px; font-weight: bold; color: var(--text-main); }
  .device-on-badge { background: rgba(248,81,73,0.2); color: #f85149; font-size: 11px; padding: 2px 6px; border-radius: 10px; border: 1px solid #f85149; margin-top: 6px; display: inline-block; }
  .device-off-badge { background: rgba(139,148,158,0.2); color: #8b949e; font-size: 11px; padding: 2px 6px; border-radius: 10px; border: 1px solid #8b949e; margin-top: 6px; display: inline-block; }
  
  canvas { width: 100%; height: 60px; background: rgba(0,0,0,0.2); border-radius: 8px; margin-top: 15px; border: 1px solid var(--border-color); }
  
  h2 { font-size: 16px; font-weight: 600; margin-bottom: 16px; display: flex; align-items: center; justify-content: space-between; }
  
  .btn-power-group { display: flex; gap: 10px; margin-bottom: 5px; }
  .btn-pwr { flex: 1; padding: 12px; border-radius: 10px; border: 1px solid; font-weight: 600; font-size: 14px; cursor: pointer; text-align: center; }
  .btn-pwr-off { background: rgba(248,81,73,0.1); color: #f85149; border-color: #f85149; }
  .btn-pwr-on { background: rgba(46,160,67,0.1); color: #2ea043; border-color: #2ea043; }

  .help-icon { display: inline-block; width: 16px; height: 16px; background: #444; border-radius: 50%; text-align: center; line-height: 16px; font-size: 11px; font-weight: bold; color: #fff; cursor: pointer; margin-left: 6px; }
  
  .param-group { margin-bottom: 12px; }
  .param-label-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 4px; }
  .param-label { font-size: 13px; font-weight: 500; display: flex; align-items: center; }
  .param-default { font-size: 11px; color: var(--text-sub); }
  
  .input-wrapper { display: flex; background: #0d1117; border: 1px solid #30363d; border-radius: 8px; overflow: hidden; height: 38px; }
  .input-wrapper input { flex: 1; min-width: 0; border: none; background: transparent; padding: 0 12px; color: var(--text-main); font-size: 14px; outline: none; }
  .input-wrapper input:focus { background: rgba(255,255,255,0.05); }
  .input-wrapper button { background: #21262d; border: none; border-left: 1px solid #30363d; color: var(--text-sub); font-size: 12px; padding: 0 16px; cursor: pointer; white-space: nowrap; }

  .btn-group { display: flex; gap: 8px; margin-top: 20px; }
  .btn-main { flex: 2; background: var(--accent-green); border: none; border-radius: 8px; color: white; font-size: 14px; font-weight: 600; padding: 12px; cursor: pointer; }
  .btn-reset-all { flex: 1; background: #21262d; border: 1px solid var(--border-color); border-radius: 8px; color: var(--text-sub); font-size: 13px; cursor: pointer; }
</style>
</head>
<body>
<div class="container">
  <header>
    <h1>방치감지 스마트 콘센트</h1>
    <p>실시간 모니터링 & 원격 제어 V7.1</p>
  </header>

  <div class="card">
    <div class="status-header">
      <span style="font-size: 14px; font-weight: 600;">시스템 상태</span>
      <span class="status-badge" id="state-badge">로딩중...</span>
    </div>
    
    <div class="score-display">
      <div class="score-num" id="score">0.0</div>
      <div class="score-label" style="color:var(--text-sub); font-size:12px;">방치 위험도 점수</div>
    </div>
    
    <div class="countdown" id="countdown">안전 대기 중</div>
    
    <div class="bar-container">
      <div class="bar-fill" id="bar-fill"></div>
    </div>

    <div class="grid">
      <div class="grid-item">
        <div class="title">⚡ 소비 전류</div>
        <div class="value"><span id="current">0.00</span> A</div>
        <div id="device-badge" class="device-off-badge">💤 대기 중 (OFF)</div>
      </div>
      <div class="grid-item">
        <div class="title">👤 사람 감지</div>
        <div class="value" id="person">없음</div>
        <div style="font-size:11px; color:var(--text-sub); margin-top:6px;">레이더 센서</div>
      </div>
    </div>
    
    <canvas id="graphCanvas" width="300" height="60"></canvas>
  </div>

  <div class="card">
    <h2>🕹️ 원격 전원 제어 <span class="help-icon" onclick="alert('멀리서도 릴레이 전원을 강제로 차단하거나 복구할 수 있습니다.')">?</span></h2>
    <div class="btn-power-group">
      <button class="btn-pwr btn-pwr-off" onclick="overridePwr('off')">강제 차단 (OFF)</button>
      <button class="btn-pwr btn-pwr-on" onclick="overridePwr('on')">전원 복구 (ON)</button>
    </div>
  </div>

  <div class="card">
    <h2>⚙️ 맞춤형 안전 설정</h2>
    <form id="param-form">
      
      <!-- ★ 켜짐 판정 전류 복구 완료 -->
      <div class="param-group">
        <div class="param-label-row">
          <span class="param-label">켜짐 판정 전류 (A) <span class="help-icon" onclick="alert('이 전류를 넘어서면 기기가 켜진 것으로 판단합니다.')">?</span></span>
          <span class="param-default">기준: 0.35A</span>
        </div>
        <div class="input-wrapper">
          <input type="number" step="0.05" name="cthresh" id="in-cthresh" placeholder="0.35">
          <button type="button" onclick="setDefault('in-cthresh', 0.35)">기본값</button>
        </div>
      </div>

      <div class="param-group">
        <div class="param-label-row">
          <span class="param-label">방치 시 자동 차단 시간</span>
          <span class="param-default">기준: 300초 (5분)</span>
        </div>
        <div class="input-wrapper">
          <input type="number" step="10" name="cutoff" id="in-cutoff" placeholder="300">
          <button type="button" onclick="setDefault('in-cutoff', 300)">기본값</button>
        </div>
      </div>
      <div class="param-group">
        <div class="param-label-row">
          <span class="param-label">차단 전 경고 알림 시점</span>
          <span class="param-default">기준: 30초 전</span>
        </div>
        <div class="input-wrapper">
          <input type="number" step="5" name="warnsec" id="in-warnsec" placeholder="30">
          <button type="button" onclick="setDefault('in-warnsec', 30)">기본값</button>
        </div>
      </div>
      <div class="param-group">
        <div class="param-label-row">
          <span class="param-label">사람 복귀 시 회복 속도</span>
          <span class="param-default">기준: 45초</span>
        </div>
        <div class="input-wrapper">
          <input type="number" step="5" name="recovery" id="in-recovery" placeholder="45">
          <button type="button" onclick="setDefault('in-recovery', 45)">기본값</button>
        </div>
      </div>
      <div class="btn-group">
        <button type="button" class="btn-reset-all" onclick="resetAllDefaults()">전체 기본값</button>
        <button type="submit" class="btn-main">설정값 저장</button>
      </div>
    </form>
  </div>
</div>

<script>
let histScore = Array(30).fill(0);
let histCurr = Array(30).fill(0);

function setDefault(id, val) { document.getElementById(id).value = val; }
function resetAllDefaults() {
  setDefault('in-cthresh', 0.35); // ★ 기본값 0.35 추가
  setDefault('in-cutoff', 300); 
  setDefault('in-warnsec', 30);
  setDefault('in-recovery', 45);
}
function overridePwr(cmd) {
  if(confirm(cmd === 'off' ? '정말 콘센트 전원을 강제로 차단할까요?' : '전원을 다시 공급하고 위험도를 초기화할까요?')) {
    fetch('/override?cmd=' + cmd).then(() => refresh());
  }
}

function drawGraph() {
  const canvas = document.getElementById('graphCanvas');
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  let w = canvas.width, h = canvas.height, step = w / 29;
  
  ctx.beginPath();
  ctx.moveTo(0, h);
  for(let i=0; i<30; i++) ctx.lineTo(i*step, h - (histScore[i] / 100 * h));
  ctx.lineTo(w, h);
  ctx.fillStyle = 'rgba(248,81,73,0.15)'; ctx.fill();

  ctx.beginPath();
  for(let i=0; i<30; i++) {
    let y = h - (Math.min(histCurr[i], 15) / 15 * h);
    if(i===0) ctx.moveTo(0, y); else ctx.lineTo(i*step, y);
  }
  ctx.strokeStyle = '#58a6ff'; ctx.lineWidth = 1.5; ctx.stroke();
}

function refresh() {
  fetch('/status').then(r => r.json()).then(d => {
    document.getElementById('score').innerText = d.score.toFixed(1);
    document.getElementById('bar-fill').style.width = (d.score / d.max * 100) + '%';
    document.getElementById('current').innerText = d.current.toFixed(2);
    document.getElementById('person').innerText = d.person ? '있음' : '없음';
    document.getElementById('person').style.color = d.person ? 'var(--accent-green)' : 'var(--text-sub)';

    const dBadge = document.getElementById('device-badge');
    if(d.deviceOn) {
      dBadge.className = 'device-on-badge'; dBadge.innerText = '🔥 작동 중 (ON)';
    } else {
      dBadge.className = 'device-off-badge'; dBadge.innerText = '💤 대기 중 (OFF)';
    }

    const cDown = document.getElementById('countdown');
    if(d.timeLeft >= 0) {
      let m = Math.floor(d.timeLeft / 60);
      let s = Math.floor(d.timeLeft % 60);
      cDown.innerText = `차단까지 남은 시간 - ${m}분 ${s}초`;
      cDown.style.color = 'var(--accent-danger)';
    } else {
      cDown.innerText = d.state === '차단됨' ? '전원 차단됨' : '안전 상태 유지 중';
      cDown.style.color = 'var(--text-sub)';
    }

    const badge = document.getElementById('state-badge');
    badge.innerText = d.state;
    if (d.state === '차단됨') {
      badge.style.background = 'rgba(248,81,73,0.2)'; badge.style.color = '#f85149';
    } else if (d.state === '경고중') {
      badge.style.background = 'rgba(210,153,34,0.2)'; badge.style.color = '#d29922';
    } else {
      badge.style.background = 'rgba(46,160,67,0.2)'; badge.style.color = '#2ea043';
    }
    
    histScore.shift(); histScore.push(d.score);
    histCurr.shift(); histCurr.push(d.current);
    drawGraph();
  });
}
setInterval(refresh, 1000); refresh();

document.getElementById('param-form').addEventListener('submit', function(e) {
  e.preventDefault();
  fetch('/setparams', { method: 'POST', body: new FormData(e.target) })
    .then(() => alert('설정값이 적용되었습니다.'));
});
</script>
</body>
</html>
)HTML";

// =============================================================
//  웹서버 핸들러
// =============================================================
void handleRoot() { server.send(200, "text/html", MAIN_PAGE); }

void handleStatus() {
  bool deviceOn = (lastCurrent > CURRENT_ON_THRESHOLD);
  float incPerSec = SCORE_MAX / targetCutoffSeconds;
  float timeLeft = -1;
  
  if (deviceOn && !lastPersonHere && !isShutdown) {
    if (incPerSec > 0) timeLeft = (SCORE_MAX - riskScore) / incPerSec;
  }

  float warningThreshold = getWarningThreshold();
  String state = isShutdown ? "차단됨" : (riskScore >= warningThreshold ? "경고중" : "정상");
  
  String json = "{";
  json += "\"state\":\"" + state + "\",";
  json += "\"score\":" + String(riskScore, 1) + ",";
  json += "\"max\":" + String(SCORE_MAX, 0) + ",";
  json += "\"current\":" + String(lastCurrent, 2) + ",";
  json += "\"person\":" + String(lastPersonHere ? "true" : "false") + ",";
  json += "\"deviceOn\":" + String(deviceOn ? "true" : "false") + ",";
  json += "\"timeLeft\":" + String(timeLeft, 0);
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetParams() {
  // ★ cthresh(켜짐 판정 전류) 수신 로직 복구
  if (server.hasArg("cthresh") && server.arg("cthresh").length() > 0) CURRENT_ON_THRESHOLD = server.arg("cthresh").toFloat();
  if (server.hasArg("cutoff") && server.arg("cutoff").length() > 0) targetCutoffSeconds = server.arg("cutoff").toFloat();
  if (server.hasArg("warnsec") && server.arg("warnsec").length() > 0) warningBeforeSeconds = server.arg("warnsec").toFloat();
  if (server.hasArg("recovery") && server.arg("recovery").length() > 0) targetRecoverySeconds = server.arg("recovery").toFloat();

  if (targetCutoffSeconds < 5) targetCutoffSeconds = 5;   // ★ 수정: 최소값을 30→5초로 낮춤 (테스트 편의)
  if (warningBeforeSeconds < 5) warningBeforeSeconds = 5;
  if (targetRecoverySeconds < 5) targetRecoverySeconds = 5;

  server.send(200, "text/plain", "OK");
}

void handleOverride() {
  if (server.hasArg("cmd")) {
    String cmd = server.arg("cmd");
    if (cmd == "off") { relayOff(); isShutdown = true; riskScore = SCORE_MAX; buzzerOff(); }
    else if (cmd == "on") { relayOn(); isShutdown = false; riskScore = 0.0; buzzerOff(); }
  }
  server.send(200, "text/plain", "OK");
}

// =============================================================
//  Setup & Loop
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_RADAR, INPUT);
  pinMode(PIN_SSR, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  relayOn();
  buzzerOff();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi 연결 중");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("웹 대시보드 주소: "); Serial.println(WiFi.localIP());

  telegramClient.setInsecure();

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/setparams", HTTP_POST, handleSetParams);
  server.on("/override", handleOverride);
  server.begin();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastLoopTime >= (unsigned long)(LOOP_INTERVAL_SEC * 1000)) {
    lastLoopTime = now;

    lastCurrent = readCurrentAmps();
    lastPersonHere = isPersonDetected();
    
    // 메모리 로직 없음, 전류 기준(0.35A) 넘으면 즉시 켜짐 판별!
    bool deviceOn = (lastCurrent > CURRENT_ON_THRESHOLD);

    updateRiskScore(deviceOn, lastPersonHere);
    applyActions();

    Serial.print("전류: "); Serial.print(lastCurrent, 2);
    Serial.print("A | 사람: "); Serial.print(lastPersonHere ? "O" : "X");
    Serial.print(" | 상태: "); Serial.print(deviceOn ? "🔥ON" : "💤OFF");
    Serial.print(" | 위험도: "); Serial.println(riskScore, 1);
  }
}