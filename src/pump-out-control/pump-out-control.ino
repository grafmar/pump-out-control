#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>

#define ONE_HOUR 3600000UL
#define DEFAULT_LOG_INTERVAL_S 1200UL
#define DEFAULT_NUM_LOG_FILES 16
#define CHUNK_SIZE_BYTES 32768
#define MAX_LOG_FILES 48

// Dummy Klasse für den TempSensor
class TempSensor {
public:
  TempSensor();                   // Constructor declaration
  ~TempSensor();                  // Destructor declaration
  
  void setWaitForConversion(bool wait);
  void begin();
  uint8_t getDeviceCount();
  void requestTemperatures();
  float getTempCByIndex(uint8_t index);

private:
  bool m_wait;                    // Private member variable
};
TempSensor::TempSensor(): m_wait(true){}
TempSensor::~TempSensor() {}
void TempSensor::setWaitForConversion(bool wait) { m_wait = wait; }
void TempSensor::begin() {}
uint8_t TempSensor::getDeviceCount() { return 1; }
void TempSensor::requestTemperatures() {}
float TempSensor::getTempCByIndex(uint8_t index) {
  float minTemp = 20.0;
  float maxTemp = 25.0;
  return random(minTemp * 10, maxTemp * 10) / 10.0;
}


TempSensor tempSensors; // Create an instance of the temperature sensor class

ESP8266WebServer server(80);             // create a web server on port 80

File fsUploadFile;                                    // a File variable to temporarily store the received file
uint32_t logInterval = DEFAULT_LOG_INTERVAL_S;
uint8_t numOfLogFiles = DEFAULT_NUM_LOG_FILES;

void handleConfigPage();
void handleSaveSettings();
void handleDataDownload();
void loadLogSettings();
bool saveLogSettings();
void enforceLogFileLimit();
bool rotateLogs();
void writeLog(uint32_t timestamp, float temperature);

const char *OTAName = "ESP8266";         // A name and a password for the OTA service
const char *OTAPassword = "esp8266";

const char* mdnsName = "esp8266";        // Domain name for the mDNS responder

WiFiUDP UDP;                   // Create an instance of the WiFiUDP class to send and receive UDP messages

IPAddress timeServerIP;        // The time.nist.gov NTP server's IP address
const char* ntpServerName = "time.nist.gov";

const int NTP_PACKET_SIZE = 48;          // NTP time stamp is in the first 48 bytes of the message

byte packetBuffer[NTP_PACKET_SIZE];      // A buffer to hold incoming and outgoing packets

/*__________________________________________________________SETUP__________________________________________________________*/

void setup() {
  Serial.begin(115200);        // Start the Serial communication to send messages to the computer
  delay(10);
  Serial.println("\r\n");

  tempSensors.setWaitForConversion(false); // Don't block the program while the temperature sensor is reading
  tempSensors.begin();                     // Start the temperature sensor

  if (tempSensors.getDeviceCount() == 0) {
    Serial.printf("No DS18x20 temperature sensor found on pin %d. Rebooting.\r\n", 5);
    Serial.flush();
    ESP.reset();
  }

  startWiFi();                 // Start a Wi-Fi access point, and try to connect to some given access points. Then wait for either an AP or STA connection

  startOTA();                  // Start the OTA service

  startLittleFS();             // Start the LittleFS and list all contents
  if (!LittleFS.exists("/log_0.csv") && LittleFS.exists("/data.csv")) {
    if (!LittleFS.rename("/data.csv", "/log_0.csv")) {
      Serial.println("Failed to migrate legacy data.csv.");
    }
  }
  loadLogSettings();

  startMDNS();                 // Start the mDNS responder

  startServer();               // Start a HTTP server with a file read handler and an upload handler

  startUDP();                  // Start listening for UDP messages to port 123

  WiFi.hostByName(ntpServerName, timeServerIP); // Get the IP address of the NTP server
  Serial.print("Time server IP:\t");
  Serial.println(timeServerIP);

  sendNTPpacket(timeServerIP);
  delay(500);
}

/*__________________________________________________________LOOP__________________________________________________________*/

const unsigned long intervalNTP = ONE_HOUR; // Update the time every hour
unsigned long prevNTP = 0;
unsigned long lastNTPResponse = millis();

unsigned long prevTemp = 0;
bool tmpRequested = false;
const unsigned long DS_delay = 750;         // Reading the temperature from the DS18x20 can take up to 750ms

