
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_sleep.h>
#include <math.h>

// ========== ESP32 PIN CONNECTIONS ==========
#define MOISTURE_PIN 34
#define TDS_PIN      35
#define DS18B20_PIN  4
#define BUZZER_PIN   25

#define SDA_PIN      21
#define SCL_PIN      22

// Set this to your LCD I2C address (commonly 0x27 or 0x3F).
#define LCD_ADDRESS 0x27

// ========== WIFI HOTSPOT ==========
const char* WIFI_NAME = "SOIL_GUARD";
const char* WIFI_PASS = "soilguard123";

WebServer server(80);

// ========== LCD AND TEMPERATURE ==========
LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);

OneWire oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

// ========== TIMING ==========
const uint64_t CYCLE_MS = 600000ULL;  // 10 minutes
const unsigned long WIFI_MS = 120000UL; // 2 minutes awake
const unsigned long DISPLAY_MS = 5000UL;

unsigned long bootMillis = 0;
unsigned long lastSensorRead = 0;
unsigned long lastLCDPage = 0;
unsigned long lastAdviceUpdate = 0;

int lcdPage = 0;
bool buzzerSignalled = false;

// ========== SENSOR VALUES ==========
int moistureRaw = 0;
int moisturePercent = 0;

float temperatureC = NAN;
float tdsPpm = NAN;
float tdsVoltage = 0;

bool temperatureOK = false;

// Starting calibration values.
// Calibrate these for your actual moisture sensor.
const int SOIL_DRY_RAW = 3000;
const int SOIL_WET_RAW = 1300;

// ========== READ SENSORS ==========
void readSensors() {
  // Soil moisture: average 10 readings.
  long moistureSum = 0;

  for (int i = 0; i < 10; i++) {
    moistureSum += analogRead(MOISTURE_PIN);
    delay(10);
  }

  moistureRaw = moistureSum / 10;

  moisturePercent = map(
    moistureRaw,
    SOIL_DRY_RAW,
    SOIL_WET_RAW,
    0,
    100
  );

  moisturePercent = constrain(moisturePercent, 0, 100);

  // DS18B20 temperature.
  tempSensor.requestTemperatures();

  float reading = tempSensor.getTempCByIndex(0);

  if (reading != DEVICE_DISCONNECTED_C &&
      !isnan(reading) &&
      reading >= -55.0 &&
      reading <= 125.0) {
    temperatureC = reading;
    temperatureOK = true;
  } else {
    temperatureC = NAN;
    temperatureOK = false;
  }

  // Analog TDS/conductivity sensor.
  long tdsSum = 0;

  for (int i = 0; i < 10; i++) {
    tdsSum += analogRead(TDS_PIN);
    delay(10);
  }

  int tdsRaw = tdsSum / 10;

  // Approximate voltage for the ESP32 ADC.
  tdsVoltage = tdsRaw * (3.3f / 4095.0f);

  // Approximate demo estimate only, not calibrated.
  float compensation = 1.0f;

  if (temperatureOK) {
    compensation = 1.0f + 0.02f * (temperatureC - 25.0f);
  }

  float voltage = tdsVoltage / compensation;

  tdsPpm = (
    133.42f * pow(voltage, 3) -
    255.86f * pow(voltage, 2) +
    857.39f * voltage
  ) * 0.5f;

  if (tdsPpm < 0 || isnan(tdsPpm)) {
    tdsPpm = 0;
  }

  lastSensorRead = millis();

  Serial.println("\n--- SENSOR READINGS ---");
  Serial.print("Moisture raw: ");
  Serial.println(moistureRaw);

  Serial.print("Moisture: ");
  Serial.print(moisturePercent);
  Serial.println("%");

  Serial.print("Temperature sensor: ");
  Serial.println(temperatureOK ? "OK" : "ERROR");

  if (temperatureOK) {
    Serial.print("Temperature: ");
    Serial.print(temperatureC, 1);
    Serial.println(" C");
  }

  Serial.print("TDS estimate: ");
  Serial.print(tdsPpm, 0);
  Serial.println(" ppm");
}

