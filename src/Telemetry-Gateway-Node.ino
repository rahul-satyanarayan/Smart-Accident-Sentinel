#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <DHT.h>
#include <Preferences.h>
#include <math.h>

// =================================================================
// 1. HARDWARE PIN DEFINITIONS
// =================================================================
#define SDA_PIN                 8       // I2C SDA for SH1106 OLED
#define SCL_PIN                 9       // I2C SCL for SH1106 OLED
#define DHT_PIN                 3       // DHT22 Data Pin
#define BUZZER_PIN              5       // Active Buzzer Pin
#define TOUCH_PIN               7       // TTP223 Touch Sensor (Active-HIGH)
#define I2C_OLED_ADDR           0x3C    // Hardware OLED I2C Address

// =================================================================
// 2. TIMING CONSTANTS
// =================================================================
#define EMERGENCY_HOLD_ACK_MS   3000    // Hold 3s to acknowledge emergency
#define ALARM_CONFIG_HOLD_MS    4000    // Hold 4s during idle to set alarm
#define CONFIG_IDLE_TIMEOUT_MS  3000    // 3s inactivity auto-advances/saves alarm
#define SCREEN_STEP_TIME_MS     5000    // 5s duration per screen

// =================================================================
// 3. CREDENTIALS CONFIGURATION
// =================================================================
const char* WIFI_SSID           = "ENTER YOUR SSID"; //ENTER YOUR SSID
const char* WIFI_PASS           = "ENTER YOUR PASSWORD"; //ENTER YOUR PASSWORD

const char* BOT_TOKEN           = "ENTER YOUR BOT_TOKEN"; //ENTER YOUR BOT_TOKEN
const char* CHAT_ID             = "ENTER YOUR CHAT_ID"; //ENTER YOUR CHAT_ID

const char* AIO_USERNAME        = "ENTER YOUR AIO_USERNAME"; //ENTER YOUR AIO_USERNAME
const char* AIO_KEY             = "ENTER YOUR AIO KEY"; //ENTER YOUR AIO KEY
const char* AIO_FEED            = "ENTER YOUR AIO_FEED"; //ENTER YOUR AIO_FEED

// IST (UTC +5:30 = 19800 seconds)
const char* NTP_SERVER          = "pool.ntp.org";
const long  GMT_OFFSET_SEC      = 19800;
const int   DAYLIGHT_OFFSET_SEC = 0;

// =================================================================
// 4. OBJECTS & APPLICATION STATE ENGINE
// =================================================================
Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &Wire, -1);
DHT dht(DHT_PIN, DHT22);
Preferences prefs;

enum AppState { 
  STATE_FACE_ANIMATION, 
  STATE_DATA_OVERLAY, 
  STATE_ALARM_CONFIG, 
  STATE_EMERGENCY_ACTIVE,
  STATE_ACK_DISPATCHED
};
AppState currentState = STATE_FACE_ANIMATION;

enum ConfigStep { SET_HOURS, SET_MINUTES };
ConfigStep configStep = SET_HOURS;

// System Metrics & Flags
bool isEmergencyTriggered       = false;
bool isAlarmRinging             = false;
float currentTemp               = 0.0f;
float currentHumidity           = 0.0f;
int alarmHour                   = 7;
int alarmMinute                 = 0;

// Input & Timing Variables
unsigned long touchStartTimestamp  = 0;
bool lastTouchState                = LOW;
unsigned long lastConfigInputTime  = 0;
unsigned long ackConfirmationTimer = 0;
unsigned long lastSensorReadTime   = 0;
unsigned long lastCloudPoll        = 0;
long lastTelegramUpdateId          = 0;

