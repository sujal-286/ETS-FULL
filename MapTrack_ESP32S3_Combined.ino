/*
  ══════════════════════════════════════════════════════════════════
  MapTrack — Combined GPS Tracker + Camera Unit (State-Machine Edition)
  Waveshare ESP32-S3-LCD-2 (OV5640 camera) + SIM7600G-H
  ══════════════════════════════════════════════════════════════════

  IMPORTANT — GALLERY_HOST below MUST point at a persistent Flask
  server (e.g. Render, a VPS, PythonAnywhere), NOT Vercel. This
  firmware's gallery_server.py backend holds uploaded images in a
  single process's memory with a background expiry thread — Vercel's
  serverless model spins up isolated, short-lived function instances
  with no shared memory between requests, and typically won't even
  route /upload at all without extra Vercel-specific config. Vercel
  is fine for hosting the static dashboard (index.html) since that
  page talks to Google Sheets directly and needs no backend, but the
  image gallery backend needs a real persistent host.
  ══════════════════════════════════════════════════════════════════
*/

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include <ArduinoJson.h>
#include "img_converters.h"
#include "esp_camera.h"
#include <Arduino_GFX_Library.h>

// ══════════════════════════════════════════
//  Gfx Library Color Definitions (16-bit RGB565)
// ══════════════════════════════════════════
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define WHITE   0xFFFF
#define ORANGE  0xFD20

// ══════════════════════════════════════════
//  HARDWARE PIN MAPPING
// ══════════════════════════════════════════
#define CAM_PIN_PWDN    17
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    8
#define CAM_PIN_SIOD    21
#define CAM_PIN_SIOC    16
#define CAM_PIN_D7      2
#define CAM_PIN_D6      7
#define CAM_PIN_D5      10
#define CAM_PIN_D4      14
#define CAM_PIN_D3      11
#define CAM_PIN_D2      15
#define CAM_PIN_D1      13
#define CAM_PIN_D0      12
#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    4
#define CAM_PIN_PCLK    9

#define LCD_SCLK        39
#define LCD_MOSI        38
#define LCD_MISO        40
#define LCD_DC          42
#define LCD_CS          45
#define LCD_RST         -1
#define LCD_BL          1

#define PIN_BUTTON_WAKE 47
#define PIN_BUTTON_CAM  48

#define MODEM_RX        44
#define MODEM_TX        43
#define MODEM_PWRKEY    18

// ══════════════════════════════════════════
//  SYSTEM STATE LOGIC CONFIGURATIONS
// ══════════════════════════════════════════
enum SystemMode { MODE_SLEEP, MODE_AWAKE, MODE_PREVIEW, MODE_VERIFY };
SystemMode currentMode = MODE_AWAKE;

unsigned long stateTrackingMillis = 0;
unsigned long buttonCamPressTime = 0;
bool lastWakeBtnState = HIGH;
bool lastCamBtnState = HIGH;
unsigned long lastWakeReleaseTime = 0;

// ══════════════════════════════════════════
//  GLOBAL CONFIGURATIONS & VARIABLES
// ══════════════════════════════════════════
const char* apn       = "airtelgprs.com";
const char* apnUser   = "";
const char* apnPass   = "";

const char* scriptHost = "script.google.com";
const char* scriptPath = "/macros/s/AKfycby6JmMEATpBlxuoX3gn-Nfn540p4zGCfziGopYkvxO7fvJY-j2NnjTj5e_Q5kLVPfdj/exec";

// ── SET THIS to your persistent Flask backend (Render, etc) — NOT Vercel. See note at top of file. ──
const char* GALLERY_HOST = "map-track-5sis.vercel.app";
const int   GALLERY_PORT = 443;
const char* GALLERY_PATH = "/upload";

const char* deviceName     = "Field Unit 1";
const char* deviceIdPrefix = "esp32-gps-01";
const char* deviceCategory = "Live GPS";

