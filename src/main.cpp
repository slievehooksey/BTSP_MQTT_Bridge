#include <Arduino.h>
#include <BluetoothSerial.h>
#include <Regexp.h>
#include <ArduinoJson.h>
#include <AM4000.h>
#include <Preferences.h>
#include <WiFiManager.h> 
#include <MQTT.h>
//#include <Meshtastic.h>
//#include <ArduinoOTA.h>

Preferences prefs;
WiFiClient net;

static const char* TAG = "BTSerialBridge";

char mower_name[40];
char mqtt_server[40];
int mqtt_port;
char mqtt_username[40];
char mqtt_password[40];
char mqtt_topic[40] = "ambrogio";
char mower_mac[18];
char checkbox_btConnect[2];
String customHtml_checkbox = "type=\"checkbox\"";
int mesh_uplink_id; //this is the id of the node that has the connection to the MQTT server. We will send our messages directly to this node
int mesh_channel_id; //the ID of the 'mqtt' channel that will be used to send and receive messages on.

long btScanStarted = 0;
bool resultsReturned = true;
uint8_t btDevicesDiscovered;
String scanResults;

bool meshAvailable;
bool meshNotYetConnected = true;
//void handleMeshMessage(uint32_t from, uint32_t to,  uint8_t channel, const char* text);
//void connected_callback(mt_node_t *node, mt_nr_progress_t progress);

void printMsg(char* msg, uint8_t len, uint8_t offset1 = 0, uint8_t offset2 = 0);
bool validateMAC(char *mac);
void log(String msg);

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
void handleSettingsProfile(AM4000_SettingsProfile);
void handleSettingsMisc(AM4000_SettingsMisc);

void outputMessage(String msg, bool sendMQTT, bool sendMesh);
void processCommand(JsonDocument command);

//mqtt functions
void mqttMessageReceived(MQTTClient *client, char topic[], char bytes[], int length);
void connectMqtt();
bool connectMqttOptions();

void bindServerCallback();
void handleRoute();
void saveParamsCallback ();
void preOTA();

MQTTClient mqttClient(256); //128 is default but payloads can be up to 256

String mqttBase;
String mqttTopicJSONIn;
String mqttTopicJSONOut;

bool needMqttConnect = true;

WiFiManager wm;
WiFiManagerParameter *custom_mower_mac;
WiFiManagerParameter *custom_btConnect;
WiFiManagerParameter *custom_mower_name;
WiFiManagerParameter *custom_mqtt_server;
WiFiManagerParameter *custom_mqtt_port;
WiFiManagerParameter *custom_mqtt_username;
WiFiManagerParameter *custom_mqtt_password;
WiFiManagerParameter *custom_mqtt_topic;
WiFiManagerParameter *custom_mesh_uplink_id;
WiFiManagerParameter *custom_mesh_channel_id;


BluetoothSerial SerialBT;
AM4000 mower;
AM4000_State previousState;
AM4000_Message previousMessage;

uint8_t mowerAddress[6]; //replace with entry from config
bool btConnect = false; //Should we be trying to connect to the mower via bluetooth. 
bool btConnected = false; //has a bluetooth connection been established
long lastBTConnectAttempt; //store the last time a BT connection attempt was made. Used to avoid retrying too often.
long lastInfoMessageSent = 0; //If sending over Mesh, we don't need to send every second.

bool needReset = false;
bool scan = false;

unsigned long lastReport = 0;
unsigned long lastMqttConnectionAttempt = 0;
unsigned long lastSentToBt = 0;