const char* daysOfWeek[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// =================================================================
// 5. CLOUD & TELEGRAM API ROUTINES
// =================================================================
void setAdafruitFeedValue(const char* val) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(2500);

  HTTPClient https;
  String url = "https://io.adafruit.com/api/v2/" + String(AIO_USERNAME) + "/feeds/" + String(AIO_FEED) + "/data";
  if (https.begin(client, url)) {
    https.addHeader("X-AIO-Key", AIO_KEY);
    https.addHeader("Content-Type", "application/json");
    https.POST("{\"value\":\"" + String(val) + "\"}");
    https.end();
  }
  client.stop();
}

void sendTelegramResetMsg() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000);

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage";
  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    https.POST("chat_id=" + String(CHAT_ID) + "&text=/reset");
    https.end();
  }
  client.stop();
}

void pollCloudAndTelegram() {
  // Pause network polling while editing alarm to prevent UI lag
  if (currentState == STATE_ALARM_CONFIG) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastCloudPoll < 2000) return;
  lastCloudPoll = millis();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3000);

  // 1. Poll Adafruit IO Feed
  HTTPClient httpAIO;
  String aioUrl = "https://io.adafruit.com/api/v2/" + String(AIO_USERNAME) + "/feeds/" + String(AIO_FEED) + "/data/last";
  if (httpAIO.begin(client, aioUrl)) {
    httpAIO.addHeader("X-AIO-Key", AIO_KEY);
    int code = httpAIO.GET();
    if (code == 200) {
      String payload = httpAIO.getString();
      int valPos = payload.indexOf("\"value\":\"");
      if (valPos != -1) {
        char val = payload.charAt(valPos + 9);
        if (val == '1' && !isEmergencyTriggered) {
          isEmergencyTriggered = true;
          currentState = STATE_EMERGENCY_ACTIVE;
        } else if (val == '0' && isEmergencyTriggered) {
          isEmergencyTriggered = false;
          currentState = STATE_FACE_ANIMATION;
        }
      }
    }
    httpAIO.end();
  }

  // 2. Poll Telegram for /reset
  HTTPClient httpTG;
  String tgUrl = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/getUpdates";
  if (lastTelegramUpdateId > 0) tgUrl += "?offset=" + String(lastTelegramUpdateId + 1);

  if (httpTG.begin(client, tgUrl)) {
    int code = httpTG.GET();
    if (code == 200) {
      String resp = httpTG.getString();
      if (resp.indexOf("\"update_id\":") != -1) {
        int idPos = resp.lastIndexOf("\"update_id\":");
        int commaPos = resp.indexOf(",", idPos);
        lastTelegramUpdateId = resp.substring(idPos + 12, commaPos).toInt();

        resp.toLowerCase();
        if (resp.indexOf("reset") != -1) {
          if (isEmergencyTriggered) {
            isEmergencyTriggered = false;
            currentState = STATE_FACE_ANIMATION;
            setAdafruitFeedValue("0");
          }
        }
      }
    }
    httpTG.end();
  }
  client.stop();
}

