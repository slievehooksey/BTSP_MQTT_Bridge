#include <Arduino.h>
#include <MQTT.h>
#include <IotWebConf.h>
#include <IotWebConfUsing.h> // This loads aliases for easier class names.
#include <IotWebConfESP32HTTPUpdateServer.h>
#include <BluetoothSerial.h>
#include <Regexp.h>
#include <ArduinoJSON.h>
#include <AM4000.h>

static const char* TAG = "BTMQTTBridge";

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "AmbrogioBTSerialBridge";

// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "letmein";

#define STRING_LEN 128

// -- Configuration specific key. The value should be modified if config structure was changed.
#define CONFIG_VERSION "mqt1"

// -- Status indicator pin.
//      First it will light up (kept LOW), on Wifi connection it will blink,
//      when connected to the Wifi it will turn off (kept HIGH).
#define STATUS_PIN LED_BUILTIN

// -- Method declarations.
void handleRoot();
void mqttMessageReceived(MQTTClient *client, char topic[], char bytes[], int length);
bool connectMqtt();
bool connectMqttOptions();
// -- Callback methods.
void wifiConnected();
void configSaved();
bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper);

void printMsg(char* msg, uint8_t len, uint8_t offset1 = 0, uint8_t offset2 = 0);

// -- Bluetooth callbacks
void btDataAvailable(const uint8_t *buffer, size_t size);
void writeToBluetoothSerial(const uint8_t *buffer, size_t size);
void handleBtDeviceFound(BTAdvertisedDevice *device);

// Mower notification callbacks
void handleInfoMessage(AM4000_Info);
void handleStateMessage(AM4000_State);
void handleMessageMessage(AM4000_Message);
void handleFailureMessage(AM4000_Failure);
void handleCommandResponse(AM4000_Commands commandType, uint8_t profileIndex, uint8_t dayIndex, uint8_t areaIndex);

void mqttLog(String message);

DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
WiFiClient net;
MQTTClient mqttClient(256); //128 is default but payloads can be up to 256
BluetoothSerial SerialBT;
AM4000 mower;

char mqttServerValue[STRING_LEN];
char mqttUserNameValue[STRING_LEN];
char mqttUserPasswordValue[STRING_LEN];
char mqttPrefixValue[STRING_LEN];

char mowerMacAddressValue[18];
char mowerConnectValue[STRING_LEN];

String mqttTopic;
String mqttTopicSerialOut;
String mqttTopicSerialIn;
String mqttTopicJSONIn;
String mqttTopicJSONOut;
String mqttTopicLog;

uint8_t mowerAddress[6]; //replace with entry from config
bool btConnected = false; 
long lastBTConnectAttempt; //store the last time a BT connection attempt was made. Used to avoid retrying too often.
String scanOutput = "No scan output. If there is a valid MAC address set, scanning will not occur."; //placeholder for Bluetooth scann output

IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);

IotWebConfParameterGroup mqttGroup = IotWebConfParameterGroup("mqtt", "MQTT configuration");
IotWebConfTextParameter mqttServerParam = IotWebConfTextParameter("MQTT server", "mqttServer", mqttServerValue, STRING_LEN);
IotWebConfTextParameter mqttUserNameParam = IotWebConfTextParameter("MQTT user", "mqttUser", mqttUserNameValue, STRING_LEN);
IotWebConfPasswordParameter mqttUserPasswordParam = IotWebConfPasswordParameter("MQTT password", "mqttPass", mqttUserPasswordValue, STRING_LEN);
IotWebConfTextParameter mqttPrefixParam = IotWebConfTextParameter("MQTT topic prefix (default 'ambrogio')", "mqttPrefix", mqttPrefixValue, STRING_LEN, "ambrogio");

