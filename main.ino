#define BLYNK_TEMPLATE_ID "TMPL6ir_0blqk"
#define BLYNK_TEMPLATE_NAME "AquariumIOT1"
#define BLYNK_AUTH_TOKEN "JvpAvzGM86ZE4rhaSjtxdfbIuwJSYXBs"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>

/******** WIFI ********/
char ssid[] = "vivo V40 Pro";
char pass[] = "123456789";

/******** RTC DS3231 ********/
#define DS3231_I2C_ADDRESS 0x68
#define I2C_SDA 8
#define I2C_SCL 9

/******** DS18B20 ********/
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

/******** pH SENSOR ********/
#define PH_SENSOR_PIN 5
float calibration_value = 21.34 - 12.25;
unsigned long int avgval;
int buffer_arr[10], temp;
float ph_act = 7.0;

/******** ULTRASONIC ********/
#define trigPin 6
#define echoPin 7
long duration;
float distance = 0;
int levelPercent = 0;
float tankHeight = 18.0;

/******** OUTPUTS ********/
#define RELAY1_PIN 11   // Water pump (Water IN)
#define RELAY2_PIN 12   // pH control / Water OUT
#define RELAY3_PIN 13   // Oxygen pump
#define SERVO_PIN 16    // Feeder servo

Servo feederServo;

/******** TIME SETTINGS ********/
const unsigned long SERVO_ON_DURATION = 1500;
const unsigned long RELAY3_ON_DURATION = 60000;

/******** SYSTEM FLAGS ********/
bool manualMode = false;

// Manual control flags
bool manualWaterOn = false;
bool manualPHOn = false;
bool manualFeedingNow = false;
bool manualOxygenOn = false;

// Auto feeding flags
bool autoFeedingNow = false;
bool relay3OnNow = false;

unsigned long servoStartMillis = 0;
unsigned long relay3StartMillis = 0;

int lastFedHour = -1;
int lastFedDay  = -1;

/******** TIMER ********/
BlynkTimer timer;

/******** RTC HELPERS ********/
byte bcdToDec(byte val) {
  return ((val / 16 * 10) + (val % 16));
}

bool readDS3231time(byte *second, byte *minute, byte *hour,
                    byte *dayOfWeek, byte *dayOfMonth,
                    byte *month, byte *year) {
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0);
  if (Wire.endTransmission() != 0) return false;

  Wire.requestFrom(DS3231_I2C_ADDRESS, 7);
  if (Wire.available() < 7) return false;

  *second     = bcdToDec(Wire.read() & 0x7F);
  *minute     = bcdToDec(Wire.read());
  *hour       = bcdToDec(Wire.read() & 0x3F);
  *dayOfWeek  = bcdToDec(Wire.read());
  *dayOfMonth = bcdToDec(Wire.read());
  *month      = bcdToDec(Wire.read());
  *year       = bcdToDec(Wire.read());

  return true;
}

String dayName(byte d) {
  switch (d) {
    case 1: return "Sunday";
    case 2: return "Monday";
    case 3: return "Tuesday";
    case 4: return "Wednesday";
    case 5: return "Thursday";
    case 6: return "Friday";
    case 7: return "Saturday";
    default: return "?";
  }
}

/******** BLYNK RECONNECT SYNC ********/
BLYNK_CONNECTED() {
  Blynk.syncVirtual(V20, V10, V21, V22, V23);
}

/******** MASTER SWITCH (V20) ********/
BLYNK_WRITE(V20) {
  manualMode = param.asInt();

  if (manualMode) {
    autoFeedingNow = false;
    relay3OnNow = false;
    Blynk.virtualWrite(V24, "Manual mode");
    Serial.println("[SYSTEM] Manual mode");
  } else {
    manualWaterOn = false;
    manualPHOn = false;
    manualFeedingNow = false;
    manualOxygenOn = false;
    autoFeedingNow = false;
    relay3OnNow = false;

    feederServo.write(0);
    digitalWrite(RELAY1_PIN, LOW);
    digitalWrite(RELAY2_PIN, LOW);
    digitalWrite(RELAY3_PIN, LOW);

    Blynk.virtualWrite(V10, 0);
    Blynk.virtualWrite(V21, 0);
    Blynk.virtualWrite(V22, 0);
    Blynk.virtualWrite(V23, 0);
    Blynk.virtualWrite(V11, "Auto mode");
    Blynk.virtualWrite(V24, "Auto mode");
    Serial.println("[SYSTEM] Auto mode");
  }
}