uint32_t timeUNIX = 0;                      // The most recent timestamp received from the time server

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - prevNTP > intervalNTP) { // Request the time from the time server every hour
    prevNTP = currentMillis;
    sendNTPpacket(timeServerIP);
  }

  uint32_t time = getTime();                   // Check if the time server has responded, if so, get the UNIX time
  if (time) {
    timeUNIX = time;
    Serial.print("NTP response:\t");
    Serial.println(timeUNIX);
    lastNTPResponse = millis();
  } else if ((millis() - lastNTPResponse) > 24UL * ONE_HOUR) {
    Serial.println("More than 24 hours since last NTP response. Rebooting.");
    Serial.flush();
    ESP.reset();
  }

  if (timeUNIX != 0) {
    if (currentMillis - prevTemp > logInterval * 1000UL) {
      tempSensors.requestTemperatures(); // Request the temperature from the sensor (it takes some time to read it)
      tmpRequested = true;
      prevTemp = currentMillis;
      Serial.println("Temperature requested");
    }
    if (currentMillis - prevTemp > DS_delay && tmpRequested) { // 750 ms after requesting the temperature
      uint32_t actualTime = timeUNIX + (currentMillis - lastNTPResponse) / 1000;
      // The actual time is the last NTP time plus the time that has elapsed since the last NTP response
      tmpRequested = false;
      float temp = tempSensors.getTempCByIndex(0); // Get the temperature from the sensor
      temp = round(temp * 100.0) / 100.0; // round temperature to 2 digits

      Serial.printf("Appending temperature to file: %lu;", actualTime);
      Serial.println(temp);
      writeLog(actualTime, temp);
    }
  } else {                                    // If we didn't receive an NTP response yet, send another request
    sendNTPpacket(timeServerIP);
    delay(500);
  }

  server.handleClient();                      // run the server
  ArduinoOTA.handle();                        // listen for OTA events
}

/*__________________________________________________________SETUP_FUNCTIONS__________________________________________________________*/

void startWiFi() { // Try to connect to some given access points. Then wait for a connection
  WiFiManager wm;
  wm.setTimeout(180);               // 3 Minuten Portal-Zeit

  if (!wm.autoConnect("Pump-out-controller-AP")) {
    Serial.println("WiFi failed. Rebooting.");
    delay(3000);
    ESP.reset();
  }

  Serial.println("WiFi connected");
  Serial.print("IP address:\t");
  Serial.println(WiFi.localIP());
}

void startUDP() {
  Serial.println("Starting UDP");
  UDP.begin(123);                          // Start listening for UDP messages to port 123
  Serial.print("Local port:\t");
  Serial.println(UDP.localPort());
}

void startOTA() { // Start the OTA service
  ArduinoOTA.setHostname(OTAName);
  ArduinoOTA.setPassword(OTAPassword);

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\r\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready\r\n");
}