IotWebConfParameterGroup mowerGroup = IotWebConfParameterGroup("mower", "Mower configuration");
IotWebConfTextParameter mowerMacAddressParam = IotWebConfTextParameter("Mower MAC Address", "mowerMac", mowerMacAddressValue, 18);
IotWebConfCheckboxParameter mowerConnectParam = IotWebConfCheckboxParameter("Connect to Mower?", "mowerConnect", mowerConnectValue, STRING_LEN, true);

bool needMqttConnect = false;
bool needReset = false;
int pinState = HIGH;
unsigned long lastReport = 0;
unsigned long lastMqttConnectionAttempt = 0;
unsigned long lastSentToBt = 0;

void setup() 
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("Starting up...");

  mqttGroup.addItem(&mqttServerParam);
  mqttGroup.addItem(&mqttUserNameParam);
  mqttGroup.addItem(&mqttUserPasswordParam);
  mqttGroup.addItem(&mqttPrefixParam);

  mowerGroup.addItem(&mowerMacAddressParam);
  mowerGroup.addItem(&mowerConnectParam);

  iotWebConf.setStatusPin(STATUS_PIN);
  iotWebConf.addParameterGroup(&mqttGroup);
  iotWebConf.addParameterGroup(&mowerGroup);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.setWifiConnectionCallback(&wifiConnected);
  iotWebConf.skipApStartup();

  // -- Initializing the configuration.
  bool validConfig = iotWebConf.init();
  if (!validConfig)
  {
    mqttServerValue[0] = '\0';
    mqttUserNameValue[0] = '\0';
    mqttUserPasswordValue[0] = '\0';
  }

  mqttTopic = String(mqttPrefixValue) + "/" + String(iotWebConf.getThingName());
  mqttTopicSerialIn = mqttTopic + "/serialin";
  mqttTopicSerialOut = mqttTopic + "/serialout";
  mqttTopicJSONIn = mqttTopic + "/command";
  mqttTopicJSONOut = mqttTopic + "/state";
  mqttTopicLog = mqttTopic + "/log";

   // -- Define how to handle updateServer calls.
  iotWebConf.setupUpdateServer(
    [](const char* updatePath) { httpUpdater.setup(&server, updatePath); },
    [](const char* userName, char* password) { httpUpdater.updateCredentials(userName, password); });

  // -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", []{ iotWebConf.handleConfig(); });
  server.onNotFound([](){ iotWebConf.handleNotFound(); });

  mqttClient.setKeepAlive(30000);//the default of 10000 is too short when the bluetooth connect is also 10 seconds
  mqttClient.begin(mqttServerValue, net);
  mqttClient.onMessageAdvanced(mqttMessageReceived);


  //set callbacks to pass data back and forth between the library and the bluetooth mower connection.
  mower.setSerialDataOutputCallack(writeToBluetoothSerial);
  SerialBT.onData(btDataAvailable);

  //set callbacks for all the message types coming from the mower (via the library)
  mower.setNotificationInfoCallback(handleInfoMessage);
  mower.setNotificationStateCallback(handleStateMessage);
  mower.setNotificationFailureCallback(handleFailureMessage);
  mower.setNotificationMessageCallback(handleMessageMessage);
  mower.setNotificationCommandResponseCallback(handleCommandResponse);


  SerialBT.begin("Ambrogio BTSerial Bridge", true);

  Serial.println("Ready.");
}