/******** MANUAL FEED (V10) ********/
BLYNK_WRITE(V10) {
  if (!manualMode) {
    Blynk.virtualWrite(V10, 0);
    Blynk.virtualWrite(V24, "Manual mode");
    return;
  }

  int state = param.asInt();

  if (state == 1) {
    manualFeedingNow = true;
    autoFeedingNow = false;

    feederServo.write(60);
    Blynk.virtualWrite(V11, "Feeding ON");
    Blynk.virtualWrite(V24, "Feeding Running");

    Serial.println("[FEED] Manual Feeding ON (servo only)");

    byte s, m, h, dw, dm, mo, yr;
    if (readDS3231time(&s, &m, &h, &dw, &dm, &mo, &yr)) {
      byte hour12 = h;
      bool isPM = (h >= 12);
      if (h == 0) {
        hour12 = 12;
        isPM = false;
      } else if (h > 12) {
        hour12 = h - 12;
      }

      char feedTime[25];
      sprintf(feedTime, "%02d:%02d:%02d %s", hour12, m, s, isPM ? "PM" : "AM");
      Blynk.virtualWrite(V12, feedTime);
    }
  } else {
    manualFeedingNow = false;
    feederServo.write(0);
    Blynk.virtualWrite(V11, "Feeding OFF");
    Blynk.virtualWrite(V24, "Feeding Stopped");
    Serial.println("[FEED] Manual Feeding OFF (servo off)");
  }
}

/******** MANUAL OXYGEN SWITCH (V21) ********/
BLYNK_WRITE(V21) {
  if (!manualMode) {
    Blynk.virtualWrite(V21, 0);
    Blynk.virtualWrite(V24, "Manual mode");
    return;
  }

  int state = param.asInt();
  manualOxygenOn = (state == 1);

  if (manualOxygenOn) {
    digitalWrite(RELAY3_PIN, HIGH);
    Blynk.virtualWrite(V24, "Oxygen ON");
    Serial.println("[RELAY3] Manual ON");
  } else {
    digitalWrite(RELAY3_PIN, LOW);
    Blynk.virtualWrite(V24, "Oxygen OFF");
    Serial.println("[RELAY3] Manual OFF");
  }
}

/******** MANUAL WATER PUMP (RELAY1) – V22 ********/
BLYNK_WRITE(V22) {
  if (!manualMode) {
    Blynk.virtualWrite(V22, 0);
    Blynk.virtualWrite(V24, "Switch to manual mode first");
    return;
  }

  int state = param.asInt();
  manualWaterOn = (state == 1);

  if (manualWaterOn) {
    digitalWrite(RELAY1_PIN, HIGH);
    Blynk.virtualWrite(V24, "Water IN");
    Serial.println("[RELAY1] Manual ON");
  } else {
    digitalWrite(RELAY1_PIN, LOW);
    Blynk.virtualWrite(V24, "Water OFF");
    Serial.println("[RELAY1] Manual OFF");
  }
}

/******** MANUAL WATER OUT (RELAY2) – V23 ********/
BLYNK_WRITE(V23) {
  if (!manualMode) {
    Blynk.virtualWrite(V23, 0);
    Blynk.virtualWrite(V24, "Manual mode");
    return;
  }

  int state = param.asInt();
  manualPHOn = (state == 1);

  if (manualPHOn) {
    digitalWrite(RELAY2_PIN, HIGH);
    Blynk.virtualWrite(V24, "Water OUT ON");
    Serial.println("[RELAY2] Manual ON");
  } else {
    digitalWrite(RELAY2_PIN, LOW);
    Blynk.virtualWrite(V24, "Water OUT OFF");
    Serial.println("[RELAY2] Manual OFF");
  }
}

/******** RTC TO BLYNK ********/
void sendRTC() {
  byte s, m, h, dw, dm, mo, yr;

  if (!readDS3231time(&s, &m, &h, &dw, &dm, &mo, &yr)) {
    Serial.println("[RTC] DS3231 not detected!");
    Blynk.virtualWrite(V0, "NA");
    Blynk.virtualWrite(V1, "NA");
    Blynk.virtualWrite(V2, "NA");
    return;
  }

  byte hour12 = h;
  bool isPM = false;

  if (h >= 12) isPM = true;
  if (h == 0) {
    hour12 = 12;
    isPM = false;
  } else if (h > 12) {
    hour12 = h - 12;
  }

  char timeStr[20];
  sprintf(timeStr, "%02d:%02d:%02d %s", hour12, m, s, isPM ? "PM" : "AM");

  char dateStr[20];
  sprintf(dateStr, "%02d/%02d/%02d", dm, mo, yr);

  String dayStr = dayName(dw);

  Blynk.virtualWrite(V0, timeStr);
  Blynk.virtualWrite(V1, dateStr);
  Blynk.virtualWrite(V2, dayStr);

  Serial.print("[RTC] ");
  Serial.print(timeStr);
  Serial.print(" | ");
  Serial.print(dateStr);
  Serial.print(" | ");
  Serial.println(dayStr);
}

