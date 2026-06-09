#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <EEPROM.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#include <time.h>

#define ONE_WIRE_BUS 4

// -------------------- WIFI --------------------

const char* ssid = "Cobeldarou1";
const char* password = "@HealthCare@";

// -------------------- NTP --------------------

const long GMT_OFFSET = 12600;

const char* ntpServers[] =
{
  "2.ir.pool.ntp.org",
  "1.asia.pool.ntp.org",
  "ir.pool.ntp.org",
  "ntp.day.ir",
  "time.digiboy.ir",
  "ntp.iranet.ir"
};

// -------------------- RELAYS --------------------
// Active LOW

const uint8_t PIN_PUMP = 14;
const uint8_t PIN_SLOW = 12;
const uint8_t PIN_FAST = 13;

// -------------------- OBJECTS --------------------

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// -------------------- EEPROM --------------------

#define EEPROM_SIZE 512

struct Settings
{
  bool scheduleEnabled;

  uint8_t startHour;
  uint8_t startMinute;

  uint8_t stopHour;
  uint8_t stopMinute;

  bool autoEnabled;

  float autoMaxTemp;
  float autoMinTemp;

  uint32_t crc;
};

Settings settings;

// -------------------- RUNTIME --------------------

float currentTemp = 0;

bool pumpState = false;
bool slowState = false;
bool fastState = false;

// -------------------- AUTO ENGINE --------------------

enum AutoState
{
  AUTO_IDLE,
  AUTO_PUMPING,
  AUTO_COOLING
};

AutoState autoState = AUTO_IDLE;

unsigned long autoPumpStartMillis = 0;

const unsigned long PUMP_PREWET_TIME =
  15UL * 60UL * 1000UL;

// -------------------- SCHEDULE ENGINE --------------------

enum ScheduleState
{
  SCH_IDLE,
  SCH_PUMPING,
  SCH_RUNNING
};

ScheduleState scheduleState = SCH_IDLE;

unsigned long schedulePumpStartMillis = 0;

bool scheduleRunningToday = false;

// -------------------- SENSOR TIMER --------------------

unsigned long lastTempRead = 0;

const unsigned long TEMP_INTERVAL =
  10000UL;

// -------------------- NTP TIMER --------------------

unsigned long lastTimeCheck = 0;

const unsigned long TIME_CHECK_INTERVAL =
  10000UL;

// -------------------- HELPERS --------------------

uint32_t calcCRC32(
  const uint8_t *data,
  size_t length)
{
  uint32_t crc = 0xffffffff;

  while (length--)
  {
    uint8_t c = *data++;

    for (uint32_t i = 0x80; i > 0; i >>= 1)
    {
      bool bit = crc & 0x80000000;

      if (c & i)
      {
        bit = !bit;
      }

      crc <<= 1;

      if (bit)
      {
        crc ^= 0x04c11db7;
      }
    }
  }

  return crc;
}

// -------------------- EEPROM --------------------

void saveSettings()
{
  Settings temp = settings;

  temp.crc =
      calcCRC32(
          (uint8_t*)&temp,
          sizeof(Settings) - sizeof(uint32_t));

  EEPROM.put(0, temp);

  EEPROM.commit();
}

void loadSettings()
{
  EEPROM.begin(EEPROM_SIZE);

  EEPROM.get(0, settings);

  uint32_t crcOfData =
      calcCRC32(
          (uint8_t*)&settings,
          sizeof(Settings) - sizeof(uint32_t));

  if (crcOfData != settings.crc)
  {
    settings.scheduleEnabled = false;

    settings.startHour = 14;
    settings.startMinute = 30;

    settings.stopHour = 18;
    settings.stopMinute = 45;

    settings.autoEnabled = false;

    settings.autoMaxTemp = 26;
    settings.autoMinTemp = 24;

    saveSettings();
  }
}

// -------------------- RELAYS --------------------

void relayPump(bool on)
{
  pumpState = on;

  digitalWrite(
      PIN_PUMP,
      on ? LOW : HIGH);
}

void relaySlow(bool on)
{
  slowState = on;

  digitalWrite(
      PIN_SLOW,
      on ? LOW : HIGH);
}

void relayFast(bool on)
{
  fastState = on;

  digitalWrite(
      PIN_FAST,
      on ? LOW : HIGH);
}

void allOff()
{
  relayPump(false);
  relaySlow(false);
  relayFast(false);
}