// =================================================================
// 6. TOUCH PROCESSING ENGINE (FIXED & HIGH-RESPONSIVE)
// =================================================================
void processTouch() {
  bool currentTouch = digitalRead(TOUCH_PIN);

  // --- CASE A: ALARM CONFIGURATION MODE (TAPS ONLY) ---
  if (currentState == STATE_ALARM_CONFIG) {
    // Detect single press on rising edge (Finger Just Touched)
    if (lastTouchState == LOW && currentTouch == HIGH) {
      if (configStep == SET_HOURS) {
        alarmHour = (alarmHour + 1) % 24;
      } else {
        alarmMinute = (alarmMinute + 1) % 60;
      }
      lastConfigInputTime = millis(); // Reset inactivity timer

      // Quick audio click for tactile confirmation
      digitalWrite(BUZZER_PIN, HIGH);
      delay(25);
      digitalWrite(BUZZER_PIN, LOW);
    }
    lastTouchState = currentTouch;

    // Inactivity timeout: Step through Hours -> Minutes -> Save
    if (millis() - lastConfigInputTime >= CONFIG_IDLE_TIMEOUT_MS) {
      if (configStep == SET_HOURS) {
        configStep = SET_MINUTES;
        lastConfigInputTime = millis();
        // Double beep on step switch
        digitalWrite(BUZZER_PIN, HIGH); delay(40); digitalWrite(BUZZER_PIN, LOW);
      } else {
        prefs.putInt("alH", alarmHour);
        prefs.putInt("alM", alarmMinute);
        currentState = STATE_FACE_ANIMATION;
        // Long beep on save
        digitalWrite(BUZZER_PIN, HIGH); delay(120); digitalWrite(BUZZER_PIN, LOW);
      }
    }
    return;
  }

  // --- CASE B: EMERGENCY OR IDLE MODES (HOLDS & ALARM MUTE) ---
  if (lastTouchState == LOW && currentTouch == HIGH) {
    touchStartTimestamp = millis();
    // Mute scheduled morning alarm instantly
    if (isAlarmRinging) {
      isAlarmRinging = false;
      digitalWrite(BUZZER_PIN, LOW);
    }
  }

  if (currentTouch == HIGH) {
    unsigned long heldMs = millis() - touchStartTimestamp;

    // 1. EMERGENCY ACK HOLD (3 Seconds)
    if (isEmergencyTriggered && (heldMs >= EMERGENCY_HOLD_ACK_MS)) {
      isEmergencyTriggered = false;
      digitalWrite(BUZZER_PIN, LOW);

      // Confirmation beep
      digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW);

      currentState = STATE_ACK_DISPATCHED;
      ackConfirmationTimer = millis();

      setAdafruitFeedValue("0");
      sendTelegramResetMsg();

      touchStartTimestamp = millis() + 5000; // Debounce until release
    }

    // 2. ENTER ALARM SETUP HOLD (4 Seconds in Idle)
    if (!isEmergencyTriggered && currentState != STATE_ACK_DISPATCHED && (heldMs >= ALARM_CONFIG_HOLD_MS)) {
      currentState = STATE_ALARM_CONFIG;
      configStep = SET_HOURS;
      lastConfigInputTime = millis();

      // Chirp feedback
      digitalWrite(BUZZER_PIN, HIGH); delay(70); digitalWrite(BUZZER_PIN, LOW);

      // Wait until finger is released before registering edits
      while (digitalRead(TOUCH_PIN) == HIGH) delay(10);
      lastTouchState = LOW;
      return;
    }
  }

  lastTouchState = currentTouch;
}

// =================================================================
// 7. GRAPHICS & DISPLAY ROUTINES
// =================================================================
void drawThermometer(int x, int y) {
  display.drawRoundRect(x + 2, y, 4, 10, 2, SH110X_WHITE);
  display.fillCircle(x + 4, y + 10, 3, SH110X_WHITE);
  display.drawLine(x + 4, y + 3, x + 4, y + 8, SH110X_BLACK);
}

void drawDroplet(int x, int y) {
  display.fillCircle(x, y, 4, SH110X_WHITE);
  display.fillTriangle(x - 4, y, x + 4, y, x, y - 8, SH110X_WHITE);
}

void drawCalendarIcon(int x, int y) {
  display.drawRect(x, y, 14, 12, SH110X_WHITE);
  display.drawLine(x, y + 3, x + 14, y + 3, SH110X_WHITE);
  display.drawPixel(x + 3, y - 1, SH110X_WHITE);
  display.drawPixel(x + 11, y - 1, SH110X_WHITE);
}

void drawAnalogClock(int h, int m, int s) {
  int cx = 64, cy = 32, r = 26;
  display.drawCircle(cx, cy, r, SH110X_WHITE);
  float mAngle = (m * 6.0f) * (M_PI / 180.0f) - (M_PI / 2.0f);
  display.drawLine(cx, cy, cx + cos(mAngle) * 20, cy + sin(mAngle) * 20, SH110X_WHITE);
  float hAngle = (h * 30.0f + m / 2.0f) * (M_PI / 180.0f) - (M_PI / 2.0f);
  display.drawLine(cx, cy, cx + cos(hAngle) * 14, cy + sin(hAngle) * 14, SH110X_WHITE);
}