void setup() 
{
  Serial.begin(115200);
  Serial.println();
  Serial.println("Starting up...");
  //mt_set_debug(true);
  //randomSeed(micros());
  //mt_serial_init(16, 17, 115200);
  //set_text_message_callback(handleMeshMessage);
  //mt_request_node_report(connected_callback);

  WiFi.mode(WIFI_STA);

  prefs.begin("ambrogio");
  
  
  //set callbacks to pass data back and forth between the library and the bluetooth mower connection.
  mower.setSerialDataOutputCallack(writeToBluetoothSerial);
  SerialBT.onData(btDataAvailable);

  //set callbacks for all the message types coming from the mower (via the library)
  mower.setNotificationInfoCallback(handleInfoMessage);
  mower.setNotificationStateCallback(handleStateMessage);
  mower.setNotificationFailureCallback(handleFailureMessage);
  mower.setNotificationMessageCallback(handleMessageMessage);
  mower.setNotificationCommandResponseCallback(handleCommandResponse);

  prefs.getBytes("mowerAddress",mowerAddress,6);
  sprintf(mower_mac,  "%02X:%02X:%02X:%02X:%02X:%02X", mowerAddress[0], mowerAddress[1], mowerAddress[2], mowerAddress[3], mowerAddress[4],mowerAddress[5]);
  btConnect = prefs.getBool("btConnect");
  strcpy(mower_name, prefs.getString("mower_name", "mower").c_str());

  strcpy(mqtt_server, prefs.getString("mqtt_server", "").c_str());
  mqtt_port = prefs.getInt("mqtt_port", 1883);
  strcpy(mqtt_username, prefs.getString("mqtt_username", "").c_str());
  strcpy(mqtt_password, prefs.getString("mqtt_password", "").c_str());
  strcpy(mqtt_topic, prefs.getString("mqtt_topic", "ambrogio").c_str());

  mesh_uplink_id = prefs.getInt("mesh_uplink_id",0);
  mesh_channel_id = prefs.getInt("mesh_channel_id",0);

  custom_mower_mac = new WiFiManagerParameter("mac", "Mower MAC Address", mower_mac, 17);
    if (btConnect) {
      customHtml_checkbox += " checked=\"true\"";
    }
  custom_btConnect = new WiFiManagerParameter("btConnect", "Connect to Mower", "T", 2, customHtml_checkbox.c_str());
  custom_mower_name = new WiFiManagerParameter("mower_name", "Mower Name", mower_name, 40);
  custom_mqtt_server = new WiFiManagerParameter("server", "MQTT Server", mqtt_server, 40);
  custom_mqtt_port = new WiFiManagerParameter("port", "MQTT Port", String(mqtt_port).c_str(), 6);
  custom_mqtt_username = new WiFiManagerParameter("username", "MQTT Username", mqtt_username, 40);
  custom_mqtt_password = new WiFiManagerParameter("password", "MQTT Password", mqtt_password, 40, "type='password'");
  custom_mqtt_topic = new WiFiManagerParameter("topic", "MQTT Topic", mqtt_topic, 40);

  custom_mesh_uplink_id = new WiFiManagerParameter("mesh_uplink_id","Meshtastic Uplink Node Number", String(mesh_uplink_id).c_str(), 12);
  custom_mesh_channel_id = new WiFiManagerParameter("mesh_channel_id","Meshtastic Channel ID", String(mesh_channel_id).c_str(), 1);

  wm.addParameter(custom_mower_mac);
  wm.addParameter(custom_btConnect);

  wm.addParameter(custom_mower_name);
  wm.addParameter(custom_mqtt_server);
  wm.addParameter(custom_mqtt_port);
  wm.addParameter(custom_mqtt_username);
  wm.addParameter(custom_mqtt_password);
  wm.addParameter(custom_mqtt_topic);

  wm.addParameter(custom_mesh_uplink_id);
  wm.addParameter(custom_mesh_channel_id);

  wm.setCustomHeadElement("<style>body{max-width:500px;margin:auto}</style>");
  const char* menuhtml = "<form action='/btscan' method='get'><button>Bluetooth Scan</button></form><br/>\n";
  wm.setCustomMenuHTML(menuhtml);
  const char* menu[] = {"wifi","wifinoscan","info","param","close","sep","erase","restart","exit","update","sep","custom"};
  wm.setMenu(menu,12); // custom menu array must provide length

  wm.setSaveParamsCallback(saveParamsCallback);
  wm.setWebServerCallback(bindServerCallback);
  wm.setPreOtaUpdateCallback(preOTA);

  wm.setConfigPortalBlocking(true);
  if(wm.getWiFiIsSaved()){
  wm.setConfigPortalTimeout(20);
  }
  wm.setAPClientCheck(true);
  wm.startConfigPortal();


  if(wm.getWiFiIsSaved()){
  //  wm.autoConnect();
  //} else {
    WiFi.setAutoReconnect(true);
    WiFi.begin();
    wm.setConfigPortalBlocking(false);
    wm.startWebPortal();
  }

  mqttBase = String(mqtt_topic) + "/" + String(mower_name) ;
  mqttTopicJSONIn = mqttBase + "/command";
  mqttTopicJSONOut = mqttBase + "/state";

  mqttClient.setKeepAlive(30000);//the default of 10000 is too short when the bluetooth connect is also 10 seconds
  mqttClient.begin(mqtt_server, net);
  mqttClient.onMessageAdvanced(mqttMessageReceived);


/*   ArduinoOTA
    .onStart([]() {
      needMqttConnect = false;
      mqttClient.disconnect();
      btConnect = false;
      SerialBT.disconnect();
      SerialBT.end();
      wm.stopWebPortal();
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else {  // U_SPIFFS
        type = "filesystem";
      }

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) {
        Serial.println("Auth Failed");
      } else if (error == OTA_BEGIN_ERROR) {
        Serial.println("Begin Failed");
      } else if (error == OTA_CONNECT_ERROR) {
        Serial.println("Connect Failed");
      } else if (error == OTA_RECEIVE_ERROR) {
        Serial.println("Receive Failed");
      } else if (error == OTA_END_ERROR) {
        Serial.println("End Failed");
      }
    }); */

  //ArduinoOTA.begin(); 

  SerialBT.begin("Ambrogio BTSerial Bridge", true);

  Serial.println("Startup complete.3");
}


