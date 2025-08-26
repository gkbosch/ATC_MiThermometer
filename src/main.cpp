// Main sketch for ESP32-S3: SSD1306 display, LittleFS logging, web server with Highcharts, CSV download/erase
//gkb 8-26025
#include <Arduino.h>
#include <Adafruit_MAX31855.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <time.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// WiFi credentials injected via build flags
#ifndef WIFI_SSID
#define WIFI_SSID 
#endif
#ifndef WIFI_PASS
#define WIFI_PASS 
#endif
// Stringify helpers to turn tokens into C string literals
#ifndef STR_HELPER
#define STR_HELPER(x) #x
#endif
#ifndef STR
#define STR(x) STR_HELPER(x)
#endif
static const char* WIFI_SSID_STR = STR(WIFI_SSID);
static const char* WIFI_PASS_STR = STR(WIFI_PASS);

#ifndef OLED_SDA
#define OLED_SDA 8
#endif
#ifndef OLED_SCL
#define OLED_SCL 9
#endif

const int PIN_CS   = 5;
const int PIN_MISO = 4;
const int PIN_SCK  = 18;
Adafruit_MAX31855 thermo(PIN_SCK, PIN_CS, PIN_MISO);

AsyncWebServer server(80);

struct TempRecord {
  time_t ts;
  float temp;
};

// Active log filename (when recording). Empty means no active recording.
String currentLogFile;
// Default legacy log path used for compatibility with /data when no active file exists
#define LEGACY_LOG_FILE "/templog.csv"
#ifndef LOG_INTERVAL_MS
#define LOG_INTERVAL_MS 15000
#endif
#define LOG_INTERVAL 3000
unsigned long lastLog = 0;
bool isRecording = false;
static time_t startEpochSec = 0; // approximate epoch at boot, set after NTP sync
static unsigned long recordStartMillis = 0; // millis at start of current recording

// Format seconds into HH:MM:SS
String fmtHHMMSS(unsigned long secs) {
  unsigned long h = secs / 3600;
  unsigned long m = (secs % 3600) / 60;
  unsigned long s = secs % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, s);
  return String(buf);
}

// Format seconds into HH:MM
String fmtHHMM(unsigned long secs) {
  unsigned long h = secs / 3600;
  unsigned long m = (secs % 3600) / 60;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", h, m);
  return String(buf);
}

// Parse HH:MM:SS into seconds (returns 0 on failure)
unsigned long parseHHMMSS(const String &t) {
  int p1 = t.indexOf(':'), p2 = t.indexOf(':', p1 + 1);
  if (p1 < 0 || p2 < 0) return 0;
  unsigned long h = t.substring(0, p1).toInt();
  unsigned long m = t.substring(p1 + 1, p2).toInt();
  unsigned long s = t.substring(p2 + 1).toInt();
  return h * 3600UL + m * 60UL + s;
}

void logTemp(float temp) {
  if (!isRecording) return; // only log when recording is active
  String path = currentLogFile.length() ? currentLogFile : String(LEGACY_LOG_FILE);
  // Ensure file exists; create if missing
  if (!LittleFS.exists(path)) {
    File nf = LittleFS.open(path, "w");
    if (nf) nf.close();
  }
  File f = LittleFS.open(path, "a");
  if (!f) {
    Serial.printf("Failed to open log file for append: %s\n", path.c_str());
    return;
  }
  // Log as time since start (HH:MM:SS)
  unsigned long elapsedSec = (millis() - recordStartMillis) / 1000UL;
  String tstr = fmtHHMMSS(elapsedSec);
  f.printf("%s,%.2f\n", tstr.c_str(), temp);
  f.close();
}

// Simple I2C scanner for troubleshooting OLED wiring
void scanI2C() {
  byte count = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("I2C device found at 0x%02X\n", addr);
      count++;
    }
  }
  if (count == 0) Serial.println("No I2C devices found");
}

// Normalize FS names for UI and routing (strip mount prefix and leading slash)
String normalizeName(const char* full) {
  String s(full);
  if (s.startsWith("/littlefs/")) s = s.substring(strlen("/littlefs/"));
  if (s.startsWith("/")) s = s.substring(1);
  return s;
}