// Timings
const unsigned long GPS_TIMEOUT_MS = 120000; // 2 Minutes — acquisition window per cycle
const unsigned long GPS_CYCLE_MS   = 900000; // 15 Minutes — how often a new cycle starts
const unsigned long GPS_FIX_MAX_AGE_MS = 600000;

HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, true, 240, 320);

uint8_t *capturedBuffer = NULL;
size_t capturedLength = 0;
int capturedWidth = 0, capturedHeight = 0;

float lastLat = 0, lastLng = 0;
bool hasFix = false;
unsigned long lastFixMillis = 0;
unsigned long lastGpsCycle = 0;

// Non-blocking GNSS variables exactly from Untitled2.txt
enum GpsState { GPS_IDLE, GPS_ACQUIRING };
GpsState gpsState = GPS_IDLE;
unsigned long gpsAcquireStart = 0;
unsigned long lastGpsPoll = 0;
const unsigned long GPS_POLL_INTERVAL_MS = 3000; // 3 Seconds check gap

bool modemReady = false;
unsigned long lastModemRetry = 0;
const unsigned long MODEM_RETRY_INTERVAL_MS = 15000;

// Forward Declarations
bool atWaitFor(const char *target, unsigned long timeoutMs, String *out = nullptr);
void atSend(const String &cmd);
bool bringUpModem();
bool ensureCellularConnected();
bool pushToSheet(float lat, float lon);
bool sendImageToGallery(uint8_t *buf, size_t len, bool haveFix, float lat, float lng);
String urlEncode(const char *s);
String getTimestamp();
String joinUrl(const String &host, const String &path);
void displayFirstPowerOnSplash();
void displayWakeFromSleepScreen();
void displayTelemetry();
void displayUploadCountdown(int secondsLeft);
void startCameraPreview();
void discardCapturedImage();
void uploadImageToServer();
void enterDeepSleepMode();
void wakeUpPeripherals();
void pollGps();

// ══════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== MapTrack Combined Initializing ===");

  pinMode(PIN_BUTTON_WAKE, INPUT_PULLUP);
  pinMode(PIN_BUTTON_CAM, INPUT_PULLUP);
  pinMode(MODEM_PWRKEY, OUTPUT);
  pinMode(LCD_BL, OUTPUT);

  gfx->begin(40000000);

  // Power On Loading Screen
  displayFirstPowerOnSplash();

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  modemReady = bringUpModem();
  if (modemReady) {
    ensureCellularConnected();
  }

  // Camera Configuration Setup
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0; config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2; config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4; config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6; config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sscb_sda = CAM_PIN_SIOD;
  config.pin_sscb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 16000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&config) == ESP_OK) {
    sensor_t *s = esp_camera_sensor_get();
    if (s) s->set_vflip(s, 1);
    Serial.println("Camera OK");
  }

  // Pre-bias timing to match Untitled2 behavior on kickstart
  lastGpsCycle = millis() - GPS_CYCLE_MS + 5000;

  stateTrackingMillis = millis();
  displayTelemetry();
}

