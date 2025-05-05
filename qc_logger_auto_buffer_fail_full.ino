#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <ElegantOTA.h>
#include <time.h>

#define goodButton    D4
#define rejectButton  D5
#define repairButton  D6
#define resetButton   D3
#define BUZZER_PIN    D7
#define RELAY_PIN     D8

LiquidCrystal_I2C lcd(0x27, 20, 4);

#define EEPROM_SIZE   4096
#define ADDR_GOOD     0
#define ADDR_REJECT   4
#define ADDR_REPAIR   8
#define ADDR_DATE     12

#define EEPROM_LOG_START 100
#define EEPROM_LOG_SIZE 40
#define EEPROM_LOG_MAX 1000

//const char* ssid = "SANSAN_WIFI";
//const char* password = "1357246890";
const char* ssid = "arduino";
const char* password = "arduinouno";
const char* serverIP = "192.168.20.25";
//const char* serverIP1 = "sansan-qc.hpy.co.id";

//Static IP Configuration
//IPAddress local_IP(192, 168, 150, 216);
//IPAddress gateway(192, 168, 0, 236);
//IPAddress subnet(255, 255, 252, 0);
//IPAddress dns1(192, 168, 0, 111);
//IPAddress dns2(8, 8, 8, 8);

const String line = "Line 5";
const String unit = "Premium";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200, 60000);
ESP8266WebServer server(80);
const char* otaPassword = "admin";

int goodCount = 0;
int rejectCount = 0;
int repairCount = 0;
String lastResetDate = "";

unsigned long resetPressTime = 0;
bool resetActive = false;
unsigned long lastDisplaySwitch = 0;
unsigned long lastSaveTime = 0;
const unsigned long saveInterval = 3600000;
bool needSave = false;

const unsigned long debounceDelay = 150;
unsigned long lastGoodButtonPress = 0;
unsigned long lastRejectButtonPress = 0;
unsigned long lastRepairButtonPress = 0;
bool resetDoneToday = false;

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);

  WiFi.begin(ssid, password);

  pinMode(goodButton, INPUT_PULLUP);
  pinMode(rejectButton, INPUT_PULLUP);
  pinMode(repairButton, INPUT_PULLUP);
  pinMode(resetButton, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("QC Counter Start");
  lcd.setCursor(0, 1);
  lcd.print(unit + "-" + line);

  loadCounters();

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 30) {
    delay(500);
    Serial.print(".");
  }

  lcd.clear();
  if (WiFi.status() == WL_CONNECTED) {
    lcd.print("WiFi OK");
    timeClient.begin();
  } else {
    lcd.print("WiFi Fail");
  }
  delay(1000);

  server.on("/", []() {
    server.send(200, "text/plain", "Wemos D1 Mini - QC Logger");
  });

  server.on("/reset", HTTP_GET, []() {
    if (server.hasArg("pass") && server.arg("pass") == otaPassword) {
      goodCount = rejectCount = repairCount = 0;
      lastResetDate = getDateString();
      saveCounters();
      server.send(200, "text/plain", "EEPROM reset via web");
    } else {
      server.send(403, "text/plain", "Unauthorized");
    }
  });

  ElegantOTA.begin(&server, "admin", otaPassword);
  server.begin();
}

void loop() {
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED) WiFi.begin(ssid, password);
  timeClient.update();

  handleDailyReset();
  handleButtons();
  handlePeriodicSave();
  handleDisplayUpdate();
  handleRelayControl();
  resendBufferedLogs();
}

void handleButtons() {
  if (digitalRead(goodButton) == LOW && millis() - lastGoodButtonPress > debounceDelay) {
    goodCount++; needSave = true;
    logAndDisplay("good", goodCount);
    beep(); lastGoodButtonPress = millis();
  }

  if (digitalRead(rejectButton) == LOW && millis() - lastRejectButtonPress > debounceDelay) {
    rejectCount++; needSave = true;
    logAndDisplay("reject", rejectCount);
    beep(); lastRejectButtonPress = millis();
  }

  if (digitalRead(repairButton) == LOW && millis() - lastRepairButtonPress > debounceDelay) {
    repairCount++; needSave = true;
    logAndDisplay("repair", repairCount);
    beep(); lastRepairButtonPress = millis();
  }

  if (digitalRead(resetButton) == LOW) {
    if (!resetActive) {
      resetPressTime = millis(); resetActive = true;
      lcd.clear(); lcd.print("Hold 3s to reset");
    }

    int remaining = 3 - (millis() - resetPressTime) / 1000;
    if (remaining >= 0) {
      lcd.setCursor(11, 1);
      lcd.print(String(remaining) + "s ");
    }

    if (millis() - resetPressTime >= 3000) {
      goodCount = rejectCount = repairCount = 0;
      lastResetDate = getDateString(); saveCounters();
      lcd.clear(); lcd.print("EEPROM Reset!");
      lcd.setCursor(0, 1); lcd.print("Counts Cleared");
      beep(); delay(500); beep(); delay(500); beep();
      resetActive = false;
    }
  } else if (resetActive) {
    lcd.clear(); lcd.print("Reset Cancelled");
    delay(1000); resetActive = false;
  }
}

void handleDailyReset() {
  String today = getDateString();
  if (today != lastResetDate && !resetDoneToday && timeClient.getHours() == 6) {
    goodCount = rejectCount = repairCount = 0;
    lastResetDate = today; saveCounters(); resetDoneToday = true;
  }
  if (timeClient.getHours() != 6) resetDoneToday = false;
}

