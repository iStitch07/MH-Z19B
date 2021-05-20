#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>
#include <arduino_secrets.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>

#define MH_Z19_RX D4  // D7
#define MH_Z19_TX D3// D6
#define CO2_INTERVAL 15000

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

const char* hostname = "mhz-19b";

char mqtt_topic_status_base[] = "esp/status/";
char mqtt_topic_data_base[] = "esp/sensors/co2/";

char mqtt_topic_status[sizeof(mqtt_topic_status_base) + sizeof(hostname) + 5];
char mqtt_topic_data[sizeof(mqtt_topic_data_base) + sizeof(hostname) + 5];

StaticJsonDocument<200> dataDoc;

byte cmd_z19[]    = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
byte abc_z19[]    = {0xFF, 0x01, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86};
byte rpl_z19[]    = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#define CO2_HBYTE 2
#define CO2_LBYTE 3

int z19_co2;
int z19_co2_mean;
int z19_co2_mean2;

WiFiClient espClient;
PubSubClient client(espClient);

boolean mqtt_reconnect() {
  if(client.connect(hostname, mqttUser, mqttPassword, mqtt_topic_status, 2, true, "offline")) {
    client.publish(mqtt_topic_status, "online", true);
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
    Serial.println("Start");  //  "Начало OTA-апдейта"
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");  //  "Завершение OTA-апдейта"
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

unsigned long mTXRXData(byte cmd[], int tx_len, int rx_len, int hi_byte, int lo_byte) {
  SoftwareSerial z19Serial(MH_Z19_RX, MH_Z19_TX);
  z19Serial.begin(9600);
  while(!z19Serial.available()) {
    z19Serial.write(cmd, tx_len); 
    delay(50); 
  }
  int timeout=0;
  while(z19Serial.available() < rx_len ) {
    timeout++; 
    if(timeout > 10) {
      while(z19Serial.available()) {
        z19Serial.read();
        break;
      }
    } 
    delay(50); 
  } 
  for (int i=0; i < rx_len; i++) { 
    rpl_z19[i] = z19Serial.read(); 
  }  
  z19Serial.end();

  int high = rpl_z19[hi_byte];
  int low = rpl_z19[lo_byte];
  unsigned long val = high*256 + low;

  return val;
}  

void co2_measure() {
  z19_co2 = mTXRXData(cmd_z19, sizeof(cmd_z19)/sizeof((cmd_z19)[0]), sizeof(rpl_z19)/sizeof((rpl_z19)[0]), CO2_HBYTE, CO2_LBYTE);
  
  if (!z19_co2_mean) z19_co2_mean = z19_co2;
  z19_co2_mean = z19_co2_mean - smoothing_factor*(z19_co2_mean - z19_co2);
  
  if (!z19_co2_mean2) z19_co2_mean2 = z19_co2;
  z19_co2_mean2 = z19_co2_mean2 - smoothing_factor2*(z19_co2_mean2 - z19_co2);

  dataDoc["current"]  = z19_co2;
  dataDoc["mean"]     = z19_co2_mean;
  dataDoc["mean2"]    = z19_co2_mean2;
  return;
}

void zero_calibration() {
  int z19_zero = mTXRXData(abc_z19, sizeof(abc_z19)/sizeof((abc_z19)[0]), sizeof(abc_z19)/sizeof((abc_z19)[0]), CO2_HBYTE, CO2_LBYTE);
  dataDoc["abc"] = z19_zero;
  return;
}

void setup() {
  Serial.begin(115200);
  delay(10);
  
  strcpy(mqtt_topic_status, mqtt_topic_status_base);
  strcat(mqtt_topic_status, hostname);

  strcpy(mqtt_topic_data, mqtt_topic_data_base);
  strcat(mqtt_topic_data, hostname);

  if(WiFi.status() != WL_CONNECTED) {
    wifi_reconnect();
  }

  client.setServer(mqttServer, mqttPort);

  mqtt_reconnect();
  zero_calibration();
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
    co2_measure();
    char buffer[256];
    memset(buffer, 0, sizeof(buffer));
    size_t n = serializeJson(dataDoc, buffer);
    client.publish(mqtt_topic_data, buffer, n);
    lastCo2Measured = co2_time;
  }

}
