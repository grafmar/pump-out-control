#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <HCSR04.h>

#define ONE_HOUR 3600000UL
#define DEFAULT_LOG_INTERVAL_S 1200UL
#define DEFAULT_NUM_LOG_FILES 16
#define CHUNK_SIZE_BYTES 32768
#define MAX_LOG_FILES 48
#define TRIG D5
#define ECHO D8
#define LOAD D2

const unsigned long SENSOR_SAMPLE_INTERVAL_MS = 1000UL;
const unsigned long NTP_RETRY_INTERVAL_MS = 10000UL;
const float MIN_DISTANCE_CM = 5.0f;
const float MAX_DISTANCE_CM = 50.0f;
const float LOW_LEVEL_DISTANCE_CM = 31.0f;
const float PUMP_ON_WATER_LEVEL_CM = 21.0f;
const float PUMP_OFF_WATER_LEVEL_CM = 3.0f;
const uint8_t INVALID_SAMPLE_LIMIT = 10;
const unsigned long MAX_PUMP_RUNTIME_MS = 20UL * 60UL * 1000UL;

UltraSonicDistanceSensor distanceSensor(TRIG, ECHO);

class WaterLevelSensor {
public:
  bool update(unsigned long currentMillis) {
    if (currentMillis - m_lastSampleMillis < SENSOR_SAMPLE_INTERVAL_MS) return false;
    m_lastSampleMillis = currentMillis;

    float distanceSum = 0.0f;
    uint8_t validCount = 0;
    for (uint8_t i = 0; i < 5; i++) {
      if (i > 0) delay(60);
      float distance = distanceSensor.measureDistanceCm();
      if (distance >= MIN_DISTANCE_CM && distance <= MAX_DISTANCE_CM) {
        distanceSum += distance;
        validCount++;
      }
    }

    m_valid = validCount >= 3;
    if (!m_valid) return true;

    float distanceCm = distanceSum / validCount;
    m_waterLevelCm = LOW_LEVEL_DISTANCE_CM - distanceCm;
    return true;
  }

  bool isValid() const { return m_valid; }
  float waterLevelCm() const { return m_waterLevelCm; }

private:
  unsigned long m_lastSampleMillis = 0;
  float m_waterLevelCm = 0.0f;
  bool m_valid = false;
};

WaterLevelSensor waterLevelSensor;
bool pumpActive = false;
unsigned long pumpStartMillis = 0;
uint8_t consecutiveInvalidSamples = 0;
unsigned long lastLogMillis = 0;
const unsigned long intervalNTP = ONE_HOUR;
unsigned long prevNTP = 0;
unsigned long lastNTPResponse = millis();
uint32_t timeUNIX = 0;

ESP8266WebServer server(80);             // create a web server on port 80

File fsUploadFile;                                    // a File variable to temporarily store the received file
uint32_t logInterval = DEFAULT_LOG_INTERVAL_S;
uint8_t numOfLogFiles = DEFAULT_NUM_LOG_FILES;

void setPumpActive(bool active);
void startPump();
void stopPump();
void checkNTPResponse();
void handleConfigPage();
void handleSaveSettings();
void handleDataDownload();
void handleListFiles();
void loadLogSettings();
bool saveLogSettings();
void enforceLogFileLimit();
bool rotateLogs();
void writeLog(float waterLevelCm, uint32_t pumpRuntimeSeconds = 0);
uint32_t currentUnixTime();

const char *OTAName = "Pumpe";         // A name and a password for the OTA service
const char *OTAPassword = "pumpe";

const char* mdnsName = "pumpe";        // Domain name for the mDNS responder

WiFiUDP UDP;                   // Create an instance of the WiFiUDP class to send and receive UDP messages

IPAddress timeServerIP;        // The time.nist.gov NTP server's IP address
const char* ntpServerName = "time.nist.gov";

const int NTP_PACKET_SIZE = 48;          // NTP time stamp is in the first 48 bytes of the message

byte packetBuffer[NTP_PACKET_SIZE];      // A buffer to hold incoming and outgoing packets

/*__________________________________________________________SETUP__________________________________________________________*/

void setup() {
  pinMode(LOAD, OUTPUT);
  digitalWrite(LOAD, LOW);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Serial.begin(115200);        // Start the Serial communication to send messages to the computer
  delay(10);
  Serial.println("\r\n");

  startWiFi();                 // Start a Wi-Fi access point, and try to connect to some given access points. Then wait for either an AP or STA connection

  startOTA();                  // Start the OTA service

  startLittleFS();             // Start the LittleFS and list all contents

  loadLogSettings();

  startMDNS();                 // Start the mDNS responder

  startServer();               // Start a HTTP server with a file read handler and an upload handler

  startUDP();                  // Start listening for UDP messages to port 123

  WiFi.hostByName(ntpServerName, timeServerIP); // Get the IP address of the NTP server
  Serial.print("Time server IP:\t");
  Serial.println(timeServerIP);

  prevNTP = millis();
  sendNTPpacket(timeServerIP);
}

