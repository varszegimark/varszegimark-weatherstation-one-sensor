#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include "ThingSpeak.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESP8266HTTPClient.h>

Preferences spiffsMeasurements;

OneWire oneWire(12);
DallasTemperature sensors(&oneWire);
WiFiClient  client;
WiFiClient  clientAPI;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

IPAddress displayIP;

unsigned long previousWifiTryMillis = 0;
unsigned long wifiTryTnterval_ms = 60000;

unsigned long previousMillis = 0;
unsigned long interval = 30000;

//unsigned long myChannelNumber = 1985600;
unsigned long myChannelNumber = 1988141;
//const char * myWriteAPIKey = "7Y8WAA3BG5N4R6GS";
const char * myWriteAPIKey = "WM85208G1ABZG5N5";

const char* wiFiSSID = "blokkk";
const char* wiFiPassword = "3775580c48";

bool prevWifiOnline = false;

bool builtinLedIsOn = false;

time_t epochTime;
unsigned long offlineTime = 0;

int intervalTsUpdate1 = 30 * 60 * 1000;
unsigned long lastTsUpdate1 = 0;

int intervalNTPUpdate = 30 * 60 * 1000;
unsigned long lastNTPUpdate = 0;

int intervalDisplayUpdate = 10000;
unsigned long lastDisplayUpdate = 0;

int intervalTimeCheck = 1000;
unsigned long lastTimeCheck = 0;

String getTimeFromEpoch(unsigned long rawTime) {
  unsigned long hours = (rawTime % 86400L) / 3600;
  String hoursStr = hours < 10 ? "0" + String(hours) : String(hours);

  unsigned long minutes = (rawTime % 3600) / 60;
  String minuteStr = minutes < 10 ? "0" + String(minutes) : String(minutes);

  unsigned long seconds = rawTime % 60;
  String secondStr = seconds < 10 ? "0" + String(seconds) : String(seconds);

  return hoursStr + ":" + minuteStr + ":" + secondStr;
}

String getDateTimeOffline() {
  time_t currEpochTime = ((millis() - offlineTime)/1000UL) + epochTime;
  struct tm *ptm = gmtime ((time_t *)&currEpochTime);
  int currentYear = ptm->tm_year+1900;
  int currentMonth = ptm->tm_mon+1;
  int monthDay = ptm->tm_mday;
  String currentMonthPad = "";
  if (currentMonth < 10) { currentMonthPad = "0"; }
  String monthDayPad = "";
  if (monthDay < 10) { monthDayPad = "0"; }
  String timeStr = getTimeFromEpoch(currEpochTime);

  String currentDate = String(currentYear) + "-" + currentMonthPad + String(currentMonth) + "-" + monthDayPad + String(monthDay) + " " + timeStr;
  return currentDate;
}

void clearStoredMeasurements() {
  spiffsMeasurements.putString("meas","{\"data\":[]}");
}

void storeMeasurement(float temp, int field) {
  String meas = spiffsMeasurements.getString("meas","{\"data\":[]}");

  StaticJsonDocument<8192> doc;
  char buffer[8192];
  DeserializationError error = deserializeJson(doc, meas);
 
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  String created_at = getDateTimeOffline() + " +0100";

  int array_size = doc["data"].as<JsonArray>().size();
  bool existing_created_at = false;
  for (int i=0; i< array_size; i++) {
    if(doc["data"][i]["created_at"] == created_at) {
      doc["data"][i]["field" + (String)field] = String(temp,2);
      existing_created_at = true;
    }
  }
  
  if (!existing_created_at) {
    StaticJsonDocument<200> values;
    values["created_at"] = created_at;
    values["field" + (String)field] = String(temp,2);
    doc["data"].as<JsonArray>().add(values);
  }

  size_t n = serializeJson(doc, buffer);
  spiffsMeasurements.putString("meas",String(buffer));
}