/******** TEMPERATURE (DS18B20) ********/
void sendTemp() {
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);

  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("[TEMP] DS18B20 not detected!");
    Blynk.virtualWrite(V3, NAN);
    Blynk.virtualWrite(V4, NAN);
    return;
  }

  float tempF = DallasTemperature::toFahrenheit(tempC);

  Blynk.virtualWrite(V3, tempC);
  Blynk.virtualWrite(V4, tempF);

  Serial.print("[TEMP] ");
  Serial.print(tempC);
  Serial.print(" C | ");
  Serial.print(tempF);
  Serial.println(" F");
}

/******** pH SENSOR + AUTO CONTROL WITH HYSTERESIS ********/
void sendPH() {
  for (int i = 0; i < 10; i++) {
    buffer_arr[i] = analogRead(PH_SENSOR_PIN);
    delay(20);
  }

  for (int i = 0; i < 9; i++) {
    for (int j = i + 1; j < 10; j++) {
      if (buffer_arr[i] > buffer_arr[j]) {
        temp = buffer_arr[i];
        buffer_arr[i] = buffer_arr[j];
        buffer_arr[j] = temp;
      }
    }
  }

  avgval = 0;
  for (int i = 2; i < 8; i++) {
    avgval += buffer_arr[i];
  }

  float adcAvg = avgval / 6.0;
  float volt = adcAvg * 3.3 / 4095.0;
  ph_act = -5.70 * volt + calibration_value;

  if (volt < 0.15 || volt > 3.00 || ph_act < 0 || ph_act > 14) {
    Serial.println("[PH] Invalid or sensor not detected!");
    Blynk.virtualWrite(V6, NAN);
    if (!manualPHOn) {
      digitalWrite(RELAY2_PIN, LOW);
    }
    return;
  }

  Blynk.virtualWrite(V6, ph_act);

  Serial.print("[PH] Voltage: ");
  Serial.print(volt, 3);
  Serial.print(" V | pH: ");
  Serial.println(ph_act, 2);

  if (manualMode) {
    digitalWrite(RELAY2_PIN, manualPHOn ? HIGH : LOW);
    return;
  }

  if (ph_act < 5.8) {
    digitalWrite(RELAY2_PIN, HIGH);
    Serial.println("[RELAY2] Auto ON - pH too low (<5.8)");
  } else if (ph_act > 8.2) {
    digitalWrite(RELAY2_PIN, HIGH);
    Serial.println("[RELAY2] Auto ON - pH too high (>8.2)");
  } else if (ph_act >= 6.2 && ph_act <= 7.8) {
    digitalWrite(RELAY2_PIN, LOW);
    Serial.println("[RELAY2] Auto OFF - pH normal (6.2-7.8)");
  }
}

/******** WATER LEVEL + AUTO WATER PUMP CONTROL ********/
void measureLevel() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  duration = pulseIn(echoPin, HIGH, 30000);

  if (duration == 0) {
    Serial.println("[WATER] Ultrasonic error!");
    Blynk.virtualWrite(V8, 0);
    Blynk.virtualWrite(V9, "SENSOR ERROR");
    return;
  }

  distance = duration * 0.0343 / 2.0;
  float level = ((tankHeight - distance) / tankHeight) * 100.0;

  if (level < 0) level = 0;
  if (level > 100) level = 100;

  levelPercent = (int)level;

  Blynk.virtualWrite(V8, levelPercent);

  if (levelPercent <= 20) {
    Blynk.virtualWrite(V9, "LOW WATER");
  } else if (levelPercent >= 90) {
    Blynk.virtualWrite(V9, "TANK FULL");
  } else {
    Blynk.virtualWrite(V9, "NORMAL");
  }

  Serial.print("[WATER] Distance: ");
  Serial.print(distance);
  Serial.print(" cm | Level: ");
  Serial.print(levelPercent);
  Serial.println(" %");

  if (manualMode) {
    digitalWrite(RELAY1_PIN, manualWaterOn ? HIGH : LOW);
    return;
  }

  // Auto control with hysteresis
  if (levelPercent <= 20) {
    digitalWrite(RELAY1_PIN, HIGH);
    Serial.println("[RELAY1] Auto ON - Low water (<=20%)");
  } else if (levelPercent >= 90) {
    digitalWrite(RELAY1_PIN, LOW);
    Serial.println("[RELAY1] Auto OFF - Tank full (>=90%)");
  } else {
    Serial.println("[RELAY1] State unchanged");
  }
}