// ══════════════════════════════════════════
//  LOOP ENGINE (STATE MACHINE)
// ══════════════════════════════════════════
void loop() {
  if (!modemReady && millis() - lastModemRetry >= MODEM_RETRY_INTERVAL_MS) {
    lastModemRetry = millis();
    modemReady = bringUpModem();
    if (modemReady) ensureCellularConnected();
  }

  bool wakeState = digitalRead(PIN_BUTTON_WAKE);
  bool camState = digitalRead(PIN_BUTTON_CAM);

  // 1. Handling Button 1: Wake / Sleep Engine
  if (lastWakeBtnState == HIGH && wakeState == LOW) {
    unsigned long pressTime = millis();
    if (currentMode == MODE_SLEEP) {
      wakeUpPeripherals();
    } else if (pressTime - lastWakeReleaseTime < 350) {
      enterDeepSleepMode();
    }
  }
  if (lastWakeBtnState == LOW && wakeState == HIGH) {
    lastWakeReleaseTime = millis();
  }

  // Global Auto-Sleep Checker
  if (currentMode != MODE_SLEEP) {
    if (millis() - stateTrackingMillis >= 30000) {
      if (gpsState == GPS_ACQUIRING) {
        Serial.println("GNSS is acquiring coordinates. Delaying auto-sleep...");
        stateTrackingMillis = millis();
      } else {
        enterDeepSleepMode();
      }
    }
  }

  // 2. Handling Camera Operation Modes
  if (currentMode == MODE_PREVIEW) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      gfx->draw16bitBeRGBBitmap(0, 40, (uint16_t *)fb->buf, fb->width, fb->height);
      esp_camera_fb_return(fb);
    }

    if (lastCamBtnState == HIGH && camState == LOW) {
      stateTrackingMillis = millis();
      camera_fb_t *fbCapture = esp_camera_fb_get();
      if (fbCapture) {
        capturedLength = fbCapture->len;
        capturedWidth  = fbCapture->width;
        capturedHeight = fbCapture->height;
        if (capturedBuffer != NULL) free(capturedBuffer);
        capturedBuffer = (uint8_t *)malloc(capturedLength);
        if (capturedBuffer) {
          memcpy(capturedBuffer, fbCapture->buf, capturedLength);
          currentMode = MODE_VERIFY;
          buttonCamPressTime = 0;
        }
        esp_camera_fb_return(fbCapture);
      }
    }
  }

  else if (currentMode == MODE_VERIFY) {
    int timeElapsed = (millis() - stateTrackingMillis) / 1000;
    int timeLeft = 20 - timeElapsed;

    if (timeLeft <= 0) {
      uploadImageToServer();
    } else if (timeElapsed >= 5) {
      // First 5s after capture: just leave the captured photo on screen, no banner yet
      displayUploadCountdown(timeLeft);
    }

    if (camState == LOW) {
      if (buttonCamPressTime == 0) buttonCamPressTime = millis();
      if (millis() - buttonCamPressTime >= 4000) {
        discardCapturedImage();
        buttonCamPressTime = 0;
      }
    } else {
      if (buttonCamPressTime > 0 && (millis() - buttonCamPressTime < 350)) {
        uploadImageToServer();
      }
      buttonCamPressTime = 0;
    }
  }

  else if (currentMode == MODE_AWAKE) {
    if (lastCamBtnState == HIGH && camState == LOW) {
      stateTrackingMillis = millis();
      startCameraPreview();
    }
  }

  lastWakeBtnState = wakeState;
  lastCamBtnState = camState;

  // 3. GNSS Execution Block copied EXACTLY from Untitled2.txt
  if (gpsState == GPS_IDLE && millis() - lastGpsCycle >= GPS_CYCLE_MS) {
    lastGpsCycle = millis();
    Serial.println("Enabling GNSS engine...");
    modem.enableGPS();
    gpsState = GPS_ACQUIRING;
    gpsAcquireStart = millis();
    lastGpsPoll = 0;
  }
  pollGps();
}