int httpRequest(char* jsonBuffer) {
  String data = "{\"write_api_key\":\"" + String(myWriteAPIKey) + "\",\"updates\":" + String(jsonBuffer) + "}";
  Serial.println("Bulk writing values to Thingspeak: " + data);

  HTTPClient http;
  String url = "http://api.thingspeak.com/channels/" + String(myChannelNumber) + "/bulk_update.json";
  Serial.println(url);
  http.begin(clientAPI, url);
  delay(1000);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(data);
  String payload = http.getString();

  Serial.println("HTTP Code: " + String(httpCode));
  Serial.println("Payload: " + payload);

  http.end();

  return httpCode;
}

void sendStoredMeasurements() {
  if (WiFi.status() != WL_CONNECTED) { return; }
  String meas = spiffsMeasurements.getString("meas","{\"data\":[]}");
  
  StaticJsonDocument<8192> doc;
  DeserializationError error = deserializeJson(doc, meas);
 
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }
 
  int arraySize = doc["data"].size();
  
  if(arraySize > 0) {
    char buffer[8192];
    size_t n = serializeJson(doc["data"], buffer);
    int httpCode = httpRequest(buffer);
    if(httpCode == 202) {
      clearStoredMeasurements();
    }
  }
}

float getTemp() {
  sensors.requestTemperatures();
  if(sensors.getDeviceCount() > 0) {
    return sensors.getTempCByIndex(0);
  }
  return -256;
}


void builtinLedOn() {
  digitalWrite(BUILTIN_LED, LOW);
  builtinLedIsOn = true;
}

void builtinLedOff() {
  digitalWrite(BUILTIN_LED, HIGH);
  builtinLedIsOn = false;
}

void builtinLedToggle() {
  digitalWrite(BUILTIN_LED, builtinLedIsOn ? HIGH : LOW);
  builtinLedIsOn = !builtinLedIsOn;
}

void reConnectToWifi() {
  unsigned long currentMillis = millis();
  // if WiFi is down, try reconnecting
  if ((WiFi.status() != WL_CONNECTED) && (currentMillis - previousMillis >= interval)) {
    prevWifiOnline = false;
    Serial.println("Reconnecting to WiFi...");
    WiFi.disconnect();
    WiFi.begin(wiFiSSID, wiFiPassword);
    previousMillis = currentMillis;
  } else if (WiFi.status() == WL_CONNECTED && prevWifiOnline == false) {
    Serial.println("WiFi Connection is back");
    prevWifiOnline = true;
  }
}

void reConnectToWiFiOld() {
  unsigned long now = millis();
  if ((WiFi.status() != WL_CONNECTED) && (now - previousWifiTryMillis >= wifiTryTnterval_ms)) {
    builtinLedOff();
    Serial.print("\nReconnecting to WiFi");
    WiFi.disconnect();
    WiFi.reconnect();
    previousWifiTryMillis = now;
  

    unsigned long startReConnectTs = millis();
    while (WiFi.status() != WL_CONNECTED) {
      Serial.print(".");
      builtinLedToggle();
      delay(500);
    }
    Serial.println("ok");
    builtinLedOff();
  }
}

String getDateTimeNTP() {
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    //if(timeClient.isTimeSet()) {
    epochTime = timeClient.getEpochTime();
    offlineTime = millis();
    struct tm *ptm = gmtime ((time_t *)&epochTime);
    int currentYear = ptm->tm_year+1900;
    int currentMonth = ptm->tm_mon+1;
    int monthDay = ptm->tm_mday;
    String currentMonthPad = "";
    if (currentMonth < 10) { currentMonthPad = "0"; }
    String monthDayPad = "";
    if (monthDay < 10) { monthDayPad = "0"; }
    String currentDate = String(currentYear) + "-" + currentMonthPad + String(currentMonth) + "-" + monthDayPad + String(monthDay) + " " + timeClient.getFormattedTime();
    //}
    Serial.println("Datetime set from NTP [OK]: " + currentDate);
    return currentDate;
  } else {
    Serial.println("Datetime set from NTP [FAILED]: No WiFi connection");
    return "";
  }
}

String getTimeOfTheDayOffline() {
  time_t currEpochTime = ((millis() - offlineTime)/1000UL) + epochTime;
  String timeStr = getTimeFromEpoch(currEpochTime);

  return timeStr;
}

