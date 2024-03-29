#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>
#include <arduino_secrets.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <MHZ19.h>

#define RX_PIN D4  // D7
#define TX_PIN D3  // D6
#define CO2_INTERVAL 10000

const char* ssid          = SECRET_GENERAL_WIFI_SSID;
const char* password      = SECRET_GENERAL_WIFI_PASSWORD;
const char* mqttServer    = SECRET_MQTT_SERVER;
const int   mqttPort      = SECRET_MQTT_PORT;
const char* mqttUser      = SECRET_MQTT_USER;
const char* mqttPassword  = SECRET_MQTT_PASSWORD;

long lastReconnectAttempt = 0;
long lastCo2Measured = 0;

float smoothing_factor = 0.5;
float smoothing_factor2 = 0.15;

const char* hostname = "mh-z19b";

char mqtt_topic_status[]  = "esp/status/mh-z19b";
char mqtt_topic_data[]    = "esp/sensors/co2/mh-z19b";
char mqtt_topic_set_abc[] = "esp/set/mh-z19b";

StaticJsonDocument<240> dataDoc;

int co2 = 0;
int co2_mean = 0;
int co2_mean2 = 0;

WiFiClient espClient;
PubSubClient client(espClient);
SoftwareSerial mySerial(RX_PIN, TX_PIN);
MHZ19 myMHZ19;

boolean mqtt_reconnect() {
  if(client.connect(hostname, mqttUser, mqttPassword, mqtt_topic_status, 2, true, "offline")) {
    client.publish(mqtt_topic_status, "online", true);
    client.subscribe(mqtt_topic_set_abc);
  }

  return client.connected();
}

boolean wifi_reconnect() {
  WiFi.hostname(hostname);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  WiFi.waitForConnectResult();

  dataDoc["IP"] = WiFi.localIP();

  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    //  "Ошибка при аутентификации"
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    //  "Ошибка при начале OTA-апдейта"
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    //  "Ошибка при подключении"
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    //  "Ошибка при получении данных"
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    //  "Ошибка при завершении OTA-апдейта"
  });
  ArduinoOTA.begin();
  return true;
}

void zero_calibration() {
  // Start calibration of the sensor
  // This is not needed if you use auto calibration (abc)
  //
  // For zero calibration you need to place a sensor in clear enveronment (forest for example) 
  // for at least 20 min before start
  // 

  // Turn off the ABC (auto baseline calibration) before start
  myMHZ19.autoCalibration(false);

  unsigned long timeElapse = 12e5;
  unsigned long startTime = millis();
  unsigned long endTime = startTime + timeElapse;
  
  // waiting for 20 min (timeElapse value)
  // good idea to leave sensor alone
  while (millis() < endTime) { 
    delay(1000);
  }

  // Start calibrate. Current CO2 value will be set as "zero"
  // For MH-Z19B zero is 400 ppm
  myMHZ19.calibrate();

  return;
}

void callback(char* topic, byte* payload, unsigned int length) {
  char cmd_abc[32] = "";
  char cmd_zero[32] = "";
  
  memset(cmd_abc, 0, sizeof(cmd_abc));
  memset(cmd_zero, 0, sizeof(cmd_zero));

  StaticJsonDocument<256> payload_doc;
  deserializeJson(payload_doc, payload, length);

  strlcpy(cmd_abc, payload_doc["abc"] | "default", sizeof(cmd_abc));
  strlcpy(cmd_zero, payload_doc["zero"] | "default", sizeof(cmd_zero));

  if(strcmp(cmd_abc, "enable") == 0) {
    myMHZ19.autoCalibration(true);
  }

  if(strcmp(cmd_abc, "disable") == 0) {
    myMHZ19.autoCalibration(false);
  }

  if(strcmp(cmd_zero, "start") == 0) {
    zero_calibration();
  }

}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if(WiFi.status() != WL_CONNECTED) {
    wifi_reconnect();
  }

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
  mqtt_reconnect();

  mySerial.begin(9600);
  myMHZ19.begin(mySerial);

  //myMHZ19.autoCalibration(false);
  myMHZ19.setRange(2000);
}

void loop() {
  ArduinoOTA.handle();
  if (WiFi.status() != WL_CONNECTED) {
    wifi_reconnect();
  }

  if(!client.connected()) {
    long now = millis();
    if(now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      if(mqtt_reconnect()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    client.loop();
  }
  
  long co2_time = millis();
  if(co2_time - lastCo2Measured > CO2_INTERVAL) {
    co2 = myMHZ19.getCO2(false);
    if(myMHZ19.errorCode == RESULT_OK) {
      if (!co2_mean) co2_mean = co2;
        co2_mean = co2_mean - smoothing_factor*(co2_mean - co2);
      if (!co2_mean2) co2_mean2 = co2;
        co2_mean2 = co2_mean2 - smoothing_factor2*(co2_mean2 - co2);

      dataDoc["current"]  = co2;
      dataDoc["mean"]     = co2_mean;
      dataDoc["mean2"]    = co2_mean2;
    }

    dataDoc["IP"]       = WiFi.localIP().toString();
    dataDoc["Temp"]     = myMHZ19.getTemperature();
    dataDoc["Range"]    = myMHZ19.getRange();
    myMHZ19.getABC() ? dataDoc["abc"] = "enabled" : dataDoc["abc"] = "disabled";
    if(myMHZ19.errorCode == 1) { 
      dataDoc["status"]   = "OK";
    } else {
      dataDoc["status"]   = myMHZ19.errorCode;
    }
    dataDoc["background value"] = myMHZ19.getBackgroundCO2();
    dataDoc["accuracy"] = myMHZ19.getAccuracy();

    char buffer[256];
    memset(buffer, 0, sizeof(buffer));
    size_t n = serializeJson(dataDoc, buffer);
    client.publish(mqtt_topic_data, buffer, n);
    
    lastCo2Measured = co2_time;
  }

}