// ══════════════════════════════════════════
//  GPS MONITORING LOOP — Ported EXACTLY from Untitled2.txt
// ══════════════════════════════════════════
void pollGps() {
  if (gpsState != GPS_ACQUIRING) return;
  if (millis() - lastGpsPoll < GPS_POLL_INTERVAL_MS) return;
  lastGpsPoll = millis();

  float lat, lon, speed, alt, accuracy;
  int vsat, usat, year, month, day, hour, minute, second;

  if (modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat, &accuracy,
                    &year, &month, &day, &hour, &minute, &second)) {
    modem.disableGPS();
    gpsState = GPS_IDLE;
    Serial.printf("Fix acquired: %.6f, %.6f\n", lat, lon);
    lastLat = lat; lastLng = lon; hasFix = true; lastFixMillis = millis();

    if (ensureCellularConnected()) {
      bool ok = pushToSheet(lat, lon);
      Serial.println(ok ? "Sheet updated" : "Sheet update failed");
    } else {
      Serial.println("No cellular data — skipping sheet push this cycle");
    }
    if (currentMode == MODE_AWAKE) displayTelemetry();
    return;
  }

  if (millis() - gpsAcquireStart >= GPS_TIMEOUT_MS) {
    modem.disableGPS();
    gpsState = GPS_IDLE;
    Serial.println("No GPS fix within 2 min window — pushing 0.000,0.000 to sheet");

    if (ensureCellularConnected()) {
      bool ok = pushToSheet(0.0f, 0.0f);
      Serial.println(ok ? "Placeholder pushed" : "Placeholder push failed");
    } else {
      Serial.println("No cellular data — skipping placeholder push this cycle");
    }

    if (currentMode == MODE_AWAKE) displayTelemetry();
    return;
  }

  Serial.println("Waiting for GNSS fix... (needs open sky view)");
}

// ══════════════════════════════════════════
//  HARDWARE & POWER MANAGEMENT INTERRUPTS
// ══════════════════════════════════════════
void enterDeepSleepMode() {
  Serial.println("Powering down display and putting chip components to sleep.");
  digitalWrite(LCD_BL, LOW);
  gfx->fillScreen(BLACK);
  currentMode = MODE_SLEEP;
}

void wakeUpPeripherals() {
  digitalWrite(LCD_BL, HIGH);
  displayWakeFromSleepScreen();
  currentMode = MODE_AWAKE;
  stateTrackingMillis = millis();
  displayTelemetry();
}

// ══════════════════════════════════════════
//  UI LAYOUT INTERFACES
// ══════════════════════════════════════════
void displayFirstPowerOnSplash() {
  digitalWrite(LCD_BL, HIGH);
  gfx->fillScreen(WHITE);
  gfx->setTextSize(2);
  gfx->setTextColor(BLACK);
  gfx->setCursor(15, 80);
  gfx->println("   MapTrack Unit");
  gfx->setTextColor(BLUE);
  gfx->setTextSize(1);
  gfx->setCursor(15, 140);
  gfx->println("      Powered by:");
  gfx->setTextColor(GREEN);
  gfx->setTextSize(2);
  gfx->setCursor(15, 170);
  gfx->println("  PNT Robotics &");
  gfx->setCursor(15, 200);
  gfx->println("    Automation");
  delay(5000);
}

void displayWakeFromSleepScreen() {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE);
  gfx->setCursor(40, 140);
  gfx->println("powering on....");
  delay(1200);
}

void displayTelemetry() {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE);
  gfx->setCursor(10, 20);
  gfx->println("=== System Metrics ===");
  gfx->printf("\n \n ID: %s \n \n", deviceName);
  gfx->printf("Modem Status:%s\n", modemReady ? "ONLINE" : "OFFLINE");
  if (hasFix) {
    gfx->setTextColor(GREEN);
    gfx->printf("Lat: %.5f \nLng: %.5f\n", lastLat, lastLng);
  } else {
    gfx->setTextColor(RED);
    gfx->println("\n \n GNSS Fix: SEARCHING...");
  }
}

void displayUploadCountdown(int secondsLeft) {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE);
  gfx->setCursor(40, 140);
  gfx->printf("Uploading in \n  \n    %ds  \n \n  Hold to DEL \n \n Single press to Upload", secondsLeft);
}

void startCameraPreview() {
  gfx->fillScreen(BLACK);
  currentMode = MODE_PREVIEW;
}

void discardCapturedImage() {
  if (capturedBuffer) { free(capturedBuffer); capturedBuffer = NULL; }
  gfx->fillScreen(BLACK);
  gfx->setTextColor(RED);
  gfx->setTextSize(2);
  gfx->setCursor(40, 140);
  gfx->println("IMAGE DELETED");
  delay(1500);
  startCameraPreview();
}