// ========== FARMER ADVICE ==========
String getAdvice() {
  if (moisturePercent < 30) {
    return "Soil too dry - water crop";
  }

  if (moisturePercent > 70) {
    return "Soil very wet - check drainage";
  }

  if (!temperatureOK) {
    return "Check temperature sensor";
  }

  if (temperatureC >= 38) {
    return "Soil is hot - monitor crop";
  }

  if (temperatureC <= 10) {
    return "Soil is cold - monitor crop";
  }

  if (tdsPpm > 1000) {
    return "High TDS estimate - verify";
  }

  return "Soil looks OK - check again";
}

// ========== SINGLE LCD: SENSOR VALUES AND ADVICE ==========
void updateLCD() {
  lcd.clear();

  if (lcdPage == 0) {
    lcd.setCursor(0, 0);
    lcd.print("Moisture: ");
    lcd.print(moisturePercent);
    lcd.print("%");

    lcd.setCursor(0, 1);
    lcd.print("Temp: ");
    if (temperatureOK) {
      lcd.print(temperatureC, 1);
      lcd.write((uint8_t)223);
      lcd.print("C");
    } else {
      lcd.print("Sensor error");
    }
  } else {
    lcd.setCursor(0, 0);
    lcd.print("TDS:");
    lcd.print(tdsPpm, 0);
    lcd.print(" ppm approx");

    lcd.setCursor(0, 1);
    String advice = getAdvice();
    if (advice.length() > 16) advice = advice.substring(0, 16);
    lcd.print(advice);
  }
}

// ========== WEB DASHBOARD ==========
String dashboardHTML() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SOIL GUARD</title>
<style>
body {
  font-family: Arial, sans-serif;
  background: #edf5e9;
  text-align: center;
  padding: 15px;
}
h1 { color: #236b35; }
.card {
  background: white;
  padding: 18px;
  margin: 12px auto;
  border-radius: 12px;
  max-width: 400px;
}
.value {
  font-size: 24px;
  font-weight: bold;
  color: #236b35;
  margin-top: 8px;
}
</style>
</head>
<body>
<h1>SOIL GUARD</h1>
<p>Soil Monitoring Dashboard</p>

<div class="card">Soil Moisture
<div class="value" id="moisture">--</div></div>

<div class="card">Temperature
<div class="value" id="temperature">--</div></div>

<div class="card">TDS Estimate
<div class="value" id="tds">--</div></div>

<div class="card">Farmer Advice
<div class="value" id="advice">Loading...</div></div>

<p id="status">Connecting to sensor...</p>

<script>
async function updateDashboard() {
  try {
    const response = await fetch('/data');
    const d = await response.json();

    document.getElementById('moisture').textContent =
      d.moisture + '%';

    document.getElementById('temperature').textContent =
      d.tempOK ? d.temp.toFixed(1) + ' °C' : 'Sensor error';

    document.getElementById('tds').textContent =
      d.tds.toFixed(0) + ' ppm (approx)';

    document.getElementById('advice').textContent = d.advice;
    document.getElementById('status').textContent = 'Connected to SOIL_GUARD';
  } catch (e) {
    document.getElementById('status').textContent =
      'Connection lost. Check Wi-Fi.';
  }
}

updateDashboard();
setInterval(updateDashboard, 5000);
</script>
</body>
</html>
)rawliteral";
}

void handleData() {
  String json = "{";

  json += "\"moisture\":" + String(moisturePercent) + ",";
  json += "\"tempOK\":";
  json += temperatureOK ? "true," : "false,";
  json += "\"temp\":";
  json += String(temperatureOK ? temperatureC : 0.0f, 1);
  json += ",";
  json += "\"tds\":" + String(tdsPpm, 1) + ",";
  json += "\"advice\":\"" + getAdvice() + "\"";

  json += "}";

  server.send(200, "application/json", json);
}