void loop() 
{
  // -- doLoop should be called as frequently as possible.
  iotWebConf.doLoop();
  mqttClient.loop();
  
  if (needMqttConnect)
  {
    if (connectMqtt())
    {
      needMqttConnect = false;
    }
  }
  else if ((iotWebConf.getState() == iotwebconf::OnLine) && (!mqttClient .connected()))
  {
    Serial.println("MQTT reconnect");
    connectMqtt();
  }

  if (needReset)
  {
    Serial.println("Rebooting after 1 second.");
    iotWebConf.delay(1000);
    ESP.restart();
  }

  if(btConnected && !SerialBT.connected()){ //BT Serial connection has become disconnected
  Serial.println("BT Serial connection closed");
  mqttLog("Bluetooth connection was unexpectedly closed");
  btConnected = SerialBT.connected();
  }

if(mowerConnectParam.isChecked() && !btConnected && mqttClient.connected() && (lastBTConnectAttempt + 30000 < millis() || lastBTConnectAttempt == 0)){ //if we are wanting to be connected, and aren't already
  
      MatchState ms;
      ms.Target(mowerMacAddressValue);
      char result = ms.Match("^[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]$");
    if (result == REGEXP_MATCHED)
    { 
      String msg = "Attempting to connect to mower by MAC: " + String(mowerMacAddressValue);
      Serial.print(msg);
      mqttLog(msg);
      

      sscanf(mowerMacAddressValue, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mowerAddress[0], &mowerAddress[1], &mowerAddress[2], &mowerAddress[3], &mowerAddress[4], &mowerAddress[5]);
        char* ptr; //start and end pointer for strtol

        mowerAddress[0] = strtol(mowerMacAddressValue, &ptr, HEX );
        for( uint8_t i = 1; i < 6; i++ )
        {
          mowerAddress[i] = strtol(ptr+1, &ptr, HEX );
        }
      SerialBT.connect(mowerAddress);
      lastBTConnectAttempt = millis();
    }
    else if(result == REGEXP_NOMATCH){
      Serial.println("Not a valid MAC address");
      scanOutput = "Scanning....<br><br>";
      SerialBT.discoverAsync(handleBtDeviceFound, 25000);
      lastBTConnectAttempt = millis();
    }
    else{
      Serial.println("Regex parsing error");
    }
  
  if(SerialBT.connected()){//connection has been made successfully
    Serial.println("Connected Succesfully!");
    mqttLog("Successfully connected to Bluetooth Device");
    btConnected = SerialBT.connected();
  }
}
else if(!mowerConnectParam.isChecked() && btConnected){//we are connected but should be disconnected
    SerialBT.disconnect();
    mqttLog("Bluetooth disconnected");
}
  
}

/**
 * Handle web requests to "/" path.
 */
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>" + String(thingName) + "</title></head><body>Bluetooth Serial Port to MQTT Bridge";
  s += "<ul>";
  s += "<li>MQTT server: ";
  s += mqttServerValue;
  s += "</li>";
  s += "<li>MQTT Topic: ";
  s += mqttTopic;
  s += "</li>";
  s += "</ul>";
  s += "Go to <a href='config'>configure page</a> to change values.";
  s += "<br><br><h3>Bluetooth Scan Output:</h3>";
  s += scanOutput;
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

void wifiConnected()
{
  needMqttConnect = true;
}

void configSaved()
{
  Serial.println("Configuration was updated.");
  needReset = true;
}

bool formValidator(iotwebconf::WebRequestWrapper* webRequestWrapper)
{
  Serial.println("Validating form.");
  bool valid = true;

  int l = webRequestWrapper->arg(mqttServerParam.getId()).length();
  if (l < 3)
  {
    mqttServerParam.errorMessage = "Please provide at least 3 characters!";
    valid = false;
  }

  return valid;
}

bool connectMqtt() {
  unsigned long now = millis();
  if (1000 > now - lastMqttConnectionAttempt)
  {
    // Do not repeat within 1 sec.
    return false;
  }
  Serial.println("Connecting to MQTT server...");
  if (!connectMqttOptions()) {
    lastMqttConnectionAttempt = now;
    return false;
  }
  Serial.println("Connected!");

  mqttClient.subscribe(mqttTopicSerialIn);
  mqttClient.subscribe(mqttTopicJSONIn);
  return true;
}

bool connectMqttOptions()
{
  bool result;
  if (mqttUserPasswordValue[0] != '\0')
  {
    result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue, mqttUserPasswordValue);
  }
  else if (mqttUserNameValue[0] != '\0')
  {
    result = mqttClient.connect(iotWebConf.getThingName(), mqttUserNameValue);
  }
  else
  {
    result = mqttClient.connect(iotWebConf.getThingName());
  }
  return result;
}