void sendTempToThingspeak(int fieldNr) {
  float tempC = getTemp();
  if(tempC == -256) { 
    Serial.println("Temp sensor unavailable");
    return; 
  }
  Serial.println("Sending temp (" + String(tempC,2) + "C°) to Thingspeak field " + String(fieldNr));
  if (WiFi.status() == WL_CONNECTED) {
    int x = ThingSpeak.writeField(myChannelNumber, fieldNr, String(tempC,2), myWriteAPIKey);
    if(x == 200){
      Serial.println("Channel update successful.");
    }
    else{
      Serial.println("Problem updating channel. HTTP error code " + String(x));
    }
  } else {
    Serial.println("Store measured temp to flash memory.");
    storeMeasurement(tempC, fieldNr);
  }
}

void sendTempToDisplay() {
  if (!displayIP.isSet()) {
    return;
  }

  float tempC = getTemp();
  if(tempC == -256) { 
    return; 
  }

  HTTPClient http;
  http.begin(clientAPI, "http://" + displayIP.toString() + "/api");
  http.addHeader("Content-Type", "application/json");
  char buffer[32];
  sprintf(buffer, "{\"temp\":\"%.2f\"}", tempC);
  int httpResponseCode = http.POST(buffer);
  Serial.print("HTTP Response code: ");
  Serial.println(httpResponseCode);

  // read the response from the server
  String payload = http.getString();
  Serial.println(payload);

  // close the connection
  http.end();
  
}

void resolveDisplayAddress() {
  int n = MDNS.queryService("http", "tcp");

  for (int i = 0; i < n; ++i) {
    if (MDNS.hostname(i) == "weatherstation_one_display.local") {
      displayIP = MDNS.IP(i);
    }
    /*Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(MDNS.hostname(i));
    Serial.print(" (");
    Serial.print(MDNS.IP(i));
    Serial.print(":");
    Serial.print(MDNS.port(i));
    Serial.println(")");*/
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUILTIN_LED, OUTPUT);
  builtinLedOff();

  spiffsMeasurements.begin("measurements", false);
  
  Serial.print("\nConnecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.hostname("WeatherstationOne_Sensor");
  WiFi.begin(wiFiSSID, wiFiPassword);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    builtinLedToggle();
    delay(1000);
  }
  Serial.println("ok");
  prevWifiOnline = true;
  builtinLedOff();

  if (MDNS.begin("weatherstation_one_sensor")) {
    Serial.println("mDNS responder started");
  } else {
    Serial.println("Error starting mDNS responder");
  }

  timeClient.begin();
  timeClient.setTimeOffset(3600);
  getDateTimeNTP();

  ThingSpeak.begin(client);
  
  sensors.begin();
  sensors.requestTemperatures();
  sensors.setWaitForConversion(false); //switch to async mode
  
}

void loop() {
  reConnectToWifi();
  MDNS.update();
  resolveDisplayAddress();
  unsigned long now = millis();
  String timeOfTheDay = getTimeOfTheDayOffline();
  
  if(now - lastNTPUpdate > intervalNTPUpdate) {
    getDateTimeNTP();
    lastNTPUpdate = now;
  }

  if(lastTimeCheck == 0 || now - lastTimeCheck > intervalTimeCheck) {
    if (timeOfTheDay == "00:00:00" || timeOfTheDay == "06:00:00" || timeOfTheDay == "12:00:00" || timeOfTheDay == "18:00:00") {
      Serial.print("[" + timeOfTheDay + "] ");
      sendTempToThingspeak(2);
    }
    lastTimeCheck = now;
  }

  if(lastTsUpdate1 == 0 || now - lastTsUpdate1 > intervalTsUpdate1) {
    Serial.print("[" + timeOfTheDay + "] ");
    sendTempToThingspeak(1);
    lastTsUpdate1 = now;
    sendStoredMeasurements();
  }

  if(lastDisplayUpdate == 0 || now - lastDisplayUpdate > intervalDisplayUpdate) {
    sendTempToDisplay();
    lastDisplayUpdate = now;
  }
  
}