// -------------------- SENSOR --------------------

void updateTemperature()
{
  if (millis() - lastTempRead <
      TEMP_INTERVAL)
  {
    return;
  }

  lastTempRead = millis();

  sensors.requestTemperatures();

  float t =
      sensors.getTempCByIndex(0);

  if (t != DEVICE_DISCONNECTED_C)
  {
    currentTemp = t;
  }
}

// -------------------- TIME --------------------

bool getLocalTimeNow(struct tm &timeinfo)
{
  time_t now = time(nullptr);

  if (now < 100000)
  {
    return false;
  }

  struct tm* t = localtime(&now);

  if (t == nullptr)
  {
    return false;
  }

  timeinfo = *t;

  return true;
}

int getMinutesNow()
{
  struct tm timeinfo;

  if (!getLocalTimeNow(timeinfo))
  {
    return -1;
  }

  return
      (timeinfo.tm_hour * 60)
      + timeinfo.tm_min;
}

int getStartMinutes()
{
  return
      settings.startHour * 60
      + settings.startMinute;
}

int getStopMinutes()
{
  return
      settings.stopHour * 60
      + settings.stopMinute;
}

bool isInsideSchedule()
{
  int nowM = getMinutesNow();

  if (nowM < 0)
  {
    return false;
  }

  return
      nowM >= getStartMinutes()
      &&
      nowM < getStopMinutes();
}

// -------------------- WIFI --------------------

void connectWiFi()
{
  WiFi.mode(WIFI_STA);

  WiFi.begin(
      ssid,
      password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
  }
}

// -------------------- NTP --------------------

void initTime()
{
  configTime(
      GMT_OFFSET,
      0,
      ntpServers[0],
      ntpServers[1],
      ntpServers[2]);
}

void syncBackupServers()
{
  configTime(
      GMT_OFFSET,
      0,
      ntpServers[3],
      ntpServers[4],
      ntpServers[5]);
}

// -------------------- BOOT CHECK --------------------

void restoreScheduleState()
{
  if (!settings.scheduleEnabled)
  {
    return;
  }

  if (!isInsideSchedule())
  {
    return;
  }

  int nowM = getMinutesNow();

  int startM = getStartMinutes();

  int passed = nowM - startM;

  if (passed >= 15)
  {
    relayPump(true);
    relaySlow(true);

    scheduleState =
        SCH_RUNNING;
  }
  else
  {
    relayPump(true);

    schedulePumpStartMillis =
        millis() -
        (passed * 60000UL);

    scheduleState =
        SCH_PUMPING;
  }
}

//
// =====================================================
// AUTO ENGINE
// =====================================================
//

void autoStopCooler()
{
  relaySlow(false);
  relayFast(false);
  relayPump(false);

  autoState = AUTO_IDLE;
}

void autoStartPumping()
{
  relayPump(true);

  autoPumpStartMillis = millis();

  autoState = AUTO_PUMPING;
}

void autoStartCooling()
{
  relayPump(true);
  relaySlow(true);

  autoState = AUTO_COOLING;
}

void processAuto()
{
  if (!settings.autoEnabled)
  {
    autoState = AUTO_IDLE;
    return;
  }

  // اگر Schedule فعال است
  // Auto فقط داخل بازه زمانی اجازه کار دارد

  if (settings.scheduleEnabled)
  {
    if (!isInsideSchedule())
    {
      autoStopCooler();
      return;
    }
  }

  switch (autoState)
  {
    case AUTO_IDLE:

      if (currentTemp >= settings.autoMaxTemp)
      {
        autoStartPumping();
      }

      break;

    case AUTO_PUMPING:

      if (
          millis() - autoPumpStartMillis
          >= PUMP_PREWET_TIME)
      {
        autoStartCooling();
      }

      break;

    case AUTO_COOLING:

      if (currentTemp <= settings.autoMinTemp)
      {
        autoStopCooler();
      }

      break;
  }
}

//
// =====================================================
// SCHEDULE ENGINE
// =====================================================
//

void scheduleStop()
{
  relaySlow(false);
  relayFast(false);
  relayPump(false);

  scheduleState = SCH_IDLE;
}

void scheduleStartPumping()
{
  relayPump(true);

  schedulePumpStartMillis =
      millis();

  scheduleState =
      SCH_PUMPING;
}