/******** UPDATE LAST FEEDING TIME (V12) ********/
void updateLastFeedingTime() {
  byte s, m, h, dw, dm, mo, yr;
  if (readDS3231time(&s, &m, &h, &dw, &dm, &mo, &yr)) {
    byte hour12 = h;
    bool isPM = false;

    if (h >= 12) isPM = true;
    if (h == 0) {
      hour12 = 12;
      isPM = false;
    } else if (h > 12) {
      hour12 = h - 12;
    }

    char feedTime[25];
    sprintf(feedTime, "%02d:%02d:%02d %s", hour12, m, s, isPM ? "PM" : "AM");
    Blynk.virtualWrite(V12, feedTime);

    Serial.print("[FEED] Last Feeding Time: ");
    Serial.println(feedTime);
  }
}

/******** START AUTO FEEDING ********/
void startAutoFeeding() {
  if (manualMode) return;
  if (autoFeedingNow || manualFeedingNow) return;

  autoFeedingNow = true;
  relay3OnNow = true;
  servoStartMillis = millis();
  relay3StartMillis = millis();

  feederServo.write(60);
  digitalWrite(RELAY3_PIN, HIGH);

  Blynk.virtualWrite(V11, "Feeding + Oxygen ON");
  Blynk.virtualWrite(V24, "Feeding Running");
  Blynk.logEvent("feeding_time", "Fish feeding time");

  Serial.println("[FEED] Feeding Started");
  Serial.println("[RELAY3] Oxygen ON (auto, will turn off after 1 minute)");
  updateLastFeedingTime();
}

/******** STOP SERVO ********/
void stopAutoServo() {
  feederServo.write(0);
  autoFeedingNow = false;
  Blynk.virtualWrite(V11, "Feeding OFF, Oxygen ON");
  Serial.println("[FEED] Servo OFF");
}

/******** AUTO FEEDING CHECK ********/
void autoFeeding() {
  if (manualMode) return;

  byte s, m, h, dw, dm, mo, yr;
  if (!readDS3231time(&s, &m, &h, &dw, &dm, &mo, &yr)) {
    Serial.println("[FEED] RTC error");
    return;
  }

  if ((h % 4 == 0) && m == 0 && s < 5) {
    if (lastFedHour != h || lastFedDay != dm) {
      startAutoFeeding();
      lastFedHour = h;
      lastFedDay = dm;
    }
  }

  if (autoFeedingNow && (millis() - servoStartMillis >= SERVO_ON_DURATION)) {
    stopAutoServo();
  }

  if (relay3OnNow && !manualOxygenOn && (millis() - relay3StartMillis >= RELAY3_ON_DURATION)) {
    digitalWrite(RELAY3_PIN, LOW);
    relay3OnNow = false;
    Blynk.virtualWrite(V11, "Oxygen OFF");
    Serial.println("[RELAY3] Auto Oxygen OFF after 1 min");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(I2C_SDA, I2C_SCL);
  sensors.begin();
  sensors.setResolution(12);

  pinMode(PH_SENSOR_PIN, INPUT);
  analogReadResolution(12);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);

  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);
  digitalWrite(RELAY3_PIN, LOW);

  feederServo.setPeriodHertz(50);
  feederServo.attach(SERVO_PIN, 500, 2400);
  feederServo.write(0);

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  Blynk.virtualWrite(V10, 0);
  Blynk.virtualWrite(V21, 0);
  Blynk.virtualWrite(V22, 0);
  Blynk.virtualWrite(V23, 0);
  Blynk.virtualWrite(V11, "System Ready");
  Blynk.virtualWrite(V24, "Auto mode");

  timer.setInterval(1000L, sendRTC);
  timer.setInterval(2000L, sendTemp);
  timer.setInterval(3000L, sendPH);
  timer.setInterval(1500L, measureLevel);
  timer.setInterval(1000L, autoFeeding);
}

void loop() {
  Blynk.run();
  timer.run();
}