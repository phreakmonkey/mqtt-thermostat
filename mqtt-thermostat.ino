
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PubSubClient.h>

#include <Arduino.h>
#include <U8g2lib.h>

// Global Settings:
#define EEPROM_VERSION 0x02
char Topic[40] = "Thermostat";
char mqtt_server[64] = "mqtt.mydomain.com";
unsigned int mqtt_port = 1883;
const char* client_name = "MQTTCLIENTNAME";
const char* client_password = "MQTTPASSWORD";
// All temps are in hundredths of a degree C.  E.g. 722 = 7.22 C.
int16_t settemp = 722;  
int16_t threshold = 50; // Turn off heat when temp rises this much
int16_t mintemp = 722;  // Don't accept lower than this
int16_t maxtemp = 2944; // Don't accept higher than this
uint16_t teleperiod = 30000;  // How often to report telem (in ms)

// Amount to adjust temperature by in hundredths of degrees C.
#define CALIB 0

// Do not use pin 6 for OneWire, it cannot be held high at boot
#define ONE_WIRE_BUS 2
#define OVERRIDE 3 
#define HEAT 5

// U8G2 Constructor
U8G2_SSD1306_128X64_NONAME_F_4W_SW_SPI u8g2(U8G2_R0, /* clock=*/ 7, /* data=*/ 8, /* cs=*/ U8X8_PIN_NONE, /* dc=*/ 10, /* reset=*/ 9);

// Globals
WiFiClient espClient;
PubSubClient client(espClient);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
unsigned long counter1s = 0;
unsigned long counterTele = 0;
int tempF = 0;
int tempC = 0;
char spin = '.';
boolean ostate = LOW;
boolean heat = LOW;
boolean lastheat = LOW;

