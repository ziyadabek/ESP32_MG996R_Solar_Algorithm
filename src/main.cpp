#include <Arduino.h>
#include <EEPROM.h>
#include <ESP32Servo.h>
#include <SunPosition.h>
#include <WebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <time.h>

const int PIN_HOR = 5;
const int PIN_VER = 18;
const int PIN_VOLTAGE = 34; // Вход делителя напряжения

// Калибровка вольтметра
const float R1 = 10000.0;
const float R2 = 5100.0;
const float V_REF = 3.3;

Servo servoHor;
Servo servoVer;
WebServer server(80);

// Память
struct Config {
  byte magic;
  char ssid[32];
  char pass[32];
  int gmt;
  float lat;
  float lon;
  int verMin;
  int verMax;
  int hOff;
  int vOff;
};

Config cfg;

// Данные
int mode = 0; // 0-Авто, 1-Ручной, 2-Калибровка, 3-Демо
int currentHor = 90;
int currentVer = 90;
float sunAz = 0;
float sunAlt = 0;
float panelVolts = 0.0;

// Переменные для демо-режима
int demoHor = 0;
int demoDirHor = 1;

bool needReboot = false;
bool isAPMode = false;
bool isNight = false; // Флаг ночного режима

SemaphoreHandle_t dataMutex;