// Create filename like /templog-YYYYMMDD-HHMMSS.csv using local time (Chicago)
String makeLogFilename() {
  time_t now = time(nullptr);
  struct tm tinfo;
  localtime_r(&now, &tinfo);
  char buf[40];
  strftime(buf, sizeof(buf), "/templog-%Y%m%d-%H%M%S.csv", &tinfo);
  return String(buf);
}

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }
  // Bring up I2C explicitly before display init (use configured pins)
  Wire.begin(OLED_SDA, OLED_SCL);
  Serial.printf("I2C pins SDA=%d SCL=%d\n", OLED_SDA, OLED_SCL);
  scanI2C();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 0x3C init failed, trying 0x3D...");
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("SSD1306 init failed at both 0x3C and 0x3D");
    }
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("Booting...");
  display.display();
  delay(1000);
  display.clearDisplay();

  thermo.begin();

  // Start Wi-Fi in STA mode
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID_STR) > 0) {
    Serial.print("WiFi STA connecting to "); Serial.println(WIFI_SSID_STR);
    WiFi.begin(WIFI_SSID_STR, WIFI_PASS_STR);
  } else {
    Serial.println("WiFi SSID not set. Set in platformio.ini build_flags.");
  }
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: "); Serial.println(WiFi.localIP());
    display.clearDisplay();
    display.setCursor(0,0);
    display.setTextSize(1);
    display.println("WiFi connected:");
    display.println(WiFi.localIP());
    display.display();

  // Setup NTP and Chicago timezone (CST/CDT)
    setenv("TZ", "CST6CDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Syncing time...");
    for (int i = 0; i < 30; i++) {
      time_t now = time(nullptr);
      if (now > 1609459200) { // > Jan 1, 2021
        Serial.println("done");
    startEpochSec = now - (millis()/1000);
        break;
      }
      delay(500);
      Serial.print(".");
    }
  } else {
    Serial.println("WiFi connect timeout");
    display.clearDisplay();
    display.setCursor(0,0);
    display.setTextSize(1);
    display.println("WiFi timeout");
    display.display();
  }

  // Web server endpoints
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
  String html = "<html><head><script src='https://code.highcharts.com/highcharts.js'></script></head><body>";
  html += "<h2>Current Temp: <span id='temp'></span> C</h2><h3>Status: <span id='status'></span> | Active: <span id='active'></span></h3>";
    html += "<div id='chart' style='height:300px;'></div>";
    html += "<div><button id='startBtn'>Start Recording</button> <button id='stopBtn'>Stop Recording</button></div>";
  html += "<h3>Files</h3><ul id='files'></ul>";
  html += "<script>\n";
    html += String("const SAMPLE_MS=") + String((unsigned long)LOG_INTERVAL_MS) + String(";\n");
  html += "let timer=null;\n"
      "function fmtHHMM(ms){const s=Math.floor(ms/1000);const h=Math.floor(s/3600);const m=Math.floor((s%3600)/60);return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0');}\n"
            "function render(d){document.getElementById('temp').innerText=d.temp.toFixed?d.temp.toFixed(2):d.temp;document.getElementById('active').innerText=d.active||'';document.getElementById('status').innerText=d.recording?('Recording (elapsed '+(d.elapsed_hms||'00:00:00')+')'):'Idle';Highcharts.chart('chart',{title:{text:'Temperature'},xAxis:{type:'linear',labels:{formatter:function(){return fmtHHMM(this.value);}}},yAxis:{title:{text:'Celsius'}},plotOptions:{series:{marker:{enabled:false}}},series:[{name:'Temp',data:d.log}]});}\n"
      "function fetchOnce(){fetch('/data').then(r=>r.json()).then(render);}\n"
      "function startUpdates(){if(timer) return; timer=setInterval(fetchOnce,SAMPLE_MS); fetchOnce();}\n"
      "function stopUpdates(){if(timer){clearInterval(timer); timer=null;} fetchOnce();}\n"
      "function loadFiles(){fetch('/files').then(r=>r.json()).then(list=>{const ul=document.getElementById('files');ul.innerHTML='';list.forEach(f=>{const li=document.createElement('li');li.textContent=f.name+' ('+f.size+' bytes) ';const dl=document.createElement('button');dl.textContent='Download';dl.onclick=()=>{window.location='/csv?name='+encodeURIComponent(f.name)+'&dl=1';};const del=document.createElement('button');del.textContent='Delete';del.onclick=()=>{fetch('/delete?name='+encodeURIComponent(f.name)).then(()=>loadFiles());};li.appendChild(dl);li.appendChild(document.createTextNode(' '));li.appendChild(del);ul.appendChild(li);});});}\n"
      "document.addEventListener('DOMContentLoaded',()=>{document.getElementById('startBtn').onclick=()=>fetch('/start').then(()=>{startUpdates();loadFiles();});document.getElementById('stopBtn').onclick=()=>fetch('/stop').then(()=>{stopUpdates();loadFiles();});fetchOnce();loadFiles();});\n"
      "</script></body></html>";
    request->send(200, "text/html", html);
  });
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    float temp = thermo.readCelsius();
    JsonDocument doc; doc.clear();
    JsonArray arr = doc["log"].to<JsonArray>();
    if (isRecording && currentLogFile.length() && LittleFS.exists(currentLogFile)) {
      File f = LittleFS.open(currentLogFile, "r");
      if (f) {
    while (f.available()) {
          String line = f.readStringUntil('\n');
          int idx = line.indexOf(',');
          if (idx > 0) {
            String tstr = line.substring(0, idx);
            unsigned long secs = parseHHMMSS(tstr);
            float v = line.substring(idx+1).toFloat();
            JsonArray pt = arr.add<JsonArray>();
            pt.add(secs * 1000UL); // milliseconds since start
            pt.add(v);
          }
        }
        f.close();
      }
    }
    doc["temp"] = temp;
    doc["recording"] = isRecording;
    unsigned long elapsedSec = isRecording ? ((millis() - recordStartMillis)/1000UL) : 0UL;
    doc["elapsed"] = elapsedSec;
    doc["elapsed_hms"] = isRecording ? fmtHHMMSS(elapsedSec) : String("");
    doc["active"] = isRecording && currentLogFile.length() ? normalizeName(currentLogFile.c_str()) : String("");
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });
  server.on("/csv", HTTP_GET, [](AsyncWebServerRequest *request){
    String name = request->hasParam("name") ? request->getParam("name")->value() : String("");
    if (name.length()) name = normalizeName(name.c_str());
    String path = name.length() ? "/" + name : (isRecording && currentLogFile.length() ? currentLogFile : String(LEGACY_LOG_FILE));
    bool dl = request->hasParam("dl");
    bool exists = LittleFS.exists(path);
    Serial.printf("/csv path=%s exists=%s\n", path.c_str(), exists?"yes":"no");
    if (!exists) { request->send(404, "text/plain", "Not found"); return; }
    request->send(LittleFS, path, "text/csv", dl);
  });
  server.on("/erase", HTTP_GET, [](AsyncWebServerRequest *request){
    LittleFS.remove(LEGACY_LOG_FILE);
    request->send(200, "text/plain", "Erased legacy log");
  });

  // Start/Stop recording
  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!isRecording) {
      currentLogFile = makeLogFilename();
      File nf = LittleFS.open(currentLogFile, "w");
      if (nf) nf.close();
  recordStartMillis = millis();
      isRecording = true;
  // Take an immediate sample so the UI sees the first point without waiting a full interval
  logTemp(thermo.readCelsius());
      Serial.printf("Recording started: %s\n", currentLogFile.c_str());
    }
    request->send(200, "text/plain", String("Recording: ") + (isRecording?"ON":"OFF") + "\nFile: " + currentLogFile);
  });
  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request){
    isRecording = false;
    Serial.println("Recording stopped");
    request->send(200, "text/plain", "Recording: OFF");
  });

  // List files
  server.on("/files", HTTP_GET, [](AsyncWebServerRequest *request){
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
      File f = root.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          JsonObject o = arr.add<JsonObject>();
          o["name"] = normalizeName(f.name());
          o["size"] = (uint32_t)f.size();
        }
        f = root.openNextFile();
      }
    }
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // Delete file
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("name")) { request->send(400, "text/plain", "name required"); return; }
    String name = normalizeName(request->getParam("name")->value().c_str());
    if (name.indexOf("/") >= 0) { request->send(400, "text/plain", "bad name"); return; }
    String path = "/" + name;
    bool exists = LittleFS.exists(path);
    Serial.printf("/delete path=%s exists=%s\n", path.c_str(), exists?"yes":"no");
    if (!exists) { request->send(404, "text/plain", "not found"); return; }
    if (isRecording && path == currentLogFile) { request->send(409, "text/plain", "busy"); return; }
    LittleFS.remove(path);
    request->send(200, "text/plain", "deleted");
  });
  server.begin();
}

void loop() {
  static unsigned long lastUpdate = 0;
  unsigned long now = millis();
  if (now - lastUpdate > LOG_INTERVAL_MS) {
    float temp = thermo.readCelsius();
    logTemp(temp);
  Serial.printf("Temp: %.2f C, recording=%s, file=%s\n", temp, isRecording?"yes":"no", (isRecording? currentLogFile.c_str(): LEGACY_LOG_FILE));
    display.clearDisplay();
    display.setCursor(0,0);
    display.printf("Temp: %.2f C\n", temp);
  // Show time since start of recording in HH:MM
  unsigned long elapsedSec = isRecording ? ((millis() - recordStartMillis)/1000UL) : 0UL;
  String rec = fmtHHMM(elapsedSec);
  display.printf("Rec: %s\n", rec.c_str());
    display.display();
    lastUpdate = now;
  }
}