// ========== STOP WIFI AND SLEEP ==========
void enterDeepSleep() {
  Serial.println("SOIL GUARD entering deep sleep.");

  // GPIO25 is only a control signal.
  // Use a suitable driver/module for a 5V buzzer.
  digitalWrite(BUZZER_PIN, HIGH);
  delay(300);
  digitalWrite(BUZZER_PIN, LOW);

  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("SOIL GUARD sleeping");

  lcd.setCursor(0, 1);
  lcd.print("Sleep 10 minutes");

  delay(1000);

  uint64_t elapsed = millis() - bootMillis;
  uint64_t remaining = (elapsed < CYCLE_MS)
      ? CYCLE_MS - elapsed
      : 1000ULL;

  esp_sleep_enable_timer_wakeup(remaining * 1000ULL);
  esp_deep_sleep_start();
}

// ========== START WIFI WITH RETRIES ==========
bool startHotspot() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  // Channel 6, visible SSID, maximum 4 clients.
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.print("Starting hotspot, attempt ");
    Serial.println(attempt);

    bool started = WiFi.softAP(
      WIFI_NAME,
      WIFI_PASS,
      6,
      false,
      4
    );

    if (started) {
      delay(300);

      Serial.println("Hotspot started successfully.");
      Serial.print("Wi-Fi name: ");
      Serial.println(WIFI_NAME);
      Serial.print("IP address: ");
      Serial.println(WiFi.softAPIP());

      return true;
    }

    delay(1000);
  }

  Serial.println("ERROR: Hotspot failed to start.");
  return false;
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(MOISTURE_PIN, ADC_11db);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);

  Wire.begin(SDA_PIN, SCL_PIN);

  lcd.init();
  lcd.backlight();


  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("SOIL GUARD starting");

  lcd.setCursor(0, 1);
  lcd.print("Checking sensors");

  tempSensor.begin();
  tempSensor.setResolution(10);
  tempSensor.setWaitForConversion(true);

  delay(1000);
  readSensors();

  updateLCD();

  server.on("/", []() {
    server.send(200, "text/html", dashboardHTML());
  });

  server.on("/data", handleData);

  server.begin();

  bool wifiOK = startHotspot();

  if (wifiOK) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi: SOIL_GUARD");

    lcd.setCursor(0, 1);
    lcd.print("192.168.4.1");

    delay(2000);
  }

  bootMillis = millis();

  // Refresh the readings and displays after the Wi-Fi message.
  readSensors();
  updateLCD();

  lastLCDPage = millis();
  lastAdviceUpdate = millis();

  Serial.println("SOIL GUARD is ready.");
  Serial.println("Connect to SOIL_GUARD Wi-Fi.");
  Serial.println("Open http://192.168.4.1");
  Serial.println("Hotspot stays on for about 2 minutes.");
  Serial.println("After that, the ESP32 deep-sleeps until the next 10-minute cycle.");
}

// ========== MAIN LOOP ==========
void loop() {
  unsigned long now = millis();

  // Serve the dashboard only while the hotspot is active.
  if (now - bootMillis < WIFI_MS) {
    server.handleClient();
  }

  // Refresh sensor values every 5 seconds while awake.
  if (now - lastSensorRead >= 5000UL) {
    readSensors();
  }

  // The single LCD switches between readings and advice every 5 seconds.
  if (now - lastLCDPage >= DISPLAY_MS) {
    lastLCDPage = now;
    lcdPage = 1 - lcdPage;
    updateLCD();
  }

  // Brief buzzer control signal 30 seconds before Wi-Fi shuts down.
  if (!buzzerSignalled && now >= WIFI_MS - 30000UL) {
    buzzerSignalled = true;
    digitalWrite(BUZZER_PIN, HIGH);
    delay(300);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Shut down Wi-Fi and sleep until the next 10-minute cycle.
  if (now >= WIFI_MS) {
    enterDeepSleep();
  }

  delay(5);
}