// ================= СОВРЕМЕННЫЙ ВЕБ-ИНТЕРФЕЙС =================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Solar Tracker OS</title>
  <style>
    :root { 
      --bg: #0f172a; --card: #1e293b; --text: #f8fafc; 
      --accent: #10b981; --accent-hover: #059669; --blue: #3b82f6; --night: #8b5cf6; --demo: #f59e0b;
    }
    body { font-family: 'Segoe UI', system-ui, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 15px; }
    h2, h3 { margin-top: 0; color: #94a3b8; font-weight: 500; font-size: 1rem; text-transform: uppercase; letter-spacing: 1px; border-bottom: 1px solid #334155; padding-bottom: 8px; }
    
    .nav { display: flex; gap: 10px; margin-bottom: 20px; max-width: 800px; margin-left: auto; margin-right: auto; }
    .nav button { flex: 1; padding: 15px; background: var(--card); color: #94a3b8; border: 2px solid transparent; border-radius: 10px; font-weight: bold; cursor: pointer; transition: 0.3s; font-size: 1rem; }
    .nav button.active { background: #064e3b; color: var(--accent); border-color: var(--accent); }
    
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 15px; max-width: 800px; margin: 0 auto; }
    .card { background: var(--card); padding: 20px; border-radius: 12px; box-shadow: 0 10px 15px -3px rgba(0,0,0,0.5); }
    .card-full { grid-column: 1 / -1; }
    
    .stat-row { display: flex; justify-content: space-between; margin-bottom: 12px; font-size: 1.1rem; }
    .stat-val { font-weight: 600; color: var(--blue); }
    .volt-box { text-align: center; padding: 10px 0; }
    .volt-val { font-size: 4rem; font-weight: 800; color: var(--accent); text-shadow: 0 0 20px rgba(16,185,129,0.4); line-height: 1; }
    .volt-label { font-size: 1.2rem; color: #94a3b8; margin-top: 5px; }
    
    .btn-group { display: flex; flex-direction: column; gap: 10px; }
    button.action-btn { padding: 12px; border: none; border-radius: 8px; background: #334155; color: white; font-weight: bold; cursor: pointer; transition: 0.2s; }
    button.action-btn.active { background: var(--blue); }
    button.save-btn { background: var(--accent); color: #000; width: 100%; padding: 15px; font-size: 1.1rem; margin-top: 10px; }
    button.save-btn:hover { background: var(--accent-hover); }
    
    label { display: block; margin-bottom: 5px; color: #cbd5e1; font-size: 0.9rem; }
    input, select { width: 100%; padding: 12px; margin-bottom: 15px; background: #0f172a; border: 1px solid #334155; color: white; border-radius: 8px; box-sizing: border-box; }
    input:focus { outline: none; border-color: var(--blue); }
    input[type=range] { -webkit-appearance: none; background: #0f172a; height: 10px; border-radius: 5px; outline: none; border: 1px solid #334155; }
    input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 24px; height: 24px; border-radius: 50%; background: var(--blue); cursor: pointer; }
    
    .flex-row { display: flex; gap: 10px; }
    .flex-row > div { flex: 1; }
    .flex-row button { margin-bottom: 15px; }
  </style>
</head>
<body>

  <div class="nav">
    <button id="tab-dash" class="active" onclick="switchTab('dash')">ДАШБОРД</button>
    <button id="tab-setup" onclick="switchTab('setup')">НАСТРОЙКИ</button>
  </div>

  <div id="view-dash" class="grid">
    <div class="card card-full volt-box">
      <h2>ГЕНЕРАЦИЯ ПАНЕЛИ</h2>
      <div class="volt-val"><span id="volts">0.00</span></div>
      <div class="volt-label">ВОЛЬТ (V)</div>
    </div>
    
    <div class="card">
      <h2>ИНФОРМАЦИЯ</h2>
      <div class="stat-row"><span>Время:</span> <span class="stat-val" id="time">--:--:--</span></div>
      <div class="stat-row"><span>Солнце Азимут:</span> <span class="stat-val" id="sunAz">--°</span></div>
      <div class="stat-row"><span>Солнце Высота:</span> <span class="stat-val" id="sunAlt">--°</span></div>
      <div class="stat-row"><span>Механика Гор:</span> <span class="stat-val" id="curHor">--°</span></div>
      <div class="stat-row"><span>Механика Вер:</span> <span class="stat-val" id="curVer">--°</span></div>
    </div>

    <div class="card">
      <h2>РЕЖИМ РАБОТЫ</h2>
      <div class="btn-group">
        <button id="btn0" class="action-btn" onclick="setMode(0)">АВТО (СЛЕЖЕНИЕ)</button>
        <button id="btn3" class="action-btn" onclick="setMode(3)">🚀 ДЕМО ДЛЯ ПРЕЗЕНТАЦИИ</button>
        <button id="btn1" class="action-btn" onclick="setMode(1)">РУЧНОЙ РЕЖИМ</button>
        <button id="btn2" class="action-btn" onclick="setMode(2)">КАЛИБРОВКА (90°)</button>
      </div>
      
      <div id="manualPanel" style="display:none; margin-top:20px;">
        <label>Горизонт (Азимут): <span id="vH" style="color:var(--accent)">90</span>°</label>
        <input type="range" id="slH" min="0" max="180" oninput="document.getElementById('vH').innerText=this.value" onchange="sendManual()">
        <label>Вертикаль (Наклон): <span id="vV" style="color:var(--accent)">90</span>°</label>
        <input type="range" id="slV" min="0" max="90" oninput="document.getElementById('vV').innerText=this.value" onchange="sendManual()">
      </div>
    </div>
  </div>

  <div id="view-setup" class="grid" style="display:none;">
    <div class="card card-full">
      <h2>КОНФИГУРАЦИЯ СИСТЕМЫ</h2>
      <form id="cfgForm" onsubmit="saveCfg(event)">
        <div class="flex-row">
          <div><label>Широта (Lat)</label><input type="text" id="lat"></div>
          <div><label>Долгота (Lon)</label><input type="text" id="lon"></div>
          <div><label>GMT (Пояс)</label><input type="number" id="gmt"></div>
        </div>
        
        <div class="flex-row">
          <div><label>Мин. Наклон</label><input type="number" id="verMin"></div>
          <div><label>Макс. Наклон</label><input type="number" id="verMax"></div>
        </div>
        
        <div class="flex-row">
          <div><label>Оффсет Азимут</label><input type="number" id="hOff"></div>
          <div><label>Оффсет Наклон</label><input type="number" id="vOff"></div>
        </div>

        <label>WiFi Имя (SSID)</label>
        <div class="flex-row">
          <input type="text" id="ssid" placeholder="Название сети">
          <select id="ssidSelect" style="display:none;" onchange="document.getElementById('ssid').value=this.value;"></select>
          <button type="button" class="action-btn" id="scanBtn" onclick="scanWifi()" style="width:120px;">ПОИСК</button>
        </div>
        
        <label>WiFi Пароль</label>
        <input type="text" id="pass">
        
        <button type="submit" class="action-btn save-btn">СОХРАНИТЬ ДАННЫЕ В EEPROM</button>
      </form>
    </div>
  </div>

  <script>
    function switchTab(tab) {
      document.getElementById('view-dash').style.display = (tab === 'dash') ? 'grid' : 'none';
      document.getElementById('view-setup').style.display = (tab === 'setup') ? 'grid' : 'none';
      document.getElementById('tab-dash').className = (tab === 'dash') ? 'active' : '';
      document.getElementById('tab-setup').className = (tab === 'setup') ? 'active' : '';
    }

    function update() {
      fetch('/api/status').then(r=>r.json()).then(d=>{
        document.getElementById('time').innerText = d.time;
        document.getElementById('volts').innerText = d.volts.toFixed(2);
        document.getElementById('sunAz').innerText = d.sunAz.toFixed(1) + '°';
        document.getElementById('sunAlt').innerText = d.sunAlt.toFixed(1) + '°';
        document.getElementById('curHor').innerText = d.curHor + '°';
        document.getElementById('curVer').innerText = d.curVer + '°';
        
        // Логика ночного режима
        let btn0 = document.getElementById('btn0');
        if(d.mode == 0) {
          btn0.className = 'action-btn active';
          if(d.isNight) {
            btn0.innerText = '🌙 НОЧЬ (ЭНЕРГОСБЕРЕЖЕНИЕ)';
            btn0.style.backgroundColor = 'var(--night)';
          } else {
            btn0.innerText = 'АВТО (СЛЕЖЕНИЕ)';
            btn0.style.backgroundColor = 'var(--blue)';
          }
        } else {
          btn0.className = 'action-btn';
          btn0.innerText = 'АВТО (СЛЕЖЕНИЕ)';
          btn0.style.backgroundColor = '#334155';
        }

        let btn3 = document.getElementById('btn3');
        if(d.mode == 3) {
          btn3.className = 'action-btn active';
          btn3.innerText = '🚀 ДЕМО В ПРОЦЕССЕ...';
          btn3.style.backgroundColor = 'var(--demo)';
        } else {
          btn3.className = 'action-btn';
          btn3.innerText = '🚀 ДЕМО ДЛЯ ПРЕЗЕНТАЦИИ';
          btn3.style.backgroundColor = '#334155';
        }

        document.getElementById('btn1').className = 'action-btn ' + (d.mode==1 ? 'active' : '');
        document.getElementById('btn2').className = 'action-btn ' + (d.mode==2 ? 'active' : '');
        document.getElementById('manualPanel').style.display = d.mode==1 ? 'block' : 'none';

        if(!document.getElementById('cfgForm').classList.contains('loaded')){
          document.getElementById('lat').value = d.lat; document.getElementById('lon').value = d.lon;
          document.getElementById('gmt').value = d.gmt; document.getElementById('verMin').value = d.verMin;
          document.getElementById('verMax').value = d.verMax; document.getElementById('hOff').value = d.hOff;
          document.getElementById('vOff').value = d.vOff; document.getElementById('ssid').value = d.ssid;
          document.getElementById('cfgForm').classList.add('loaded');
          if (d.isAP) switchTab('setup'); 
        }
      });
    }

    function scanWifi() {
      let b = document.getElementById('scanBtn'); b.innerText = 'ПОИСК...';
      fetch('/api/scan').then(r=>r.json()).then(d=>{
        let s = document.getElementById('ssidSelect'); let i = document.getElementById('ssid');
        s.innerHTML = '<option value="">Выберите сеть</option>';
        d.forEach(n => s.innerHTML += `<option value="${n.ssid}">${n.ssid} (${n.rssi}dBm)</option>`);
        i.style.display = 'none'; s.style.display = 'block'; b.innerText = 'ОБНОВИТЬ';
      });
    }
    
    function setMode(m) { fetch('/api/setMode?mode='+m).then(update); }
    
    function sendManual() { 
      let h = document.getElementById('slH').value; let v = document.getElementById('slV').value;
      fetch(`/api/setManual?h=${h}&v=${v}`).then(update); 
    }
    
    function saveCfg(e) {
      e.preventDefault();
      let p = new URLSearchParams();
      ['lat','lon','gmt','verMin','verMax','hOff','vOff','ssid','pass'].forEach(k => p.set(k, document.getElementById(k).value));
      fetch('/api/saveCfg?'+p.toString()).then(()=>alert('Настройки сохранены! ESP32 перезагружается...'));
    }

    setInterval(update, 2000); 
    update();
  </script>
</body>
</html>
)rawliteral";

// Загрузка EEPROM
void loadSettings() {
  EEPROM.begin(512);
  EEPROM.get(0, cfg);
  if (cfg.magic != 123) {
    cfg.magic = 123;
    strlcpy(cfg.ssid, "", sizeof(cfg.ssid));
    strlcpy(cfg.pass, "", sizeof(cfg.pass));
    cfg.gmt = 5;
    cfg.lat = 51.1333;
    cfg.lon = 71.4333;
    cfg.verMin = 15;
    cfg.verMax = 90;
    cfg.hOff = 0;
    cfg.vOff = 0;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }
}

// Проверка включения сервоприводов
void ensureServosAttached() {
  if (!servoHor.attached())
    servoHor.attach(PIN_HOR, 500, 2400);
  if (!servoVer.attached())
    servoVer.attach(PIN_VER, 500, 2400);
}

// Отключение сервоприводов (Сон)
void detachServos() {
  if (servoHor.attached())
    servoHor.detach();
  if (servoVer.attached())
    servoVer.detach();
}

// Моментальная установка
void setServos(int h, int v) {
  ensureServosAttached();
  currentHor = constrain(h, 0, 180);
  currentVer = constrain(v, cfg.verMin, cfg.verMax);
  servoHor.write(currentHor);
  servoVer.write(currentVer);
}

// Плавный поворот без рывков
void smoothMove(int targetH, int targetV) {
  ensureServosAttached();
  targetH = constrain(targetH, 0, 180);
  targetV = constrain(targetV, cfg.verMin, cfg.verMax);

  while (currentHor != targetH || currentVer != targetV) {
    if (currentHor < targetH)
      currentHor++;
    else if (currentHor > targetH)
      currentHor--;

    if (currentVer < targetV)
      currentVer++;
    else if (currentVer > targetV)
      currentVer--;

    servoHor.write(currentHor);
    servoVer.write(currentVer);
    vTaskDelay(40 / portTICK_PERIOD_MS); // Плавная скорость
  }
}

void setupRouting() {
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", index_html); });

  server.on("/api/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; ++i) {
      if (i > 0)
        json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) +
              "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    server.send(200, "application/json", json);
    WiFi.scanDelete();
  });

  server.on("/api/status", HTTP_GET, []() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    time_t now;
    time(&now);
    struct tm timeinfo;
    char timeStr[10] = "00:00:00";
    if (getLocalTime(&timeinfo))
      strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

    String json = "{";
    json += "\"time\":\"" + String(timeStr) + "\",";
    json += "\"volts\":" + String(panelVolts) + ",";
    json += "\"sunAz\":" + String(sunAz) + ",";
    json += "\"sunAlt\":" + String(sunAlt) + ",";
    json += "\"curHor\":" + String(currentHor) + ",";
    json += "\"curVer\":" + String(currentVer) + ",";
    json += "\"mode\":" + String(mode) + ",";
    json += "\"isAP\":" + String(isAPMode ? "true" : "false") + ",";
    json += "\"isNight\":" + String(isNight ? "true" : "false") + ",";
    json += "\"lat\":" + String(cfg.lat) + ",\"lon\":" + String(cfg.lon) + ",";
    json += "\"gmt\":" + String(cfg.gmt) + ",\"verMin\":" + String(cfg.verMin) +
            ",";
    json += "\"verMax\":" + String(cfg.verMax) +
            ",\"hOff\":" + String(cfg.hOff) + ",";
    json += "\"vOff\":" + String(cfg.vOff) + ",\"ssid\":\"" + String(cfg.ssid) +
            "\"}";
    xSemaphoreGive(dataMutex);
    server.send(200, "application/json", json);
  });

  server.on("/api/setMode", HTTP_GET, []() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (server.hasArg("mode")) {
      mode = server.arg("mode").toInt();
      if (mode == 3) { // Инициализация демо режима
        demoHor = 0;
        demoDirHor = 1;
        setServos(demoHor, cfg.verMin);
      }
    }
    xSemaphoreGive(dataMutex);
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/setManual", HTTP_GET, []() {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (mode == 1 && server.hasArg("h") && server.hasArg("v")) {
      setServos(server.arg("h").toInt(), server.arg("v").toInt());
    }
    xSemaphoreGive(dataMutex);
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/saveCfg", HTTP_GET, []() {
    if (server.hasArg("ssid"))
      strlcpy(cfg.ssid, server.arg("ssid").c_str(), sizeof(cfg.ssid));
    if (server.hasArg("pass"))
      strlcpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass));
    if (server.hasArg("lat"))
      cfg.lat = server.arg("lat").toFloat();
    if (server.hasArg("lon"))
      cfg.lon = server.arg("lon").toFloat();
    if (server.hasArg("gmt"))
      cfg.gmt = server.arg("gmt").toInt();
    if (server.hasArg("verMin"))
      cfg.verMin = server.arg("verMin").toInt();
    if (server.hasArg("verMax"))
      cfg.verMax = server.arg("verMax").toInt();
    if (server.hasArg("hOff"))
      cfg.hOff = server.arg("hOff").toInt();
    if (server.hasArg("vOff"))
      cfg.vOff = server.arg("vOff").toInt();

    cfg.magic = 123;
    EEPROM.put(0, cfg);
    EEPROM.commit();
    server.send(200, "text/plain", "OK");
    needReboot = true;
  });
}