void loop() 
{
  //ArduinoOTA.handle();
  wm.process();
  mqttClient.loop();
  //meshAvailable = mt_loop(millis());
  
  // if there is an MQTT server configured, it is more than 1 second since we last tried AND wifi is connected
  if (String(mqtt_server).length() > 0 && (1000 < millis() - lastMqttConnectionAttempt) && WiFi.isConnected() && !mqttClient.connected() && needMqttConnect)
  {
    Serial.println("Going to try and connect to MQTT");
    connectMqtt();
    lastMqttConnectionAttempt = millis();
  }

  if(btConnected && !SerialBT.connected()){ //BT Serial connection has become disconnected
  Serial.println("BT Serial connection closed");
  btConnected = SerialBT.connected();
  }


if(btConnect && !btConnected && (lastBTConnectAttempt + 30000 < millis() || lastBTConnectAttempt == 0)){ //if we are wanting to be connected, and aren't already
  String msg = "Attempting to connect to mower by MAC: " + String(mower_mac);
  log(msg);
      SerialBT.connect(mowerAddress);
      lastBTConnectAttempt = millis();

  
  if(SerialBT.connected()){//connection has been made successfully
    Serial.println("Connected to bluetooth device succesfully!");
    btConnected = SerialBT.connected();
    //mower.init();
  }
}
else if(!btConnect && btConnected){//we are connected but should be disconnected
    Serial.println("Disconnected from Bluetooth Serial");
    SerialBT.disconnect();
}

if(Serial.available() > 0){
  JsonDocument command;
  DeserializationError error = deserializeJson(command, Serial);
  Serial.println(command["cmnd"].as<String>());
      if (error) {
        //Serial.print(F("deserializeJson() failed: "));
        //Serial.println(error.f_str());
        return;
      }
    processCommand(command);
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
      //ESP_LOGD(TAG, "Sending data received from mower to library" );
      mower.serialInput(packet,length);
      frameStart = i + 1;
    }
  } 
}