void handlePeriodicSave() {
  if (needSave && millis() - lastSaveTime > 3000) {
    saveCounters(); lastSaveTime = millis(); needSave = false;
  }
  if (millis() - lastSaveTime > saveInterval) {
    saveCounters(); lastSaveTime = millis();
  }
}

void handleDisplayUpdate() {
  if (millis() - lastDisplaySwitch > 3000) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Good = "); lcd.print(goodCount);
    lcd.setCursor(0, 1); lcd.print("Reject = "); lcd.print(rejectCount);
    lcd.setCursor(0, 2); lcd.print("Repair = "); lcd.print(repairCount);
    lcd.setCursor(0, 3); lcd.print("Total = "); lcd.print(goodCount + rejectCount);
    lastDisplaySwitch = millis();
  }
}

void handleRelayControl() {
  int total = goodCount + rejectCount;
  float defectPercent = total > 0 ? (rejectCount * 100.0 / total) : 0;
  digitalWrite(RELAY_PIN, (defectPercent > 10) ? HIGH : LOW);
}

void logAndDisplay(String label, int count) {
  String logTime = getDateTime();
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    WiFiClient client;
    String url = "http://" + String(serverIP) + "/qc/log.php";
    http.begin(client, url);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String postData = "count_type=" + label +
                      "&count=1" +
                      "&log_timestamp=" + logTime +
                      "&line_name=" + line +
                      "&unit_name=" + unit;

    int httpCode = http.POST(postData);
    http.end();

    if (httpCode != 200) {
      bufferLogEEPROM(label, logTime);
    } else {
      Serial.println("Posted: " + label + " " + logTime);
    }
  } else {
    bufferLogEEPROM(label, logTime);
  }
}

void bufferLogEEPROM(String label, String timestamp) {
  for (int i = 0; i < EEPROM_LOG_MAX; i++) {
    int addr = EEPROM_LOG_START + i * EEPROM_LOG_SIZE;
    if (EEPROM.read(addr) == 0xFF || EEPROM.read(addr) == 0x00) {
      String logEntry = label + "|" + timestamp;
      for (int j = 0; j < logEntry.length(); j++) {
        EEPROM.write(addr + j, logEntry[j]);
      }
      EEPROM.write(addr + logEntry.length(), 0);
      EEPROM.commit();
      Serial.println("Buffered: " + logEntry);
      break;
    }
  }
}

void resendBufferedLogs() {
  if (WiFi.status() == WL_CONNECTED) {
    for (int i = 0; i < EEPROM_LOG_MAX; i++) {
      int addr = EEPROM_LOG_START + i * EEPROM_LOG_SIZE;
      if (EEPROM.read(addr) != 0xFF && EEPROM.read(addr) != 0x00) {
        String log = readString(addr);
        int sep = log.indexOf('|');
        if (sep > 0) {
          String label = log.substring(0, sep);
          String ts = log.substring(sep + 1);

          HTTPClient http;
          WiFiClient client;
          String url = "http://" + String(serverIP) + "/qc/log.php";
          http.begin(client, url);
          http.addHeader("Content-Type", "application/x-www-form-urlencoded");
          String postData = "count_type=" + label + "&count=1" + "&log_timestamp=" + ts + "&line_name=" + line + "&unit_name=" + unit;
          int httpCode = http.POST(postData);
          http.end();

          if (httpCode == 200) {
            for (int j = 0; j < EEPROM_LOG_SIZE; j++) EEPROM.write(addr + j, 0xFF);
            EEPROM.commit();
            Serial.println("Flushed: " + log);
          }
        }
      }
    }
  }
}

void saveCounters() {
  EEPROM.put(ADDR_GOOD, goodCount);
  EEPROM.put(ADDR_REJECT, rejectCount);
  EEPROM.put(ADDR_REPAIR, repairCount);
  writeString(ADDR_DATE, getDateString());
  EEPROM.commit();
}

void loadCounters() {
  EEPROM.get(ADDR_GOOD, goodCount);
  EEPROM.get(ADDR_REJECT, rejectCount);
  EEPROM.get(ADDR_REPAIR, repairCount);
  lastResetDate = readString(ADDR_DATE);
}

String getDateTime() {
  time_t now = timeClient.getEpochTime();
  struct tm *timeinfo = gmtime(&now);
  char buffer[20];
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d",
          timeinfo->tm_year + 1900,
          timeinfo->tm_mon + 1,
          timeinfo->tm_mday,
          timeinfo->tm_hour,
          timeinfo->tm_min,
          timeinfo->tm_sec);
  return String(buffer);
}

String getDateString() {
  time_t now = timeClient.getEpochTime();
  struct tm *timeinfo = gmtime(&now);
  char buffer[11];
  sprintf(buffer, "%04d-%02d-%02d",
          timeinfo->tm_year + 1900,
          timeinfo->tm_mon + 1,
          timeinfo->tm_mday);
  return String(buffer);
}

void writeString(int addr, String data) {
  int len = data.length();
  for (int i = 0; i < len; i++) EEPROM.write(addr + i, data[i]);
  EEPROM.write(addr + len, 0);
}

String readString(int addr) {
  char data[EEPROM_LOG_SIZE];
  int len = 0;
  unsigned char k = EEPROM.read(addr);
  while (k != 0 && len < EEPROM_LOG_SIZE - 1) {
    data[len++] = k;
    k = EEPROM.read(addr + len);
  }
  data[len] = '\0';
  return String(data);
}

void beep() {
  digitalWrite(BUZZER_PIN, HIGH); delay(100);
  digitalWrite(BUZZER_PIN, LOW);
}
