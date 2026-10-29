#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <time.h>
#include <math.h>

// =================================================================
// HARDWARE PIN DEFINITIONS
// =================================================================
#define I2C_SDA_PIN             21
#define I2C_SCL_PIN             22
#define AP_FORCE_PIN            4       // Pushbutton to enter AP mode (Active-LOW)
#define STATUS_LED_PIN          2       // Onboard Status LED

// =================================================================
// SENSOR THRESHOLDS & TIMING CONSTANTS
// =================================================================
#define IMPACT_THRESHOLD_G      1.5f    // G-force threshold to trigger impact
#define QUIET_GYRO_THRESHOLD_DPS 60.0f  // Stillness verification threshold
#define INACTIVITY_WINDOW_MS    0       // Immobility verification duration (2s)
#define ALERT_COOLDOWN_MS       0       // Cooldown between triggers
#define ALERT_RETRY_INTERVAL_MS 30000   // 30s unacknowledged reminder interval
#define LCD_SCROLL_SPEED_MS     350     // Marquee scrolling speed

// =================================================================
// NETWORK, CLOUD & TELEGRAM CREDENTIALS
// =================================================================
const char* BOT_TOKEN           = "ENTER YOUR BOT_TOKEN"; //ENTER YOUR BOT_TOKEN
const char* CHAT_ID             = "ENTER YOUR CHAT_ID"; //ENTER YOUR CHAT_ID

// Adafruit IO Credentials
const char* AIO_USERNAME        = "ENTER YOUR AIO_USERNAME"; //ENTER YOUR AIO_USERNAME
const char* AIO_KEY             = "ENTER YOUR AIO KEY"; //ENTER YOUR AIO KEY
const char* AIO_FEED            = "ENTER YOUR AIO_FEED"; //ENTER YOUR AIO_FEED

// Indian Standard Time (IST: UTC + 5:30 = 19800 seconds)
const char* NTP_SERVER          = "pool.ntp.org";
const long  GMT_OFFSET_SEC      = 19800;
const int   DAYLIGHT_OFFSET_SEC = 0;

// =================================================================
// SYSTEM OBJECTS & STATE MACHINE
// =================================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_MPU6050 mpu;
Preferences prefs;
WebServer server(80);

enum SystemState {
  STATE_AP_MODE,
  STATE_ARMED,
  STATE_IMPACT_VERIFY,
  STATE_ALERT_LATCHED
};
SystemState currentState = STATE_ARMED;

// Network Ring Buffer
#define MAX_NETWORKS 5
String savedSSIDs[MAX_NETWORKS];
String savedPass[MAX_NETWORKS];
int networkCount = 0;

// Runtime Tracking Variables
unsigned long impactStartTime   = 0;
unsigned long lastAlertSentTime = 0;
unsigned long lastRetrySentTime = 0;
unsigned long lastScrollTime    = 0;
int scrollIndex                 = 0;
long lastUpdateId               = 0;
float capturedGForce            = 0.0f;

const String SCROLL_MSG = "EMERGENCY: ACCIDENT DETECTED! WAITING FOR GUARDIAN ACK...    ";

// Forward Declarations
void startAPMode();
bool attemptConnectionToKnownNetworks();
void updateLCD(const String& line1, const String& line2);