void scheduleStartCooling()
{
  relayPump(true);
  relaySlow(true);

  scheduleState =
      SCH_RUNNING;
}

void processSchedule()
{
  if (!settings.scheduleEnabled)
  {
    scheduleState = SCH_IDLE;
    return;
  }

  // اگر Auto فعال باشد
  // Auto اولویت دارد

  if (settings.autoEnabled)
  {
    return;
  }

  int nowM = getMinutesNow();

  if (nowM < 0)
  {
    return;
  }

  int startM = getStartMinutes();

  int stopM = getStopMinutes();

  //
  // خارج از بازه
  //

  if (nowM < startM ||
      nowM >= stopM)
  {
    if (scheduleState != SCH_IDLE)
    {
      scheduleStop();
    }

    return;
  }

  //
  // داخل بازه
  //

  int passedMinutes =
      nowM - startM;

  switch (scheduleState)
  {
    case SCH_IDLE:

      scheduleStartPumping();

      break;

    case SCH_PUMPING:

      if (passedMinutes >= 15)
      {
        scheduleStartCooling();
      }
      else
      {
        if (
            millis() -
            schedulePumpStartMillis
            >= PUMP_PREWET_TIME)
        {
          scheduleStartCooling();
        }
      }

      break;

    case SCH_RUNNING:

      break;
  }
}

//
// =====================================================
// STATUS JSON
// =====================================================
//

String getStatusJson()
{
  String json = "{";

  json += "\"temp\":";
  json += String(currentTemp,1);

  json += ",\"pump\":";
  json += (pumpState ? "true":"false");

  json += ",\"slow\":";
  json += (slowState ? "true":"false");

  json += ",\"fast\":";
  json += (fastState ? "true":"false");

  json += ",\"autoEnabled\":";
  json +=
      (settings.autoEnabled
       ? "true"
       : "false");

  json += ",\"scheduleEnabled\":";
  json +=
      (settings.scheduleEnabled
       ? "true"
       : "false");

  json += "}";

  return json;
}

//
// =====================================================
// CLOCK JSON
// =====================================================
//

String getClockString()
{
  struct tm timeinfo;

  if (!getLocalTimeNow(timeinfo))
  {
    return "--:--";
  }

  char buf[16];

  sprintf(
      buf,
      "%02d:%02d",
      timeinfo.tm_hour,
      timeinfo.tm_min);

  return String(buf);
}

//
// =====================================================
// ROUTE HELPERS
// =====================================================
//