/*__________________________________________________________LOOP__________________________________________________________*/

void loop() {
  unsigned long currentMillis = millis();

  // resend NTP request if no response has been received within the retry interval, or if the time has been set and the interval has passed
  if ((timeUNIX == 0 && currentMillis - prevNTP >= NTP_RETRY_INTERVAL_MS) ||
      (timeUNIX != 0 && currentMillis - prevNTP >= intervalNTP)) {
    prevNTP = currentMillis;
    sendNTPpacket(timeServerIP);
  }

  checkNTPResponse();

  if (waterLevelSensor.update(currentMillis)) {
    if (!waterLevelSensor.isValid()) {
      Serial.println("Invalid ultrasonic distance (expected 5-50 cm).");
      if (consecutiveInvalidSamples < INVALID_SAMPLE_LIMIT) {
        consecutiveInvalidSamples++;
      }
      if (pumpActive && consecutiveInvalidSamples >= INVALID_SAMPLE_LIMIT) {
        stopPump();
      }
    } else {
      consecutiveInvalidSamples = 0;
      float level = waterLevelSensor.waterLevelCm();
      Serial.printf("Water level: %.2f cm\n", level);

      if (!pumpActive && level >= PUMP_ON_WATER_LEVEL_CM) {
        startPump();
      } else if (pumpActive && level <= PUMP_OFF_WATER_LEVEL_CM) {
        stopPump();
      }
    }
  }

  currentMillis = millis();
  if (pumpActive && currentMillis - pumpStartMillis >= MAX_PUMP_RUNTIME_MS) {
    Serial.println("Maximum pump runtime reached; stopping pump.");
    stopPump();
  }

  if (waterLevelSensor.isValid() && currentMillis - lastLogMillis >= logInterval * 1000UL) {
    lastLogMillis = currentMillis;
    writeLog(waterLevelSensor.waterLevelCm());
  }

  server.handleClient();                      // run the server
  ArduinoOTA.handle();                        // listen for OTA events
}

void checkNTPResponse() {
  uint32_t time = getTime();
  if (time) {
    timeUNIX = time;
    Serial.print("NTP response:\t");
    Serial.println(timeUNIX);
    lastNTPResponse = millis();
  } else if (millis() - lastNTPResponse > 24UL * ONE_HOUR) {
    Serial.println("More than 24 hours since last NTP response. Rebooting.");
    Serial.flush();
    ESP.reset();
  }
}

void setPumpActive(bool active) {
  pumpActive = active;
  digitalWrite(LOAD, active ? HIGH : LOW);
  digitalWrite(LED_BUILTIN, active ? LOW : HIGH);
}

void startPump() {
  pumpStartMillis = millis();
  setPumpActive(true);
  Serial.println("Pump started.");
}

void stopPump() {
  uint32_t runtimeSeconds = (millis() - pumpStartMillis) / 1000UL;
  setPumpActive(false);
  Serial.printf("Pump stopped after %lu s.\n", static_cast<unsigned long>(runtimeSeconds));
  writeLog(waterLevelSensor.waterLevelCm(), runtimeSeconds);
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
  server.on("/list", HTTP_GET, handleListFiles);

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

void handleListFiles() {
  String html = "<h2>Files</h2><ul>";
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    html += "<li>" + dir.fileName() + " (" + String(dir.fileSize()) + ")</li>";
  }
  html += "</ul>";
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

void writeLog(float waterLevelCm, uint32_t pumpRuntimeSeconds) {
  if (timeUNIX == 0) {
    Serial.println("NTP time unavailable; dropping log entry.");
    return;
  }

  uint32_t timestamp = currentUnixTime();
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

  bool success = logFile.print(timestamp) > 0;
  success = (logFile.print(';') == 1) && success;
  success = (logFile.print(waterLevelCm, 2) > 0) && success;
  success = (logFile.print(';') == 1) && success;
  if (pumpRuntimeSeconds > 0) success = (logFile.print(pumpRuntimeSeconds) > 0) && success;
  success = (logFile.println() > 0) && success;
  logFile.close();
  if (!success) Serial.println("Failed while writing log record.");
}

uint32_t currentUnixTime() {
  return timeUNIX + (millis() - lastNTPResponse) / 1000UL;
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