void uploadImageToServer() {
  gfx->fillScreen(ORANGE);
  gfx->setTextColor(BLACK);
  gfx->setTextSize(2);
  gfx->setCursor(20, 140);
  gfx->println("    PROCESSING \n \n      UPLOAD...");

  uint8_t *jpg_buf = NULL;
  size_t jpg_len = 0;
  if (fmt2jpg(capturedBuffer, capturedLength, capturedWidth, capturedHeight, PIXFORMAT_RGB565, 80, &jpg_buf, &jpg_len)) {
    bool ok = sendImageToGallery(jpg_buf, jpg_len, hasFix, lastLat, lastLng);
    gfx->fillScreen(ok ? GREEN : RED);
    gfx->setTextColor(ok ? BLACK : WHITE);
    gfx->setCursor(30, 140);
    gfx->println(ok ? "     UPLOAD \n \n     SUCCESSFUL!" : "UPLOAD \n \n      FAILED");
    free(jpg_buf);
  }
  delay(2000);
  if (capturedBuffer) { free(capturedBuffer); capturedBuffer = NULL; }
  currentMode = MODE_AWAKE;
  stateTrackingMillis = millis();
  displayTelemetry();
}

// ══════════════════════════════════════════
//  MODEM DRIVERS AND TELEMETRY SUBSTRATES
// ══════════════════════════════════════════
bool bringUpModem() {
  digitalWrite(MODEM_PWRKEY, HIGH); delay(200);
  digitalWrite(MODEM_PWRKEY, LOW);  delay(1500);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(5000);
  if (modem.testAT(10000)) { modem.init(); return true; }
  return false;
}

bool ensureCellularConnected() {
  if (!modemReady) return false;
  if (modem.isNetworkConnected() && modem.isGprsConnected()) return true;
  if (!modem.waitForNetwork(30000L)) return false;
  return modem.gprsConnect(apn, apnUser, apnPass);
}

// Safely joins a host + path without producing a double slash, even if
// the host constant accidentally has a trailing "/" (this is what broke
// the gallery upload URL last time).
String joinUrl(const String &host, const String &path) {
  String h = host;
  while (h.endsWith("/")) h.remove(h.length() - 1);
  String p = path;
  if (!p.startsWith("/")) p = "/" + p;
  return h + p;
}

bool pushToSheet(float lat, float lon) {
  if (!modemReady) return false;
  StaticJsonDocument<512> doc;
  doc["action"] = "append";
  doc["rowId"] = String(deviceIdPrefix) + "-" + String(millis());
  doc["name"] = deviceName; doc["lat"] = lat; doc["lng"] = lon;
  doc["datetime"] = getTimestamp(); doc["category"] = deviceCategory;
  String payload; serializeJson(doc, payload);

  String url = String("https://") + joinUrl(scriptHost, scriptPath);
  String resp;
  atSend("AT+HTTPTERM"); atWaitFor("OK", 2000);
  atSend("AT+HTTPINIT"); if (!atWaitFor("OK", 5000, &resp)) return false;
  atSend("AT+HTTPPARA=\"CID\",1"); atWaitFor("OK", 3000);
  atSend("AT+CSSLCFG=\"sslversion\",0,4"); atWaitFor("OK", 3000);
  atSend("AT+CSSLCFG=\"enableSNI\",0,1"); atWaitFor("OK", 3000);
  atSend("AT+HTTPSSL=1"); atWaitFor("OK", 3000);
  atSend("AT+HTTPPARA=\"URL\",\"" + url + "\""); if (!atWaitFor("OK", 5000)) return false;
  atSend("AT+HTTPPARA=\"CONTENT\",\"application/json\""); atWaitFor("OK", 3000);
  atSend("AT+HTTPDATA=" + String(payload.length()) + ",20000"); if (!atWaitFor("DOWNLOAD", 5000)) return false;
  SerialAT.print(payload); if (!atWaitFor("OK", 15000)) return false;
  atSend("AT+HTTPACTION=1"); if (!atWaitFor("+HTTPACTION: 1,", 20000, &resp)) return false;

  int idx = resp.indexOf("+HTTPACTION: 1,");
  int status = resp.substring(idx + 15, resp.indexOf(',', idx + 15)).toInt();
  atSend("AT+HTTPTERM"); atWaitFor("OK", 2000);
  return (status == 200 || status == 302);
}