// =================================================================
// LCD DISPLAY HELPER
// =================================================================
void updateLCD(const String& line1, const String& line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

// =================================================================
// NVS STORAGE (WIFI CREDENTIALS)
// =================================================================
void loadSavedNetworks() {
  prefs.begin("wifi_creds", true);
  networkCount = prefs.getInt("count", 0);
  for (int i = 0; i < networkCount; i++) {
    savedSSIDs[i] = prefs.getString(("s" + String(i)).c_str(), "");
    savedPass[i]  = prefs.getString(("p" + String(i)).c_str(), "");
  }
  prefs.end();
}

void saveNewNetwork(const String& ssid, const String& pass) {
  prefs.begin("wifi_creds", false);
  for (int i = 0; i < networkCount; i++) {
    if (savedSSIDs[i] == ssid) {
      prefs.putString(("p" + String(i)).c_str(), pass);
      prefs.end();
      return;
    }
  }

  int idx = (networkCount < MAX_NETWORKS) ? networkCount : (MAX_NETWORKS - 1);
  prefs.putString(("s" + String(idx)).c_str(), ssid);
  prefs.putString(("p" + String(idx)).c_str(), pass);
  if (networkCount < MAX_NETWORKS) {
    networkCount++;
    prefs.putInt("count", networkCount);
  }
  prefs.end();
}

// =================================================================
// SOFTAP CAPTIVE CONFIGURATION PORTAL
// =================================================================
// =================================================================
// SOFTAP CAPTIVE CONFIGURATION PORTAL (PROFESSIONAL UI)
// =================================================================
void startAPMode() {
  currentState = STATE_AP_MODE;
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AccidentAlert-Setup", "deskBuddy_alert");

  updateLCD("AP:AccidentAlert", "IP:" + WiFi.softAPIP().toString());
  Serial.println("[Node 1] Entered SoftAP Configuration Mode.");

  // Route 1: Main Provisioning Page
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>Bike Node 1 Setup</title>";
    html += "<style>";
    html += "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 20px; display: flex; justify-content: center; }";
    html += ".card { background: #1e293b; max-width: 400px; width: 100%; border-radius: 12px; padding: 24px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); border: 1px solid #334155; }";
    html += "h2 { margin-top: 0; color: #38bdf8; text-align: center; font-size: 20px; }";
    html += "p { color: #94a3b8; font-size: 13px; text-align: center; }";
    html += "label { display: block; margin-top: 14px; font-weight: 600; font-size: 13px; color: #cbd5e1; }";
    html += "input, select { width: 100%; padding: 10px 12px; margin-top: 6px; border-radius: 6px; border: 1px solid #475569; background: #0f172a; color: #fff; box-sizing: border-box; font-size: 14px; }";
    html += "input:focus, select:focus { outline: none; border-color: #38bdf8; }";
    html += "button { width: 100%; padding: 12px; background: #2563eb; color: #fff; border: none; border-radius: 6px; margin-top: 22px; font-weight: bold; cursor: pointer; font-size: 15px; transition: 0.2s; }";
    html += "button:hover { background: #1d4ed8; }";
    html += "</style></head><body>";
    html += "<div class='card'>";
    html += "<h2>Node-1 Wi-Fi Setup</h2>";
    html += "<p>Vehicle Crash Alert System</p>";
    html += "<form action='/save' method='POST'>";
    
    // Scan networks and populate dropdown
    int n = WiFi.scanNetworks();
    html += "<label>Select Detected Wi-Fi</label>";
    html += "<select name='ssid'>";
    if (n == 0) {
      html += "<option value=''>No Networks Detected</option>";
    } else {
      for (int i = 0; i < n; ++i) {
        html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
      }
    }
    html += "</select>";

    html += "<label>Or Enter Custom SSID</label>";
    html += "<input type='text' name='custom_ssid' placeholder='Manual SSID (Optional)'>";

    html += "<label>Wi-Fi Password</label>";
    html += "<input type='password' name='password' placeholder='Enter Password' required>";

    html += "<button type='submit'>Save & Connect</button>";
    html += "</form></div></body></html>";
    server.send(200, "text/html", html);
  });

  // Route 2: Save Credentials and Reboot
  server.on("/save", HTTP_POST, []() {
    String selSSID = server.arg("ssid");
    String customSSID = server.arg("custom_ssid");
    String pass = server.arg("password");

    String targetSSID = (customSSID.length() > 0) ? customSSID : selSSID;

    if (targetSSID.length() > 0) {
      saveNewNetwork(targetSSID, pass);
      String resp = "<html><body style='background:#0f172a;color:#38bdf8;font-family:sans-serif;text-align:center;padding-top:40px;'>";
      resp += "<h2>Credentials Saved!</h2><p style='color:#94a3b8;'>Rebooting Node 1 and connecting to " + targetSSID + "...</p></body></html>";
      server.send(200, "text/html", resp);
      updateLCD("Saved New WiFi", "Restarting...");
      delay(2000);
      ESP.restart();
    } else {
      server.send(400, "text/html", "<h2>Error: Empty SSID</h2>");
    }
  });

  server.begin();

  // Execution loop during AP mode
  while (currentState == STATE_AP_MODE) {
    server.handleClient();
    digitalWrite(STATUS_LED_PIN, (millis() / 500) % 2);

    if (digitalRead(AP_FORCE_PIN) == LOW) {
      unsigned long startHold = millis();
      while (digitalRead(AP_FORCE_PIN) == LOW) {
        if (millis() - startHold > 3000) {
          server.stop();
          WiFi.softAPdisconnect(true);
          delay(500);
          if (attemptConnectionToKnownNetworks()) {
            currentState = STATE_ARMED;
            updateLCD("System Armed", WiFi.SSID());
          } else {
            WiFi.softAP("AccidentAlert-Setup", "deskBuddy_alert");
            server.begin();
            updateLCD("AP:AccidentAlert", "IP:" + WiFi.softAPIP().toString());
          }
          break;
        }
        delay(50);
      }
    }
    delay(10);
  }
}