void writeToBluetoothSerial(const uint8_t *buffer, size_t size){
  while(millis()-lastSentToBt < 50){ //delay a while to avoid overloading the send buffer
    delay(10);
  }
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

bool validateMAC(char *mac){
  MatchState ms;
  ms.Target(mac);
  char result = ms.Match("^[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]:[0-9A-Fa-f][0-9A-Fa-f]$");
    if (result == REGEXP_MATCHED)
    { 
      Serial.println("MAC address is valid");
      return true;
    }
   else if(result == REGEXP_NOMATCH){
      Serial.println("Not a valid MAC address");
      return false;
    }
    else{
      Serial.println("Regex parsing error");
      return false;
    }
}

void handleInfoMessage(AM4000_Info info){
  JsonDocument doc;
  doc["type"] = "info";
  doc["volt"] = info._batteryVoltage;
  doc["batt"] = info._batteryPercent;
  doc["curr"] = info._current;
  String buffer;
  size_t n = serializeJson(doc,buffer);
  bool sendMesh = false;
   if(millis() > lastInfoMessageSent + 15000){
    lastInfoMessageSent = millis();
    sendMesh = true;
  }
  outputMessage(buffer,true,sendMesh);
}

void handleStateMessage(AM4000_State state){
  if(state != previousState ){
  JsonDocument doc;
  doc["type"] = "stat";
  doc["stat"] = stateToString[state];
  char buffer[256];
  size_t n = serializeJson(doc,buffer);
  outputMessage(buffer,true,true);
  previousState = state;
  }
}

void handleMessageMessage(AM4000_Message message){
  if(message != previousMessage){
  JsonDocument doc;
  doc["type"] = "mesg";
  doc["mesg"] = messageToString[message];
  char buffer[256];
  size_t n = serializeJson(doc,buffer);
  outputMessage(buffer,true,true);
  previousMessage = message;
  }
}

void handleFailureMessage(AM4000_Failure failure){
  JsonDocument doc;
  doc["type"] = "fail";
  doc["fail"] = failureToString[failure];
  char buffer[256];
  size_t n = serializeJson(doc,buffer);
  outputMessage(buffer,true,true);
}

void handleCommandResponse(AM4000_Commands commandType, uint8_t profileIndex, uint8_t dayIndex, uint8_t areaIndex){
  //Serial.print("handling command response: ");
  Serial.println(commandType);
  JsonDocument doc;
if(commandType == COMMS_SCHEDULE_SETTINGS_GET){
  //Serial.println("command response is a schedule");
  doc["type"] = "sche";
  doc["pid"] = profileIndex;
  doc["did"] = dayIndex;
  doc["rain"] = mower.getSettingsProfile(profileIndex)._settingsRobot._rainSensor;
  JsonArray cycles = doc["cycles"].to<JsonArray>();
  for(uint8_t i=0;i<=3;i++){
    JsonArray cycle = cycles.add<JsonArray>();
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._startHour);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._startMinute);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._stopHour);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._stopMinute);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._area1 ? 1 : 0);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._area2 ? 1 : 0);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._area3 ? 1 : 0);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._area4 ? 1 : 0);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._area5 ? 1 : 0);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._area6 ? 1 : 0);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._area7 ? 1 : 0);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._area8 ? 1 : 0);
    cycle.add(mower.getSettingsProfile(profileIndex)._settingsSchedules[dayIndex]._cycles[i]._borderCut ? 1 : 0);
}
}
if(commandType == COMMS_AREA_SETTINGS_GET){
    //Serial.println("command response is an area");

    doc["type"] = "area";
    doc["aid"] = areaIndex;
    doc["dim"] = mower.getSettingsArea(areaIndex)._dimension;
    doc["dist"] = mower.getSettingsArea(areaIndex)._distance;
    doc["flag"] = mower.getSettingsArea(areaIndex)._flags;
    doc["wire"] = mower.getSettingsArea(areaIndex)._onWire;
    
  }
  if(commandType == COMMS_ROBOT_SETTINGS_GET){
    //Serial.println("command response is robot settings");
    doc["type"] = "robot";
    doc["pid"] = profileIndex;
    doc["bladeheight"] = mower.getSettingsRobot(profileIndex)._bladeHeight;
    doc["grass"] = mower.getSettingsRobot(profileIndex)._grassSensibility;
    doc["onwire"] = mower.getSettingsRobot(profileIndex)._onWire;
    doc["rain"] = mower.getSettingsRobot(profileIndex)._rainSensor;
  }
  if(commandType == COMMS_GET_CONFIG){
    doc["type"] = "config";
    doc["model"] = mower.getModelType();
  }
  if(commandType == COMMS_MISC_SETTINGS_GET){
    doc["type"] = "misc";
    doc["pid"] = mower.getActiveProfile();
    doc["manual"] = mower.getManualMode();
  }
  if(commandType == COMMS_FACTORY_SETTINGS_GET){
    doc["type"] = "factory";
    doc["serialnum"] = mower.getSerialNumber();
  }
  char buffer[256];
    size_t n = serializeJson(doc,buffer);
  outputMessage(buffer,true,true);
}