void startLittleFS() { // Start the LittleFS and list all contents
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed. Rebooting.");
    Serial.flush();
    ESP.reset();
    return;
  }

  Serial.println("LittleFS started. Contents:");
  {
    Dir dir = LittleFS.openDir("/");
    while (dir.next()) {                      // List the file system contents
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("\tFS File: %s, size: %s\r\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
    Serial.printf("\n");
  }
}

void startMDNS() { // Start the mDNS responder
  MDNS.begin(mdnsName);                        // start the multicast domain name server
  Serial.print("mDNS responder started: http://");
  Serial.print(mdnsName);
  Serial.println(".local");
}

void startServer() { // Start a HTTP server with a file read handler and an upload handler
  server.on("/config.html", HTTP_GET, handleConfigPage);
  server.on("/savesettings", HTTP_GET, handleSaveSettings);
  server.on("/data.csv", HTTP_GET, handleDataDownload);

  server.on("/upload.html", HTTP_POST, []() {
    server.send(200, "text/plain", "");
  }, handleFileUpload);

  server.onNotFound(handleNotFound);          // if someone requests any other file or page, go to function 'handleNotFound'
  // and check if the file exists

  server.begin();                             // start the HTTP server
  Serial.println("HTTP server started.");
}

void handleConfigPage() {
  File file = LittleFS.open("/config.html.template", "r");
  if (!file) {
    Serial.println("Failed to open config.html.template");
    server.send(500, "text/plain", "500: Config template unavailable");
    return;
  }

  String html = file.readString();
  file.close();
  html.replace("%INTERVAL%", String(logInterval));
  html.replace("%NUMFILES%", String(numOfLogFiles));
  server.send(200, "text/html", html);
}

void handleSaveSettings() {
  if (!server.hasArg("interval") || !server.hasArg("numChunks")) {
    server.send(400, "text/plain", "400: Missing settings");
    return;
  }

  long interval = server.arg("interval").toInt();
  long fileCount = server.arg("numChunks").toInt();
  if (interval < 1 || interval > 1000000L ||
      fileCount < 2 || fileCount > MAX_LOG_FILES) {
    server.send(400, "text/plain", "400: Settings out of range");
    return;
  }

  uint32_t previousInterval = logInterval;
  uint8_t previousFileCount = numOfLogFiles;
  logInterval = static_cast<uint32_t>(interval);
  numOfLogFiles = static_cast<uint8_t>(fileCount);
  if (!saveLogSettings()) {
    logInterval = previousInterval;
    numOfLogFiles = previousFileCount;
    server.send(500, "text/plain", "500: Failed to save settings");
    return;
  }

  enforceLogFileLimit();
  server.sendHeader("Location", "/");
  server.send(303);
}

void loadLogSettings() {
  if (LittleFS.exists("/logcfg.txt")) {
    File file = LittleFS.open("/logcfg.txt", "r");
    if (file) {
      long interval = file.readStringUntil('\n').toInt();
      long fileCount = file.readStringUntil('\n').toInt();
      file.close();

      if (interval >= 1 && interval <= 1000000L)
        logInterval = static_cast<uint32_t>(interval);
      else
        Serial.println("Invalid saved log interval; using default.");

      if (fileCount >= 2 && fileCount <= MAX_LOG_FILES)
        numOfLogFiles = static_cast<uint8_t>(fileCount);
      else
        Serial.println("Invalid saved log file count; using default.");
    } else {
      Serial.println("Failed to open logcfg.txt; using default settings.");
    }
  } else if (!saveLogSettings()) {
    Serial.println("Failed to create logcfg.txt.");
  }

  enforceLogFileLimit();
  Serial.printf("Log settings: interval=%lu s, files=%u\n",
                static_cast<unsigned long>(logInterval),
                static_cast<unsigned int>(numOfLogFiles));
}

bool saveLogSettings() {
  File file = LittleFS.open("/logcfg.txt", "w");
  if (!file) {
    Serial.println("Failed to open logcfg.txt for writing.");
    return false;
  }

  bool success = file.print(logInterval) > 0;
  success = (file.print('\n') == 1) && success;
  success = (file.print(numOfLogFiles) > 0) && success;
  success = (file.print('\n') == 1) && success;
  file.close();
  return success;
}

void enforceLogFileLimit() {
  for (int i = numOfLogFiles; i < MAX_LOG_FILES; i++) {
    String path = "/log_" + String(i) + ".csv";
    if (LittleFS.exists(path) && !LittleFS.remove(path)) {
      Serial.printf("Failed to remove excess log file: %s\n", path.c_str());
    }
  }
}

bool rotateLogs() {
  String oldest = "/log_" + String(numOfLogFiles - 1) + ".csv";
  if (LittleFS.exists(oldest) && !LittleFS.remove(oldest)) {
    Serial.printf("Failed to remove oldest log file: %s\n", oldest.c_str());
    return false;
  }

  for (int i = numOfLogFiles - 2; i >= 0; i--) {
    String oldName = "/log_" + String(i) + ".csv";
    String newName = "/log_" + String(i + 1) + ".csv";
    if (LittleFS.exists(oldName) && !LittleFS.rename(oldName, newName)) {
      Serial.printf("Failed to rotate log file: %s\n", oldName.c_str());
      return false;
    }
  }
  return true;
}

void writeLog(uint32_t timestamp, float temperature) {
  String path = "/log_0.csv";
  if (LittleFS.exists(path)) {
    File currentLog = LittleFS.open(path, "r");
    if (!currentLog) {
      Serial.println("Failed to open current log for size check.");
      return;
    }
    bool full = currentLog.size() >= CHUNK_SIZE_BYTES;
    currentLog.close();
    if (full && !rotateLogs()) return;
  }

  File logFile = LittleFS.open(path, "a");
  if (!logFile) {
    Serial.println("Failed to open log_0.csv for writing.");
    return;
  }

  logFile.print(timestamp);
  logFile.print(';');
  logFile.print(temperature, 2);
  logFile.print(';');
  logFile.print(2 * temperature, 2);
  logFile.print(';');
  logFile.print(45.0 - 1.2 * temperature, 2);
  logFile.print(';');
  logFile.print(2 * (45.0 - temperature), 2);
  logFile.print(';');
  logFile.print(2.0 + 1.1 * temperature, 2);
  logFile.print(';');
  logFile.println(43.0 - temperature, 2);
  logFile.close();
}

void handleDataDownload() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");

  uint8_t buffer[512];
  for (int i = numOfLogFiles - 1; i >= 0; i--) {
    String path = "/log_" + String(i) + ".csv";
    if (!LittleFS.exists(path)) continue;

    File logFile = LittleFS.open(path, "r");
    if (!logFile) {
      Serial.printf("Failed to open log for download: %s\n", path.c_str());
      continue;
    }

    while (logFile.available()) {
      size_t bytesRead = logFile.read(buffer, sizeof(buffer));
      if (bytesRead == 0) break;
      server.sendContent(reinterpret_cast<const char*>(buffer), bytesRead);
    }
    logFile.close();
  }

  server.sendContent("");
}