// =================================================================
// WIFI AUTO-CONNECT
// =================================================================
bool attemptConnectionToKnownNetworks() {
  updateLCD("Scanning WiFi...", "Please wait...");
  int n = WiFi.scanNetworks();
  if (n == 0 || networkCount == 0) return false;

  int bestIdx = -1;
  int highestRSSI = -1000;

  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    for (int j = 0; j < networkCount; ++j) {
      if (ssid == savedSSIDs[j]) {
        if (WiFi.RSSI(i) > highestRSSI) {
          highestRSSI = WiFi.RSSI(i);
          bestIdx = j;
        }
      }
    }
  }

  if (bestIdx != -1) {
    String targetSSID = savedSSIDs[bestIdx];
    String targetPass = savedPass[bestIdx];

    updateLCD("Connecting to:", targetSSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(targetSSID.c_str(), targetPass.c_str());

    unsigned long startTry = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTry < 10000) {
      delay(250);
      digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
    }

    if (WiFi.status() == WL_CONNECTED) {
      digitalWrite(STATUS_LED_PIN, HIGH);
      updateLCD("Connected!", targetSSID);
      delay(1000);
      return true;
    }
  }
  return false;
}

// =================================================================
// ADAFRUIT IO CLOUD ROUTINES
// =================================================================
void setAdafruitFeedValue(const char* val) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(4000);

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

bool checkAdafruitAck() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3500);

  HTTPClient https;
  bool ackReceived = false;
  String url = "https://io.adafruit.com/api/v2/" + String(AIO_USERNAME) + "/feeds/" + String(AIO_FEED) + "/data/last";

  if (https.begin(client, url)) {
    https.addHeader("X-AIO-Key", AIO_KEY);
    int code = https.GET();
    if (code == 200) {
      String payload = https.getString();
      int valPos = payload.indexOf("\"value\":\"");
      if (valPos != -1 && payload.charAt(valPos + 9) == '0') {
        ackReceived = true; // Desk Buddy has reset the feed to '0'
      }
    }
    https.end();
  }
  client.stop();
  return ackReceived;
}

// =================================================================
// TELEGRAM ALERT & REMOTE RESET API
// =================================================================
String generateTimestampedAlert(float gForce) {
  struct tm timeinfo;
  char dateStr[20] = "12-Oct-2026";
  char timeStr[15] = "03:03 AM";

  if (getLocalTime(&timeinfo, 100)) {
    strftime(dateStr, sizeof(dateStr), "%d-%b-%Y", &timeinfo);
    strftime(timeStr, sizeof(timeStr), "%I:%M %p", &timeinfo);
  }

  String msg = "🚨 *EMERGENCY IMPACT ALERT*\n";
  msg += "━━━━━━━━━━━━━━\n";
  msg += "Device: Node-1 (Vehicle)\n";
  msg += "Status: 🔴 CRASH DETECTED\n";
  msg += "Severity: HIGH (G-Force: " + String(gForce, 1) + "G)\n";
  msg += "━━━━━━━━━━━━━━\n";
  msg += "📅 Date: " + String(dateStr) + "\n";
  msg += "⏰ Time: " + String(timeStr);

  return msg;
}

bool sendTelegramAlert(const String& message) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage";
  String payload = "chat_id=" + String(CHAT_ID) + "&text=" + message + "&parse_mode=Markdown";

  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int httpCode = https.POST(payload);
    https.end();
    client.stop();
    return (httpCode == 200);
  }
  client.stop();
  return false;
}