void handleSettingsProfile(uint8_t profile){

}
void handleSettingsMisc(AM4000_SettingsMisc misc){

}

void saveParamsCallback () {

  strcpy(mower_mac, custom_mower_mac->getValue());
  if(validateMAC(mower_mac)){   
    sscanf(mower_mac, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mowerAddress[0], &mowerAddress[1], &mowerAddress[2], &mowerAddress[3], &mowerAddress[4], &mowerAddress[5]);
    prefs.putBytes("mowerAddress",mowerAddress,6);
  }
  btConnect = strncmp(custom_btConnect->getValue(), "T", 1) == 0;
  prefs.putBool("btConnect", btConnect);
  strcpy(mower_name, custom_mower_name->getValue());

  strcpy(mqtt_server, custom_mqtt_server->getValue());
  mqtt_port = atoi(custom_mqtt_port->getValue());
  strcpy(mqtt_username, custom_mqtt_username->getValue());
  strcpy(mqtt_password, custom_mqtt_password->getValue());
  strcpy(mqtt_topic, custom_mqtt_topic->getValue());

  mesh_uplink_id = atoi(custom_mesh_uplink_id->getValue());
  mesh_channel_id = atoi(custom_mesh_channel_id->getValue());

  prefs.putString("mower_name", mower_name);
  prefs.putString("mqtt_server", mqtt_server);
  prefs.putInt("mqtt_port", mqtt_port);
  prefs.putString("mqtt_username", mqtt_username);
  prefs.putString("mqtt_password", mqtt_password);
  prefs.putString("mqtt_topic", mqtt_topic);
  prefs.putInt("mesh_uplink_id", mesh_uplink_id);
  prefs.putInt("mesh_channel_id", mesh_channel_id);

}

void bindServerCallback()
{
  wm.server->on("/btscan", handleRoute); // this is now crashing esp32 for some reason
  // wm.server->on("/info",handleRoute); // you can override wm!
}

void handleRoute()
{
  String html = F("<html><head><title>Bluetooth Scan Results</title><meta http-equiv=\"refresh\" content=\"5\" /></head><body>Bluetooth scan in progress.<br><br>This page will refresh automatically until results are available.</body></html>");

  if((millis() - btScanStarted > 25000 || btScanStarted == 0)){
    if(resultsReturned){
    scanResults = "";
    btDevicesDiscovered = 0;
    SerialBT.discoverAsync(handleBtDeviceFound, 25000);
    wm.server->send(200, "text/html", html);
    btScanStarted = millis();
    resultsReturned = false;
    }
    else{
      scanResults += "<br><br>Scan complete. Refresh again to rescan.";
      wm.server->send(200, "text/html", String(btDevicesDiscovered) + " device(s) found." + scanResults);
      resultsReturned = true;
    }
  }
  wm.server->send(200, "text/html", html); 
}

void handleBtDeviceFound(BTAdvertisedDevice *device){
      scanResults += "<br>";
      scanResults += device->getAddress().toString(true);
      scanResults += " - ";
      scanResults += device->getName().c_str();
      btDevicesDiscovered++;
}

void connectMqtt() {
  
  if(connectMqttOptions()){
  mqttClient.subscribe(mqttTopicJSONIn);
  Serial.println("Connected!");
  }
  else{
    Serial.println("Unable to connect to MQTT!");
  }

}

bool connectMqttOptions()
{
  bool result;
  if (String(mqtt_username).length() > 0)
  {
    result = mqttClient.connect(mqtt_server, mqtt_username, mqtt_password);
  }
  else
  {
    result = mqttClient.connect(mqtt_server);
  }
  return result;
}