void renderBuddyFace(int emotionIndex) {
  int lx = 20, rx = 76, ey = 10;
  switch (emotionIndex) {
    case 0: // Idle
      display.fillRoundRect(lx, ey, 32, 40, 10, SH110X_WHITE);
      display.fillRoundRect(rx, ey, 32, 40, 10, SH110X_WHITE);
      display.fillCircle(lx + 16, ey + 20, 7, SH110X_BLACK);
      display.fillCircle(rx + 16, ey + 20, 7, SH110X_BLACK);
      display.drawCircle(64, 48, 6, SH110X_WHITE);
      display.fillRect(56, 42, 16, 6, SH110X_BLACK);
      break;
    case 1: // Happy
      for (int i = 0; i < 4; i++) {
        display.drawLine(lx + 4, ey + 28 - i, lx + 16, ey + 12 - i, SH110X_WHITE);
        display.drawLine(lx + 16, ey + 12 - i, lx + 28, ey + 28 - i, SH110X_WHITE);
        display.drawLine(rx + 4, ey + 28 - i, rx + 16, ey + 12 - i, SH110X_WHITE);
        display.drawLine(rx + 16, ey + 12 - i, rx + 28, ey + 28 - i, SH110X_WHITE);
      }
      display.fillCircle(64, 46, 7, SH110X_WHITE);
      display.fillRect(54, 38, 20, 8, SH110X_BLACK);
      break;
    case 2: // Curious
      display.fillRoundRect(lx, ey, 32, 40, 10, SH110X_WHITE);
      display.fillCircle(lx + 16, ey + 18, 7, SH110X_BLACK);
      display.fillRoundRect(rx, ey + 10, 32, 28, 6, SH110X_WHITE);
      display.fillCircle(rx + 16, ey + 24, 5, SH110X_BLACK);
      display.drawLine(58, 48, 70, 44, SH110X_WHITE);
      break;
    case 3: // Wink
      display.fillRoundRect(lx, ey, 32, 40, 10, SH110X_WHITE);
      display.fillCircle(lx + 16, ey + 20, 7, SH110X_BLACK);
      for (int i = 0; i < 4; i++) {
        display.drawLine(rx, ey + 14 + i, rx + 24, ey + 24 + i, SH110X_WHITE);
        display.drawLine(rx, ey + 34 - i, rx + 24, ey + 24 - i, SH110X_WHITE);
      }
      display.drawCircle(64, 46, 8, SH110X_WHITE);
      display.fillRect(54, 36, 20, 10, SH110X_BLACK);
      break;
    case 4: // Sleepy
      display.fillRoundRect(lx, ey + 14, 32, 20, 6, SH110X_WHITE);
      display.fillRoundRect(rx, ey + 14, 32, 20, 6, SH110X_WHITE);
      display.fillCircle(lx + 16, ey + 25, 4, SH110X_BLACK);
      display.fillCircle(rx + 16, ey + 25, 4, SH110X_BLACK);
      display.drawCircle(64, 48, 5, SH110X_WHITE);
      break;
    case 5: // Heart Eyes
      for (int s = 0; s < 2; s++) {
        int cx = (s == 0) ? lx + 16 : rx + 16;
        int cy = ey + 18;
        display.fillCircle(cx - 6, cy - 4, 7, SH110X_WHITE);
        display.fillCircle(cx + 6, cy - 4, 7, SH110X_WHITE);
        display.fillTriangle(cx - 13, cy - 2, cx + 13, cy - 2, cx, cy + 14, SH110X_WHITE);
      }
      display.drawCircle(64, 48, 7, SH110X_WHITE);
      display.fillRect(54, 40, 20, 8, SH110X_BLACK);
      break;
  }
}