void syncTelegramOffset() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/getUpdates?offset=-1";

  if (https.begin(client, url)) {
    int code = https.GET();
    if (code == 200) {
      String payload = https.getString();
      int idPos = payload.lastIndexOf("\"update_id\":");
      if (idPos != -1) {
        int commaPos = payload.indexOf(",", idPos);
        lastUpdateId = payload.substring(idPos + 12, commaPos).toInt();
      }
    }
    https.end();
  }
  client.stop();
}

bool checkForRemoteReset() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(2500);

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/getUpdates?offset=" + String(lastUpdateId + 1);

  bool resetFound = false;
  if (https.begin(client, url)) {
    int httpCode = https.GET();
    if (httpCode == 200) {
      String response = https.getString();
      if (response.indexOf("\"update_id\":") != -1) {
        int idPos = response.lastIndexOf("\"update_id\":");
        int commaPos = response.indexOf(",", idPos);
        lastUpdateId = response.substring(idPos + 12, commaPos).toInt();

        response.toLowerCase();
        if (response.indexOf("/reset") != -1 || response.indexOf("\"text\":\"reset\"") != -1) {
          resetFound = true;
        }
      }
    }
    https.end();
  }
  client.stop();
  return resetFound;
}

// =================================================================
// SETUP
// =================================================================
void setup() {
  Serial.begin(115200);

  pinMode(AP_FORCE_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  updateLCD("Node 1 Booting", "Accident System");
  delay(1000);

  // Initialize MPU6050
  if (!mpu.begin()) {
    updateLCD("MPU6050 Error", "Check Wiring!");
    Serial.println("[Node 1] MPU6050 initialization failed!");
    while (1) { delay(100); }
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  loadSavedNetworks();

  // If force button held on boot, enter provisioning
  if (digitalRead(AP_FORCE_PIN) == LOW) {
    startAPMode();
  }

  // Connect to known WiFi
  if (!attemptConnectionToKnownNetworks()) {
    updateLCD("No Known WiFi", "Opening AP Mode");
    delay(1500);
    startAPMode();
  }

  // Synchronize NTP clock (IST UTC +5:30)
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER, "time.google.com");

  // Synchronize Telegram update cursor
  syncTelegramOffset();

  // Clear any latent crash state in Adafruit IO cloud
  setAdafruitFeedValue("0");

  updateLCD("System Armed", WiFi.SSID());
}

// =================================================================
// MAIN LOOP
// =================================================================
void loop() {
  // Hold AP_FORCE_PIN > 3s during runtime to manually open SoftAP
  if (digitalRead(AP_FORCE_PIN) == LOW) {
    unsigned long pressStart = millis();
    while (digitalRead(AP_FORCE_PIN) == LOW) {
      if (millis() - pressStart > 3000) {
        startAPMode();
        return;
      }
      delay(50);
    }
  }

  // --- STATE 3: ALERT LATCHED (Waiting for Guardian ACK) ---
  if (currentState == STATE_ALERT_LATCHED) {
    // 1. Rapid blinking status LED
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 300) {
      lastBlink = millis();
      digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN));
    }

    // 2. Marquee scrolling message on LCD
    if (millis() - lastScrollTime >= LCD_SCROLL_SPEED_MS) {
      lastScrollTime = millis();
      lcd.setCursor(0, 0);
      lcd.print("ACCIDENT ALERT! ");

      String currentSlice = "";
      for (int i = 0; i < 16; i++) {
        currentSlice += SCROLL_MSG[(scrollIndex + i) % SCROLL_MSG.length()];
      }
      lcd.setCursor(0, 1);
      lcd.print(currentSlice);
      scrollIndex = (scrollIndex + 1) % SCROLL_MSG.length();
    }

    // 3. 30s Retry Reminder to Telegram
    if (millis() - lastRetrySentTime >= ALERT_RETRY_INTERVAL_MS) {
      lastRetrySentTime = millis();
      Serial.println("[Node 1] 30s Timeout! Resending Telegram Reminder...");
      sendTelegramAlert("⚠️ *REMINDER*: Accident Alert Unacknowledged! Immediate Help Required!");
    }

    // 4. Verification Check: Adafruit IO ('0') OR Telegram (/reset)
    static unsigned long lastResetPoll = 0;
    if (millis() - lastResetPoll > 3000) {
      lastResetPoll = millis();

      if (checkAdafruitAck() || checkForRemoteReset()) {
        Serial.println("[Node 1] Acknowledgment verified!");

        updateLCD("HELP DISPATCHED!", "GUARDIAN ACKED");
        sendTelegramAlert("✅ *Node 1*: Help Dispatch acknowledged and confirmed on bike display.");

        setAdafruitFeedValue("0"); // Ensure feed is cleared

        delay(4000); // Latch display message for reading
        scrollIndex = 0;
        currentState = STATE_ARMED;
        digitalWrite(STATUS_LED_PIN, HIGH);
        updateLCD("System Armed", WiFi.SSID());
        return;
      }
    }
    delay(20);
    return;
  }

  // --- MOTION & SENSOR PROCESSING (ARMED & VERIFYING MODES) ---
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  // Compute 3D resultant acceleration: ||a|| = sqrt(ax^2 + ay^2 + az^2)
  float netAccMPS2 = sqrt((a.acceleration.x * a.acceleration.x) +
                          (a.acceleration.y * a.acceleration.y) +
                          (a.acceleration.z * a.acceleration.z));
  float netAccG = netAccMPS2 / 9.80665f; // Convert m/s^2 to G-Force

  // Compute 3D resultant angular velocity: ||omega|| in deg/s
  float netGyroDPS = sqrt((g.gyro.x * g.gyro.x) +
                          (g.gyro.y * g.gyro.y) +
                          (g.gyro.z * g.gyro.z)) * (180.0f / 3.14159f);

  // --- STAGE 1: ARMED (Waiting for Impact Spike) ---
  if (currentState == STATE_ARMED) {
    if (netAccG >= IMPACT_THRESHOLD_G && (millis() - lastAlertSentTime > ALERT_COOLDOWN_MS)) {
      capturedGForce = netAccG;
      impactStartTime = millis();
      currentState = STATE_IMPACT_VERIFY;
      Serial.printf("[Node 1] High-G Spike: %.2f G! Verifying immobility...\n", netAccG);
      updateLCD("! IMPACT SPIKE !", "Verifying...");
    }
  }

  // --- STAGE 2: POST-IMPACT VERIFICATION (Stillness Analysis) ---
  else if (currentState == STATE_IMPACT_VERIFY) {
    unsigned long elapsed = millis() - impactStartTime;

    // False positive check: if significant motion continues, abort trigger
    if (netGyroDPS > QUIET_GYRO_THRESHOLD_DPS) {
      currentState = STATE_ARMED;
      Serial.println("[Node 1] Continuous motion detected. Alert aborted.");
      updateLCD("Motion Detected", "Alert Aborted!");
      delay(1500);
      updateLCD("System Armed", WiFi.SSID());
      return;
    }

    float remainingSec = (float)(INACTIVITY_WINDOW_MS - elapsed) / 1000.0f;
    lcd.setCursor(0, 1);
    lcd.printf("Holding: %.1fs   ", (remainingSec > 0.0f ? remainingSec : 0.0f));

    // Crash verified after inactivity window
    if (elapsed >= INACTIVITY_WINDOW_MS) {
      updateLCD("CRASH CONFIRMED!", "Alerting Buddy..");
      Serial.println("[Node 1] Crash verified. Dispatching alerts...");

      // 1. Dispatch custom-formatted Markdown Telegram alert
      String alertPayload = generateTimestampedAlert(capturedGForce);
      bool sent = sendTelegramAlert(alertPayload);
      lastAlertSentTime = millis();

      // 2. Set Adafruit IO feed to '1' to immediately trigger Node 2 (Desk Buddy)
      setAdafruitFeedValue("1");

      if (sent) {
        Serial.println("[Node 1] Telegram & Adafruit alerts dispatched. Waiting for ACK...");
        scrollIndex = 0;
        lastRetrySentTime = millis();
        lcd.clear();
        currentState = STATE_ALERT_LATCHED;
      } else {
        updateLCD("Send Failed!", "Check Network");
        delay(2000);
        currentState = STATE_ARMED;
        updateLCD("System Armed", WiFi.SSID());
      }
    }
  }

  delay(20);
}