bool sendImageToGallery(uint8_t *buf, size_t len, bool haveFix, float lat, float lng) {
  if (!ensureCellularConnected()) return false;
  String url = String("https://") + joinUrl(GALLERY_HOST, GALLERY_PATH) + "?device=" + urlEncode(deviceName);
  if (haveFix) url += "&lat=" + String(lat, 6) + "&lng=" + String(lng, 6);

  Serial.print("[Gallery] Setting URL: "); Serial.println(url);

  String resp;
  atSend("AT+HTTPTERM"); atWaitFor("OK", 2000);
  atSend("AT+HTTPINIT"); if (!atWaitFor("OK", 5000, &resp)) return false;
  atSend("AT+HTTPPARA=\"CID\",1"); atWaitFor("OK", 3000);
  atSend("AT+CSSLCFG=\"sslversion\",0,4"); atWaitFor("OK", 3000);
  atSend("AT+CSSLCFG=\"enableSNI\",0,1"); atWaitFor("OK", 3000);
  atSend("AT+HTTPSSL=1"); atWaitFor("OK", 3000);
  atSend("AT+HTTPPARA=\"URL\",\"" + url + "\""); if (!atWaitFor("OK", 5000)) return false;
  atSend("AT+HTTPPARA=\"CONTENT\",\"image/jpeg\""); atWaitFor("OK", 3000);
  atSend("AT+HTTPDATA=" + String(len) + ",20000"); if (!atWaitFor("DOWNLOAD", 5000)) return false;
  Serial.printf("[Gallery] Sending %d bytes...\n", len);
  SerialAT.write(buf, len); if (!atWaitFor("OK", 15000)) return false;
  Serial.println("[Gallery] Firing HTTPACTION...");
  atSend("AT+HTTPACTION=1"); if (!atWaitFor("+HTTPACTION: 1,", 20000, &resp)) return false;

  int idx = resp.indexOf("+HTTPACTION: 1,");
  int status = resp.substring(idx + 15, resp.indexOf(',', idx + 15)).toInt();
  Serial.printf("[Gallery] HTTP status: %d\n", status);
  atSend("AT+HTTPTERM"); atWaitFor("OK", 2000);
  return (status == 200);
}

bool atWaitFor(const char *target, unsigned long timeoutMs, String *out) {
  String buf; unsigned long start = millis(); bool found = false;
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      buf += (char)SerialAT.read();
      if (!found && buf.indexOf(target) != -1) {
        found = true; unsigned long grace = millis() + 400;
        while (millis() < grace) {
          while (SerialAT.available()) { buf += (char)SerialAT.read(); grace = millis() + 100; }
        }
        if (out) *out = buf; return true;
      }
    }
  }
  if (out) *out = buf; return false;
}

void atSend(const String &cmd) { SerialAT.print(cmd + "\r\n"); }

String urlEncode(const char *s) {
  String out;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (isalnum((unsigned char)c) || c=='-' || c=='_' || c=='.' || c=='~') out += c;
    else if (c==' ') out += "%20";
    else { char buf[4]; sprintf(buf, "%%%02X", (unsigned char)c); out += buf; }
  }
  return out;
}

String getTimestamp() {
  int year, month, day, hour, minute, second; float tz;
  if (modem.getNetworkTime(&year, &month, &day, &hour, &minute, &second, &tz)) {
    char buf[24]; snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d", year, month, day, hour, minute);
    return String(buf);
  }
  return "uptime-" + String(millis() / 1000) + "s";
}