void handlePumpOn()
{
  relayPump(true);

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

void handlePumpOff()
{
  relayPump(false);

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

void handleSlowOn()
{
  relaySlow(true);

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

void handleSlowOff()
{
  relaySlow(false);

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

void handleFastOn()
{
  relayFast(true);

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

void handleFastOff()
{
  relayFast(false);

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

void handleAutoOn()
{
  settings.autoEnabled = true;

  saveSettings();

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

void handleAutoOff()
{
  settings.autoEnabled = false;

  autoStopCooler();

  saveSettings();

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

void handleScheduleOn()
{
  settings.scheduleEnabled = true;

  saveSettings();

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

void handleScheduleOff()
{
  settings.scheduleEnabled = false;

  scheduleStop();

  saveSettings();

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

//
// =====================================================
// MAIN HTML
// =====================================================
//

const char MAIN_PAGE[] PROGMEM = R"rawliteral(

<!DOCTYPE html>
<html lang="fa" dir="rtl">

<head>

<meta charset="utf-8">

<meta name="viewport"
content="width=device-width,initial-scale=1">

<title>Smart Cooler</title>

<style>

:root
{
 --bg:#0f172a;
 --card:#1e293b;
 --text:#f8fafc;
 --muted:#94a3b8;
 --green:#10b981;
 --blue:#3b82f6;
 --red:#ef4444;
 --orange:#f59e0b;
}

*
{
 box-sizing:border-box;
 margin:0;
 padding:0;
 font-family:tahoma;
}

body
{
 background:var(--bg);
 color:var(--text);
 padding:12px;
}

.wrap
{
 max-width:520px;
 margin:auto;
}

.card
{
 background:var(--card);
 border-radius:18px;
 padding:18px;
 margin-bottom:12px;
}

.title
{
 text-align:center;
 font-size:24px;
 margin-bottom:10px;
}

.clock
{
 text-align:center;
 font-size:42px;
 font-weight:bold;
}

.temp
{
 text-align:center;
 font-size:34px;
 margin-top:10px;
}

.grid
{
 display:grid;
 grid-template-columns:1fr 1fr;
 gap:10px;
}

.btn
{
 text-decoration:none;
 color:white;
 display:block;

 text-align:center;

 padding:18px;

 border-radius:14px;

 background:#475569;
}

.on
{
 background:var(--green);
}

.auto
{
 background:var(--blue);
}

.section
{
 margin-bottom:10px;
 font-size:18px;
}

.input
{
 width:100%;
 padding:12px;
 border:none;
 border-radius:12px;
 margin-top:5px;
 margin-bottom:10px;
 font-size:18px;
}

.save
{
 width:100%;
 border:none;
 padding:14px;
 border-radius:12px;
 background:var(--orange);
 color:white;
 font-size:18px;
}

.status
{
 line-height:2;
}

.small
{
 color:var(--muted);
 text-align:center;
 margin-top:8px;
}

</style>

</head>

<body>

<div class="wrap">

<div class="card">

<div class="title">
SMART COOLER
</div>

<div
id="clock"
class="clock">
--:--
</div>

<div
id="temp"
class="temp">
--.- °C
</div>

<div class="small">
ESP8266 Controller
</div>

</div>

<div class="card">

<div class="section">
کنترل دستی
</div>

<div class="grid">

<a href="/pump/on"
class="btn"
id="pumpBtn">
Pump
</a>

<a href="/slow/on"
class="btn"
id="slowBtn">
Slow
</a>

<a href="/fast/on"
class="btn"
id="fastBtn">
Fast
</a>

<a href="/auto/on"
class="btn"
id="autoBtn">
Auto
</a>

</div>

</div>

<div class="card">

<div class="section">
زمان بندی
</div>

<form action="/saveSchedule">

<label>
شروع
</label>

<input
name="sh"
type="number"
class="input"
min="0"
max="23">

<input
name="sm"
type="number"
class="input"
min="0"
max="59">

<label>
پایان
</label>

<input
name="eh"
type="number"
class="input"
min="0"
max="23">

<input
name="em"
type="number"
class="input"
min="0"
max="59">

<button
class="save">
ذخیره
</button>

</form>

</div>

<div class="card">

<div class="section">
تنظیمات Auto
</div>

<form action="/saveAuto">

<label>
حداکثر دما
</label>

<input
name="max"
type="number"
step="0.1"
class="input">

<label>
حداقل دما
</label>

<input
name="min"
type="number"
step="0.1"
class="input">

<button
class="save">
ذخیره
</button>

</form>

</div>

<div class="card">

<div class="section">
وضعیت سیستم
</div>

<div
id="status"
class="status">
در حال بارگذاری...
</div>

</div>

</div>

<script>

function updateButton(
  id,
  state,
  onUrl,
  offUrl)
{
  let btn =
    document.getElementById(id);

  if(!btn)
    return;

  if(state)
  {
    btn.classList.add("on");
    btn.href = offUrl;
  }
  else
  {
    btn.classList.remove("on");
    btn.href = onUrl;
  }
}

async function refreshData()
{
  try
  {
    let res =
      await fetch(
        '/status?t=' +
        Date.now());

    let j =
      await res.json();

    document
      .getElementById("temp")
      .innerHTML =
      j.temp + " °C";

    document
      .getElementById("clock")
      .innerHTML =
      j.clock;

    updateButton(
      "pumpBtn",
      j.pump,
      "/pump/on",
      "/pump/off");

    updateButton(
      "slowBtn",
      j.slow,
      "/slow/on",
      "/slow/off");

    updateButton(
      "fastBtn",
      j.fast,
      "/fast/on",
      "/fast/off");

    updateButton(
      "autoBtn",
      j.autoEnabled,
      "/auto/on",
      "/auto/off");

    let txt = "";

    txt +=
      "Pump: "
      + (j.pump ? "ON":"OFF");

    txt += "<br>";

    txt +=
      "Slow: "
      + (j.slow ? "ON":"OFF");

    txt += "<br>";

    txt +=
      "Fast: "
      + (j.fast ? "ON":"OFF");

    txt += "<br>";

    txt +=
      "Auto: "
      + (j.autoEnabled ? "ON":"OFF");

    txt += "<br>";

    txt +=
      "Schedule: "
      + (j.scheduleEnabled ? "ON":"OFF");

    document
      .getElementById("status")
      .innerHTML =
      txt;
  }
  catch(e)
  {
    console.log(e);
  }
}

setInterval(
  refreshData,
  1000);

refreshData();

</script>

</body>

</html>

)rawliteral";

//
// =====================================================
// ROOT PAGE
// =====================================================
//

void handleRoot()
{
  server.send_P(
      200,
      "text/html",
      MAIN_PAGE);
}

//
// =====================================================
// STATUS API
// =====================================================
//

void handleStatus()
{
  String json = "{";

  json += "\"temp\":";
  json += String(currentTemp,1);

  json += ",\"clock\":\"";
  json += getClockString();
  json += "\"";

  json += ",\"pump\":";
  json += (pumpState ? "true" : "false");

  json += ",\"slow\":";
  json += (slowState ? "true" : "false");

  json += ",\"fast\":";
  json += (fastState ? "true" : "false");

  json += ",\"autoEnabled\":";
  json += (settings.autoEnabled ? "true" : "false");

  json += ",\"scheduleEnabled\":";
  json += (settings.scheduleEnabled ? "true" : "false");

  json += "}";

  server.send(
      200,
      "application/json",
      json);
}

//
// =====================================================
// SAVE AUTO
// =====================================================
//

void handleSaveAuto()
{
  if(server.hasArg("max"))
  {
    settings.autoMaxTemp =
      server.arg("max").toFloat();
  }

  if(server.hasArg("min"))
  {
    settings.autoMinTemp =
      server.arg("min").toFloat();
  }

  saveSettings();

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

//
// =====================================================
// SAVE SCHEDULE
// =====================================================
//

void handleSaveSchedule()
{
  if(server.hasArg("sh"))
  {
    settings.startHour =
      server.arg("sh").toInt();
  }

  if(server.hasArg("sm"))
  {
    settings.startMinute =
      server.arg("sm").toInt();
  }

  if(server.hasArg("eh"))
  {
    settings.stopHour =
      server.arg("eh").toInt();
  }

  if(server.hasArg("em"))
  {
    settings.stopMinute =
      server.arg("em").toInt();
  }

  saveSettings();

  server.sendHeader(
      "Location",
      "/");

  server.send(302);
}

//
// =====================================================
// ROUTES
// =====================================================
//

void initRoutes()
{
  server.on(
      "/",
      handleRoot);

  server.on(
      "/status",
      handleStatus);

  server.on(
      "/pump/on",
      handlePumpOn);

  server.on(
      "/pump/off",
      handlePumpOff);

  server.on(
      "/slow/on",
      handleSlowOn);

  server.on(
      "/slow/off",
      handleSlowOff);

  server.on(
      "/fast/on",
      handleFastOn);

  server.on(
      "/fast/off",
      handleFastOff);

  server.on(
      "/auto/on",
      handleAutoOn);

  server.on(
      "/auto/off",
      handleAutoOff);

  server.on(
      "/schedule/on",
      handleScheduleOn);

  server.on(
      "/schedule/off",
      handleScheduleOff);

  server.on(
      "/saveAuto",
      handleSaveAuto);

  server.on(
      "/saveSchedule",
      handleSaveSchedule);
}

//
// =====================================================
// OTA
// =====================================================
//

void initOTA()
{
  httpUpdater.setup(
      &server,
      "/update");
}

//
// =====================================================
// SETUP
// =====================================================
//

void setup()
{
  Serial.begin(115200);

  pinMode(
      PIN_PUMP,
      OUTPUT);

  pinMode(
      PIN_SLOW,
      OUTPUT);

  pinMode(
      PIN_FAST,
      OUTPUT);

  allOff();

  loadSettings();

  sensors.begin();

  connectWiFi();

  initTime();

  delay(3000);

  initRoutes();

  initOTA();

  server.begin();

  restoreScheduleState();

  Serial.println();
  Serial.println("READY");

  Serial.println(
      WiFi.localIP());
}

//
// =====================================================
// LOOP
// =====================================================
//

void loop()
{
  server.handleClient();

  updateTemperature();

  processAuto();

  processSchedule();

  if(
      millis()
      -
      lastTimeCheck
      >
      TIME_CHECK_INTERVAL)
  {
    lastTimeCheck =
      millis();

    syncBackupServers();
  }
}