void outputMessage(String msg, bool sendMQTT, bool sendMesh){
  if(sendMQTT && mqttClient.connected()){
    //Serial.println("MQTT is connected. Publishing message");
    mqttClient.publish(mqttTopicJSONOut.c_str(),msg);
  } else {
    //Serial.println("MQTT not connected. Not publishing message");
  }
  //Serial.println(msg);
  if(sendMesh && meshAvailable && !meshNotYetConnected && mesh_uplink_id > 0){
    //Serial.println("MESH is available. Publishing message");
    //Serial.println(mesh_channel_id);
    //mt_send_text(msg.c_str(), mesh_uplink_id, mesh_channel_id);
  }
  //
}

void mqttMessageReceived(MQTTClient *client, char topic[], char bytes[], int length)
{
  //should only ever get messages on this topic as that's all that is subscribed, but check anyway
if(strcmp(topic, mqttTopicJSONIn.c_str()) == 0){
      JsonDocument command;
      DeserializationError error = deserializeJson(command, bytes, length);

      if (error) {
        //Serial.print(F("deserializeJson() failed: "));
        //Serial.println(error.f_str());
        return;
      }
      processCommand(command);
    }
}

void processCommand(JsonDocument command){
if(command["cmnd"] == "sys" ){
  if(command["type"] == "mac"){
    const char* newMAC = command["mac"].as<const char*>();
    validateMAC((char*)newMAC);
  }
  if(command["type"] == "con"){
   // Serial.println(command["con"].as<String>());
    btConnect = command["con"].as<bool>();
   // Serial.print("btConnect is now: ");
   // Serial.println(btConnect);
    prefs.putBool("btConnect",btConnect);
  }
  if(command["type"] == "scan"){
    //Serial.println(command["scan"].as<String>());
    scan = command["scan"].as<bool>();
    //Serial.print("scan is now: ");
    //Serial.println(scan);
  }
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
if(command["cmnd"] == "work" ){
  mower.workUntil(command["hh"],command["mm"],command["areas"][0],command["areas"][1],command["areas"][2],command["areas"][3],command["areas"][4],command["areas"][5],command["areas"][6],command["areas"][7]);
}
if(command["cmnd"] == "get" ){
  if(command["type"] == "sche"){
    mower.requestScheduleRefresh(command["prof"],command["day"]);
  }
  if(command["type"] == "area"){
    mower.requestAreaSettingsRefresh(command["aid"]);
  }
  }
  if(command["cmnd"] == "set" ){
    if(command["type"] == "manual"){
      mower.setManualMode(command["manual"]);
    }
  }
}

/* void handleMeshMessage(uint32_t from, uint32_t to,  uint8_t channel, const char* text) {
  // Do your own thing here. This example just prints the message to the serial console.
  Serial.print("Received a text message on channel: ");
  Serial.print(channel);
  Serial.print(" from: ");
  Serial.print(from);
  Serial.print(" to: ");
  Serial.print(to);
  Serial.print(" message: ");
  Serial.println(text);
  if (to == 0xFFFFFFFF){
    Serial.println("This is a BROADCAST message.");
  } else if (to == my_node_num){
    Serial.println("This is a DM to me!");
  } else {
    Serial.println("This is a DM to someone else.");
  }
} */

void log(String msg){
  Serial.println(msg);
  JsonDocument doc;
  doc["type"] = "log";
  doc["msg"] = msg;
  char buffer[256];
  size_t n = serializeJson(doc,buffer);
  outputMessage(buffer,true,true);
}

// This callback function will be called whenever the radio connects to a node
//void connected_callback(mt_node_t *node, mt_nr_progress_t progress) {
//  if (meshNotYetConnected) 
//    Serial.println("Connected to Meshtastic device!");
//  meshNotYetConnected = false;
//}

void preOTA(){
  Serial.println("Closing connections to free up memory for OTA");
  prefs.end();
  needMqttConnect = false;
  mqttClient.disconnect();
  btConnect = false;
  SerialBT.disconnect();
  SerialBT.end();
}