// ================= ЯДРО 0 (Веб-сервер) =================
void TaskWeb(void *pvParameters) {
  while (true) {
    server.handleClient();
    if (needReboot) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      ESP.restart();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ================= ЯДРО 1 (Механика) =================
void TaskTracker(void *pvParameters) {
  while (true) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);

    // Замер напряжения
    long sum = 0;
    for (int i = 0; i < 10; i++) {
      sum += analogRead(PIN_VOLTAGE);
      vTaskDelay(2 / portTICK_PERIOD_MS);
    }
    float vPin = ((float)sum / 10.0 / 4095.0) * V_REF;
    panelVolts = vPin * ((R1 + R2) / R2);

    // Логика работы
    if (mode != 0) {
      isNight = false;
      ensureServosAttached();

      if (mode == 2) {
        setServos(90, 90);
      } else if (mode == 3) {
        // Демо-режим (Плавное движение)
        demoHor += demoDirHor * 2; // Шаг 2 градуса

        if (demoHor >= 180) {
          demoHor = 180;
          demoDirHor = -1;
        } else if (demoHor <= 0) {
          demoHor = 0;
          demoDirHor = 1;
        }

        // Вертикаль имитирует солнце (подъем в центре, опускание по краям)
        // Математика: парабола, где на 90 градусах азимута наклон максимальный
        // (verMax)
        float progress = abs(demoHor - 90) / 90.0; // от 0 (центр) до 1 (края)
        int targetV = cfg.verMax - (progress * (cfg.verMax - cfg.verMin));

        setServos(demoHor, targetV);
      }
    } else if (mode == 0 && !isAPMode) {
      time_t now;
      time(&now);
      if (now > 100000) {
        SunPosition sun(cfg.lat, cfg.lon, now, cfg.gmt);
        sunAz = sun.azimuth();
        sunAlt = sun.altitude();

        int targetHor = map(sunAz, 90, 270, 0, 180) + cfg.hOff;
        int targetVer = sunAlt + cfg.vOff;

        if (sunAlt <= 0) {
          if (!isNight) {
            isNight = true;
            detachServos();
          }
        } else {
          if (isNight) {
            isNight = false;
            smoothMove(targetHor, targetVer);
          } else {
            setServos(targetHor, targetVer);
          }
        }
      }
    }

    xSemaphoreGive(dataMutex);

    // В демо-режиме цикл работает быстрее для плавности
    if (mode == 3)
      vTaskDelay(100 / portTICK_PERIOD_MS);
    else
      vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);

  Serial.println();
  Serial.println("==========================================");
  Serial.println("   ☀️  Solar Tracker OS  |  Booting...");
  Serial.println("==========================================");

  dataMutex = xSemaphoreCreateMutex();
  loadSettings();

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ensureServosAttached();

  WiFi.disconnect(true);
  delay(100);

  if (String(cfg.ssid) != "") {
    Serial.print("[WiFi] Подключение к сети: ");
    Serial.println(cfg.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.pass);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
      delay(500);
      Serial.print(".");
      tries++;
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] ОШИБКА подключения! Сброс настроек...");
      cfg.magic = 0;
      EEPROM.put(0, cfg);
      EEPROM.commit();
      ESP.restart();
    }
  }

  if (String(cfg.ssid) == "" || WiFi.status() != WL_CONNECTED) {
    isAPMode = true;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("SolarTracker", "12345678");
    Serial.println();
    Serial.println("------------------------------------------");
    Serial.println("  [РЕЖИМ НАСТРОЙКИ — Точка Доступа (AP)]");
    Serial.println("------------------------------------------");
    Serial.println("  WiFi сеть : SolarTracker");
    Serial.println("  Пароль    : 12345678");
    Serial.print("  IP адрес  : ");
    Serial.println(WiFi.softAPIP());
    Serial.println("  Откройте браузер и введите IP адрес выше");
    Serial.println("------------------------------------------");
  } else {
    isAPMode = false;
    WiFi.mode(WIFI_STA);
    configTime(cfg.gmt * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println();
    Serial.println("------------------------------------------");
    Serial.println("  [РАБОЧИЙ РЕЖИМ — Подключено к WiFi]");
    Serial.println("------------------------------------------");
    Serial.print("  Сеть      : ");
    Serial.println(cfg.ssid);
    Serial.print("  IP адрес  : ");
    Serial.println(WiFi.localIP());
    Serial.print("  GMT пояс  : +");
    Serial.println(cfg.gmt);
    Serial.println("  Откройте браузер и введите IP адрес выше");
    Serial.println("------------------------------------------");
  }

  Serial.println("[OK] Веб-сервер запущен на порту 80");
  Serial.println("[OK] FreeRTOS задачи запущены");
  Serial.println("==========================================");

  setupRouting();
  server.begin();

  xTaskCreatePinnedToCore(TaskWeb, "WebTask", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskTracker, "TrackerTask", 4096, NULL, 1, NULL, 1);
}

void loop() { vTaskDelete(NULL); }