void setup(void) 
{
  EEPROM.begin(128);
  readConfig();
  u8g2.begin();
  pinMode(ONE_WIRE_BUS, INPUT_PULLUP);
  pinMode(OVERRIDE, INPUT_PULLUP);
  pinMode(HEAT, OUTPUT);
  digitalWrite(HEAT, LOW);
  sensors.begin();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop(void)
{
  char topic[40];
  char msg[75];
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  if (millis() - counter1s > 2000) {
    counter1s = millis();
    if (tempC == 0)  tempC = processTemp();
    else tempC = (tempC * 3 + processTemp()) / 4;
    tempF = (int) (tempC * 1.8 + 3200);

    if (heat == LOW) {
      if (tempC < settemp) heat = HIGH;
    } else {
      if (tempC >= settemp + threshold) heat = LOW;
    }

    if (ostate == HIGH) heat = LOW; // Override HEAT when OVERRIDE is HIGH
    
    digitalWrite(HEAT, heat);

    if (lastheat != heat) {  // HEAT status change:
      lastheat = heat;
      sprintf(topic, "stat/%s/HEAT", Topic);
      if (heat) sprintf(msg, "ON");
      else sprintf(msg, "OFF");
      client.publish(topic, msg);
      u8g2.begin();  // Reset OLED display, 'cuz relay fucks it up
    }
    DrawTemp(tempF); // Finally, update the display
  }

  if (millis() - counterTele > teleperiod) {
    counterTele = millis();
    sprintf(topic, "tele/%s/TEMPERATURE", Topic);
    Decimate(msg, tempC);
    client.publish(topic, msg);
    sprintf(topic, "tele/%s/SETTEMP", Topic);
    Decimate(msg, settemp);
    client.publish(topic, msg);
    sprintf(topic, "tele/%s/HEAT", Topic);
    if (heat) sprintf(msg, "ON");
    else sprintf(msg, "OFF");
    client.publish(topic, msg);
    sprintf(topic, "tele/%s/DOOR", Topic);
    sprintf(msg, "%d", ostate);
    client.publish(topic, msg);
  }

  if (digitalRead(OVERRIDE) != ostate) {
    char topic[40];
    char msg[75];
    ostate = digitalRead(OVERRIDE);
    sprintf(topic, "stat/%s/DOOR", Topic);
    sprintf(msg, "%d", ostate);
    client.publish(topic, msg);
  }
}


void Decimate(char *buf, int num)
{
  uint8_t decplace;
  sprintf(buf, "%d", num);
  decplace = strlen(buf);
  buf[decplace + 1] = 0;
  buf[decplace] = buf[decplace-1];
  buf[decplace-1] = buf[decplace-2];
  buf[decplace-2] = '.';
}

int Round(int num)
{
  int ret = num / 100;
  if (num % 100 > 49) ret++;
  return ret;
}

int processTemp()
{ 
  int temp = 0;
  // call sensors.requestTemperatures() to issue a global temperature
  // request to all devices on the bus
  sensors.requestTemperatures(); // Send the command to get temperatures
  temp = (int) (sensors.getTempCByIndex(0) * 100); // Get temp F from first sensor
  temp += CALIB;
  return temp;
}

void DrawTemp(int tempF)
{
  uint8_t decplace;
  int setTemp;
  char msg[25];
  Decimate(msg, tempF);
  msg[strlen(msg)-1] = 0; // one less digit

  u8g2.clearBuffer();               // clear the internal memory
  u8g2.setFont(u8g2_font_fub25_tr); // choose a suitable font
  u8g2.drawStr(0,26, msg);  // write something to the internal memory

  u8g2.setFont(u8g2_font_fub11_tr);
  setTemp = Round(settemp * 1.8 + 3200);
  sprintf(msg, "Set: %d", setTemp);
  u8g2.drawStr(0,50, msg);
  u8g2.drawGlyph(100,55, spin);
  Spinner();
  u8g2.sendBuffer();                // transfer internal memory to the display
}

void Spinner()
{
  if (ostate) {
    switch (spin) {
      case 'd':
        spin = 'D';
        break;
      case 'D':
        spin = 'd';
        break;
      default:
        spin = 'd';
        break;
    }
    return;
  }
  if (heat) {
    switch (spin) {
      case '.':
        spin = 'o';
        break;
      case 'o':
        spin = 'O';
        break;
      case 'O':
        spin = '.';
        break;
      default:
        spin = '.';
        break;
    } 
  } else {
      switch (spin) {
        case '.':
          spin = ' ';
          break;
        case ' ':
          spin = '.';
          break;
        default:
          spin = '.';
          break;
      }
  }
}

/*** MQTT Routines ***
 *   We follow the convention used by adrenst's Sonoff-MQTT project
 *  tele/$TOPIC/#  = telemetry messages
 *  stat/$TOPIC/#  = state change messages
 *  cmnd/$TOPIC/#  = command messages
 ***/
void reconnect() {
  uint8_t retries = 3;
  char topic[40];
  sprintf(topic, "tele/%s/status", Topic);
  while ((!client.connected()) && retries > 0) {
    if (client.connect(topic, client_name, client_password)) {
      client.publish(topic, "online");
      sprintf(topic, "cmnd/%s/#", Topic);
      client.subscribe(topic);
    } else {
      retries--;
      delay(2000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  char cmdstr[75], buf[10];
  int j, k;
  sprintf(cmdstr, "cmnd/%s/SETTEMP", Topic);
  if (!strncmp(topic, cmdstr, 75)) {
    j = 0;
    for (int i=0; i < length; i++) {
      if (payload[i] != '.') {
        buf[j] = payload[i];
        j++;
      }
    }
    buf[j] = 0;
    k = atoi(buf);
    if (k < mintemp || k > maxtemp) return;
    settemp = k;
    writeTemp();
  }
}

void readConfig()
{
  /*** EEPROM Values
  * int8_t EEPROM_VERSION 0x02
  * int16_t settemp 
  * int16_t threshold     default: 10
  * int16_t mintemp
  * int16_t maxtemp
  * uint16_t teleperiod   default: 300
  * char topic[40] 
  * char mqtt_server[64] 
  * unsigned int mqtt_port = 1883;
  *****/
  int8_t eeprom_ver;
  uint16_t eeAddress = 0;
  EEPROM.get(eeAddress, eeprom_ver); eeAddress += sizeof(eeprom_ver);
  if (eeprom_ver != EEPROM_VERSION)
  {
    writeConfig();  // Initialize new EEPROM
    return;
  }
  EEPROM.get(eeAddress, settemp); eeAddress += sizeof(settemp);
  EEPROM.get(eeAddress, threshold); eeAddress += sizeof(threshold);
  EEPROM.get(eeAddress, mintemp); eeAddress += sizeof(mintemp);
  EEPROM.get(eeAddress, maxtemp); eeAddress += sizeof(maxtemp);
  EEPROM.get(eeAddress, teleperiod); eeAddress += sizeof(teleperiod);
  EEPROM.get(eeAddress, Topic); eeAddress += sizeof(Topic);
  EEPROM.get(eeAddress, mqtt_server); eeAddress += sizeof(mqtt_server);
  EEPROM.get(eeAddress, mqtt_port); eeAddress += sizeof(mqtt_port);
  return;
}

void writeConfig()
{
  int8_t eeprom_ver = EEPROM_VERSION;
  uint16_t eeAddress = 0;
  EEPROM.put(eeAddress, eeprom_ver); eeAddress += sizeof(eeprom_ver);
  EEPROM.put(eeAddress, settemp); eeAddress += sizeof(settemp);
  EEPROM.put(eeAddress, threshold); eeAddress += sizeof(threshold);
  EEPROM.put(eeAddress, mintemp); eeAddress += sizeof(mintemp);
  EEPROM.put(eeAddress, maxtemp); eeAddress += sizeof(maxtemp);
  EEPROM.put(eeAddress, teleperiod); eeAddress += sizeof(teleperiod);
  EEPROM.put(eeAddress, Topic); eeAddress += sizeof(Topic);
  EEPROM.put(eeAddress, mqtt_server); eeAddress += sizeof(mqtt_server);
  EEPROM.put(eeAddress, mqtt_port); eeAddress += sizeof(mqtt_port);
  EEPROM.commit();
  return;
}

void writeTemp()
{
  uint16_t eeAddress = 0;
  eeAddress += sizeof(int8_t);  // Skip eeprom_ver
  EEPROM.put(eeAddress, settemp);
  EEPROM.commit();
  return;
}