void renderDataScreen(int screenIndex, struct tm* timeinfo) {
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);

  switch (screenIndex) {
    case 0: // Digital Clock
      display.setCursor(15, 25);
      display.printf("%02d:%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
      break;
    case 1: // Date & Day
      drawCalendarIcon(12, 25);
      display.setCursor(34, 15);
      display.print(daysOfWeek[timeinfo->tm_wday]);
      display.setCursor(34, 40);
      display.printf("%02d/%02d", timeinfo->tm_mday, timeinfo->tm_mon + 1);
      break;
    case 2: // Temperature
      drawThermometer(20, 20);
      display.setCursor(45, 25);
      display.printf("%.1f C", currentTemp);
      break;
    case 3: // Humidity
      drawDroplet(25, 35);
      display.setCursor(45, 25);
      display.printf("%.0f %%", currentHumidity);
      break;
    case 4: // Analog Clock
      drawAnalogClock(timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
      break;
  }
}

void renderEmergencyScreen() {
  bool flashInverted = ((millis() / 500) % 2) == 0;

  if (flashInverted) {
    display.fillRect(0, 0, 128, 64, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK, SH110X_WHITE);
  } else {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
  }

  display.setTextSize(1);
  display.setCursor(14, 2);
  display.print("*** CRITICAL! ***");
  display.setCursor(10, 16);
  display.print("ACCIDENT DETECTED");
  display.setCursor(8, 28);
  display.print("Check Telegram App");

  // Visual Solid Hold Progress Bar (3 Seconds)
  if (digitalRead(TOUCH_PIN) == HIGH) {
    unsigned long heldMs = millis() - touchStartTimestamp;
    int progressWidth = map(constrain(heldMs, 0, EMERGENCY_HOLD_ACK_MS), 0, EMERGENCY_HOLD_ACK_MS, 0, 120);

    display.setCursor(16, 42);
    display.print("HOLD TO DISMISS...");

    if (flashInverted) {
      display.drawRect(4, 52, 120, 8, SH110X_BLACK);
      display.fillRect(4, 52, progressWidth, 8, SH110X_BLACK);
    } else {
      display.drawRect(4, 52, 120, 8, SH110X_WHITE);
      display.fillRect(4, 52, progressWidth, 8, SH110X_WHITE);
    }
  } else {
    display.setCursor(4, 46);
    display.print("[Hold Touch 3s ACK]");
  }
}

void renderAlarmConfigScreen() {
  bool digitBlink = ((millis() / 350) % 2) == 0;

  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(14, 4);
  display.print(configStep == SET_HOURS ? "EDIT ALARM: HOURS" : "EDIT ALARM: MINS");

  display.setTextSize(3);
  display.setCursor(20, 20);

  if (configStep == SET_HOURS && !digitBlink) {
    display.print("  ");
  } else {
    display.printf("%02d", alarmHour);
  }

  display.print(":");

  if (configStep == SET_MINUTES && !digitBlink) {
    display.print("  ");
  } else {
    display.printf("%02d", alarmMinute);
  }

  // Timeout remaining bar
  unsigned long timeSinceLastTap = millis() - lastConfigInputTime;
  int remainingWidth = map(constrain(CONFIG_IDLE_TIMEOUT_MS - timeSinceLastTap, 0, CONFIG_IDLE_TIMEOUT_MS), 0, CONFIG_IDLE_TIMEOUT_MS, 0, 120);

  display.setTextSize(1);
  display.setCursor(16, 46);
  display.print("Tap to change");
  display.drawRect(4, 58, 120, 4, SH110X_WHITE);
  display.fillRect(4, 58, remainingWidth, 4, SH110X_WHITE);
}

void updateBuzzer() {
  if (isEmergencyTriggered) {
    digitalWrite(BUZZER_PIN, ((millis() / 100) % 2));
  } else if (isAlarmRinging) {
    digitalWrite(BUZZER_PIN, HIGH);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// =================================================================
// 8. SETUP
// =================================================================
void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(TOUCH_PIN, INPUT);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  display.begin(I2C_OLED_ADDR, true);
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(15, 20);
  display.print("Desk Buddy Node 2");
  display.setCursor(15, 36);
  display.print("Connecting WiFi...");
  display.display();

  dht.begin();
  prefs.begin("desk_buddy", false);
  alarmHour   = prefs.getInt("alH", 7);
  alarmMinute = prefs.getInt("alM", 0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(400);

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "time.google.com");
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 1700000000 && retries < 25) {
    delay(400);
    now = time(nullptr);
    retries++;
  }

  display.clearDisplay();
  display.setCursor(20, 24);
  display.print("WiFi Connected!");
  display.setCursor(16, 40);
  display.print("System Armed!");
  display.display();
  delay(1200);
}

// =================================================================
// 9. MAIN LOOP
// =================================================================
void loop() {
  processTouch();            // Immediate priority touch handling
  updateBuzzer();            
  pollCloudAndTelegram();    // Sync Adafruit IO & Telegram

  struct tm timeinfo;
  bool isTimeValid = getLocalTime(&timeinfo, 10);

  if (isTimeValid && !isEmergencyTriggered) {
    if (timeinfo.tm_hour == alarmHour && timeinfo.tm_min == alarmMinute && timeinfo.tm_sec == 0) {
      isAlarmRinging = true;
    }
  }

  if (millis() - lastSensorReadTime >= 2500) {
    lastSensorReadTime = millis();
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) currentTemp = t;
    if (!isnan(h)) currentHumidity = h;
  }

  display.clearDisplay();

  if (currentState == STATE_EMERGENCY_ACTIVE || isEmergencyTriggered) {
    renderEmergencyScreen();
  }
  else if (currentState == STATE_ACK_DISPATCHED) {
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(18, 14);
    display.print("HELP DISPATCHED!");
    display.setCursor(14, 28);
    display.print("ACK SENT TO BIKE");
    display.setCursor(20, 42);
    display.print("Buzzer Muted.");
    display.drawRect(4, 54, 120, 6, SH110X_WHITE);
    display.fillRect(4, 54, 120, 6, SH110X_WHITE);

    if (millis() - ackConfirmationTimer >= 3500) {
      currentState = STATE_FACE_ANIMATION;
    }
  }
  else if (currentState == STATE_ALARM_CONFIG) {
    renderAlarmConfigScreen();
  }
  else {
    const unsigned long totalFaceCycle = 6 * SCREEN_STEP_TIME_MS; // 30s
    const unsigned long totalDataCycle = 5 * SCREEN_STEP_TIME_MS; // 25s
    const unsigned long totalPeriod    = totalFaceCycle + totalDataCycle;

    unsigned long cycleTime = millis() % totalPeriod;

    // Show hold progress bar when holding touch in idle
    if (digitalRead(TOUCH_PIN) == HIGH) {
      unsigned long holdMs = millis() - touchStartTimestamp;
      int pBar = map(constrain(holdMs, 0, ALARM_CONFIG_HOLD_MS), 0, ALARM_CONFIG_HOLD_MS, 0, 120);

      display.setTextSize(1);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(8, 20);
      display.print("HOLD TO SET ALARM");
      display.drawRect(4, 40, 120, 8, SH110X_WHITE);
      display.fillRect(4, 40, pBar, 8, SH110X_WHITE);
    } else {
      if (cycleTime < totalFaceCycle) {
        renderBuddyFace(cycleTime / SCREEN_STEP_TIME_MS);
      } else {
        if (isTimeValid) {
          renderDataScreen((cycleTime - totalFaceCycle) / SCREEN_STEP_TIME_MS, &timeinfo);
        }
      }
    }
  }

  display.display();
  delay(15);
}