void mqttMessageReceived(MQTTClient *client, char topic[], char bytes[], int length)
{
  //should only ever get messages on this topic as that's all that is subscribed, but check anyway
  if(strcmp(topic, mqttTopicSerialIn.c_str()) == 0)
  {
    uint8_t data[length];
    memcpy(data,bytes,length);
    SerialBT.write(data,length);
    }
    else if(strcmp(topic, mqttTopicJSONIn.c_str()) == 0){
      JsonDocument command;
      DeserializationError error = deserializeJson(command, bytes, length);

      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return;
      }
      if(command["cmnd"] == "play" ){
        mower.play();
      }
      if(command["cmnd"] == "pause" ){
        mower.pause();
      }
      if(command["cmnd"] == "home" ){
        mower.goHome();
      }
    }
}

void btDataAvailable(const uint8_t *buffer, size_t size){
  char message[size];
  memcpy(message,buffer,size);
  int msgPos = 0;
  int frameStart = 0;
  //sometimes 2 frames of data can come in together, so split them up
  for(size_t i = 0; i < size; i++ ){
    if(buffer[i] == 0x03){
      uint8_t length = i - frameStart + 1;
      uint8_t packet[length];
      memcpy(packet,buffer+frameStart,length);
      //mqttClient.publish(mqttTopicSerialOut.c_str(),packet,length);
      ESP_LOGD(TAG, "Sending data received from mower to library" );
      mower.serialInput(packet,length);
      frameStart = i + 1;
    }
  } 
}

void writeToBluetoothSerial(const uint8_t *buffer, size_t size){
  if(SerialBT.connected()){
  SerialBT.write(buffer,size);
  lastSentToBt = millis();
  }
}

void printMsg(char* msg, uint8_t len, uint8_t offset1, uint8_t offset2) {
  for (int z=offset1; z<len-offset2; z++) {
    if (msg[z] < 16) Serial.print("0");
    Serial.print((char)msg[z], HEX);
    Serial.print (" ");
  }
  Serial.println();
}

void handleBtDeviceFound(BTAdvertisedDevice *device){
      scanOutput += "<br>";
      scanOutput += device->getAddress().toString(true);
      scanOutput += " - ";
      scanOutput += device->getName().c_str();
}

void handleInfoMessage(AM4000_Info info){
  JsonDocument doc;
  doc["typ"] = "info";
  doc["volt"] = info._batteryVoltage;
  doc["batt"] = info._batteryPercent;
  doc["curr"] = info._current;
  char buffer[256];
  size_t n = serializeJson(doc,buffer);
  mqttClient.publish(mqttTopicJSONOut.c_str(),buffer, n);
}

void handleStateMessage(AM4000_State state){
  JsonDocument doc;
  doc["type"] = "stat";
  doc["stat"] = stateToString[state];
  char buffer[256];
  size_t n = serializeJson(doc,buffer);
  mqttClient.publish(mqttTopicJSONOut.c_str(),buffer, n);
}

void handleMessageMessage(AM4000_Message message){
  JsonDocument doc;
  doc["type"] = "mesg";
  doc["mesg"] = messageToString[message];
  char buffer[256];
  size_t n = serializeJson(doc,buffer);
  mqttClient.publish(mqttTopicJSONOut.c_str(),buffer, n);
}

void handleFailureMessage(AM4000_Failure failure){
  JsonDocument doc;
  doc["type"] = "fail";
  doc["fail"] = failureToString[failure];
  char buffer[256];
  size_t n = serializeJson(doc,buffer);
  mqttClient.publish(mqttTopicJSONOut.c_str(),buffer, n);
}

void handleCommandResponse(AM4000_Commands commandType, uint8_t profileIndex, uint8_t dayIndex, uint8_t areaIndex){

}

void mqttLog(String message){
  if(mqttClient.connected()){
    mqttClient.publish(mqttTopicLog.c_str(),message);
  }
}