/*__________________________________________________________SERVER_HANDLERS__________________________________________________________*/

void handleNotFound() { // if the requested file or page doesn't exist, return a 404 not found error
  if (!handleFileRead(server.uri())) {        // check if the file exists in the flash memory (LittleFS), if so, send it
    server.send(404, "text/plain", "404: File Not Found");
  }
}

bool handleFileRead(String path) { // send the right file to the client (if it exists)
  Serial.println("handleFileRead: " + path);
  if (path.endsWith("/")) path += "index.html";          // If a folder is requested, send the index file
  String contentType = getContentType(path);             // Get the MIME type
  String pathWithGz = path + ".gz";
  bool compressed = LittleFS.exists(pathWithGz);
  if (compressed) path = pathWithGz;
  if (!compressed && !LittleFS.exists(path)) {
    Serial.println(String("\tFile Not Found: ") + path);
    return false;
  }

  File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.println(String("\tFailed to open: ") + path);
    return false;
  }

  server.streamFile(file, contentType);
  file.close();
  Serial.println(String("\tSent file: ") + path);
  return true;
}

void handleFileUpload() { // upload a new file to the LittleFS
  HTTPUpload& upload = server.upload();
  String path;
  if (upload.status == UPLOAD_FILE_START) {
    path = upload.filename;
    if (!path.startsWith("/")) path = "/" + path;
    if (!path.endsWith(".gz")) {                         // The file server always prefers a compressed version of a file
      String pathWithGz = path + ".gz";                  // So if an uploaded file is not compressed, the existing compressed
      if (LittleFS.exists(pathWithGz))                     // version of that file must be deleted (if it exists)
        LittleFS.remove(pathWithGz);
    }
    Serial.print("handleFileUpload Name: "); Serial.println(path);
    fsUploadFile = LittleFS.open(path, "w");               // Open the file for writing in LittleFS (create if it doesn't exist)
    path = String();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize); // Write the received bytes to the file
  } else if (upload.status == UPLOAD_FILE_END) {
    if (fsUploadFile) {                                   // If the file was successfully created
      fsUploadFile.close();                               // Close the file again
      Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
      server.sendHeader("Location", "/success.html");     // Redirect the client to the success page
      server.send(303);
    } else {
      server.send(500, "text/plain", "500: couldn't create file");
    }
  }
}

/*__________________________________________________________HELPER_FUNCTIONS__________________________________________________________*/

String formatBytes(size_t bytes) { // convert sizes in bytes to KB and MB
  if (bytes < 1024) {
    return String(bytes) + "B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes / 1024.0) + "KB";
  } else {
    return String(bytes / 1024.0 / 1024.0) + "MB";
  }
}

String getContentType(String filename) { // determine the filetype of a given filename, based on the extension
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".json")) return "application/json";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".png")) return "image/png";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

unsigned long getTime() { // Check if the time server has responded, if so, get the UNIX time, otherwise, return 0
  if (UDP.parsePacket() == 0) { // If there's no response (yet)
    return 0;
  }
  UDP.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer
  // Combine the 4 timestamp bytes into one 32-bit number
  uint32_t NTPTime = (packetBuffer[40] << 24) | (packetBuffer[41] << 16) | (packetBuffer[42] << 8) | packetBuffer[43];
  // Convert NTP time to a UNIX timestamp:
  // Unix time starts on Jan 1 1970. That's 2208988800 seconds in NTP time:
  const uint32_t seventyYears = 2208988800UL;
  // subtract seventy years:
  uint32_t UNIXTime = NTPTime - seventyYears;
  return UNIXTime;
}


void sendNTPpacket(IPAddress& address) {
  Serial.println("Sending NTP request");
  memset(packetBuffer, 0, NTP_PACKET_SIZE);  // set all bytes in the buffer to 0
  // Initialize values needed to form NTP request
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode

  // send a packet requesting a timestamp:
  UDP.beginPacket(address, 123); // NTP requests are to port 123
  UDP.write(packetBuffer, NTP_PACKET_SIZE);
  UDP.endPacket();
}
