#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>
#include <HTTPClient.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SDA_PIN 8
#define SCL_PIN 9
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Wi-Fi Credentials
const char* ssid = "TampleDiago";
const char* password = "12345699";

// NTP Client Setup
WiFiUDP ntpUDP;
// NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // Update every minute
// NTPClient timeClient(ntpUDP, "pool.ntp.org", 18000, 60000); // 18000 seconds = 5 hours
NTPClient timeClient(ntpUDP, "time.google.com", 18000, 60000);

// EEPROM Configuration
#define EEPROM_SIZE 128
#define ADDR_ENERGY1 0
#define ADDR_ENERGY2 4
#define ADDR_ENERGY3 8
#define ADDR_ONTIME1 12
#define ADDR_ONTIME2 16
#define ADDR_ONTIME3 20
#define ADDR_LAST_RESET 24 // Stores timestamp of last daily reset (Unix epoch)

// PZEM-004T UART Pins
#define PZEM_RX_PIN 16  // ESP32 RX (Connect to PZEM TX)
#define PZEM_TX_PIN 17  // ESP32 TX (Connect to PZEM RX)

// Initialize PZEM sensor on Serial2
PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);

// ACS712 Sensor Pins and Constants
const int currentPin1 = 4; // Light-1-Main
const int currentPin2 = 5; // Light-2
const int currentPin3 = 6; // Socket-1

const int numSamples = 1000;
const float ADC_MAX = 4095.0;
const float VOLTAGE_REF = 3.3;
const float ACS_SENSITIVITY = 0.066; // ACS712-30A = 66mV/A
const float NOISE_THRESHOLD = 0.30;
const int STABLE_RECAL_TIME = 5; // seconds

float offset1 = 0, offset2 = 0, offset3 = 0;
unsigned long lastLowCurrentTime = 0;
unsigned long lastUpdateTime = 0;

// Energy (in Wh) accumulators
float energy1 = 0.0, energy2 = 0.0, energy3 = 0.0;

// Appliance ON Time tracking (in minutes)
float totalOnTime1 = 0.0; // in minutes
float totalOnTime2 = 0.0; // in minutes
float totalOnTime3 = 0.0; // in minutes

unsigned long applianceOnStartTime1 = 0;
unsigned long applianceOnStartTime2 = 0;
unsigned long applianceOnStartTime3 = 0;

bool appliance1_was_on = false;
bool appliance2_was_on = false;
bool appliance3_was_on = false;

// Relay Pins
const int relay1Pin = 11; // Light-1-Main
const int relay2Pin = 12; // Light-2
const int relay3Pin = 13; // Socket-1

// Relay States (true = ON, false = OFF)
bool relay1State = true; // Initial: Appliance ON
bool relay2State = true;
bool relay3State = true;

// Voltage threshold
const float VOLTAGE_THRESHOLD = 150.0;
bool lowVoltageAlert = false;

// Schedule structure
struct Schedule {
    int hour;    // 1-12
    int minute;  // 0-59
    String ampm; // "AM" or "PM"
    bool active;
};

Schedule schedules[3] = {{0, 0, "", false}, {0, 0, "", false}, {0, 0, "", false}}; // For relays 1, 2, 3

// InfluxDB Configuration
const char* INFLUXDB_URL = "http://192.168.41.78:8086";
const char* INFLUXDB_TOKEN = "yRTaoZbfgAxja1QghqdggMUqTZgjaCTSakNNZZtnzrmQp9SQobLH9MDhk7j5FYxFBc3TigbmO8M30QdHo52Q5g==";
const char* INFLUXDB_ORG = "iotlab";
const char* INFLUXDB_BUCKET = "smart_power_analysis";

// InfluxDB Sender Class
class InfluxDBSender {
private:
    HTTPClient http;

    bool sendData(const String& measurement, const String& tags, const String& fields) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Wi-Fi not connected. Cannot send data to InfluxDB.");
            return false;
        }

        String url = String(INFLUXDB_URL) + "/api/v2/write?org=" + INFLUXDB_ORG + "&bucket=" + INFLUXDB_BUCKET + "&precision=ms";
        http.begin(url);
        http.addHeader("Authorization", "Token " + String(INFLUXDB_TOKEN));
        http.addHeader("Content-Type", "text/plain");

        String payload = measurement + "," + tags + " " + fields;
        Serial.print("Sending to InfluxDB: ");
        Serial.println(payload);

        int httpCode = http.POST(payload);
        if (httpCode > 0) {
            Serial.print("InfluxDB response code: ");
            Serial.println(httpCode);
            if (httpCode == 204) {
                http.end();
                return true;
            } else {
                Serial.print("Failed to send data. Response: ");
                Serial.println(http.getString());
            }
        } else {
            Serial.print("Failed to send data. HTTP error: ");
            Serial.println(http.errorToString(httpCode));
        }
        http.end();
        return false;
    }

public:
    void sendApplianceMetrics(const char* appliance, float current, float energy) {
        String tags = "appliance=" + String(appliance);
        String fields = "current=" + String(current, 6) + ",energy=" + String(energy, 6);
        if (!sendData("appliance_metrics", tags, fields)) {
            Serial.println("Failed to send appliance metrics for " + String(appliance));
        }
    }

    void sendHomeMetrics(float voltage, float current, float power, float energy, float frequency, float powerFactor) {
        String tags = "location=home";
        String fields = "voltage=" + String(voltage, 6) + ",current=" + String(current, 6) +
                        ",power=" + String(power, 6) + ",energy=" + String(energy, 6) +
                        ",frequency=" + String(frequency, 6) + ",powerFactor=" + String(powerFactor, 6);
        if (!sendData("home_metrics", tags, fields)) {
            Serial.println("Failed to send home metrics");
        }
    }

    void sendDailySummary(const char* appliance, float onTimeHours, float energyKWh) {
        String tags = "appliance=" + String(appliance);
        String fields = "onTimeHours=" + String(onTimeHours, 6) + ",energyKWh=" + String(energyKWh, 6);
        if (!sendData("appliance_daily", tags, fields)) {
            Serial.println("Failed to send daily summary for " + String(appliance));
        }
    }
};

// Create AsyncWebServer and WebSocket
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
InfluxDBSender influxSender;


void setupOLED() {
  // Initialize I2C with custom SDA and SCL pins
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;); // Loop forever if initialization fails
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  
  // Display initial message
  display.setCursor(0, 0);
  display.println(F("Smart Energy Analysis"));
  display.println(F("and Control System"));
  
  // Display Server IP
  display.setCursor(0, 24);
  display.print(F("Server IP: "));
  display.println(WiFi.localIP());
  
  display.display();
}

// Login Page
const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
    <title>Smart Home Login</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f4; color: #333; display: flex; justify-content: center; align-items: center; height: 100vh; }
        .login-container { background-color: white; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); padding: 30px; width: 100%; max-width: 400px; text-align: center; }
        .login-container h2 { color: #4CAF50; margin-bottom: 20px; }
        .form-group { margin-bottom: 15px; }
        .form-group label { display: block; font-weight: bold; margin-bottom: 5px; }
        .form-group input { width: calc(100% - 20px); padding: 10px; border: 1px solid #ccc; border-radius: 5px; font-size: 1em; }
        .login-btn { background-color: #4CAF50; color: white; padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-size: 1em; }
        .login-btn:hover { background-color: #45a049; }
        .error-message { color: #f44336; margin-top: 10px; display: none; }
    </style>
</head>
<body>
    <div class="login-container">
        <h2>Smart Home Login</h2>
        <div class="form-group">
            <label for="username">Username:</label>
            <input type="text" id="username" placeholder="Enter username">
        </div>
        <div class="form-group">
            <label for="password">Password:</label>
            <input type="password" id="password" placeholder="Enter password">
        </div>
        <button class="login-btn" onclick="login()">Login</button>
        <p id="errorMessage" class="error-message">Invalid username or password!</p>
    </div>
<script>
    function login() {
        var username = document.getElementById('username').value;
        var password = document.getElementById('password').value;
        var errorMessage = document.getElementById('errorMessage');
        
        if (username === 'admin' && password === 'admin') {
            sessionStorage.setItem('isAuthenticated', 'true');
            window.location.href = '/';
        } else {
            errorMessage.style.display = 'block';
            setTimeout(() => { errorMessage.style.display = 'none'; }, 3000);
        }
    }

    // Check if already authenticated
    if (sessionStorage.getItem('isAuthenticated') === 'true') {
        window.location.href = '/';
    }
</script>
</body></html>
)rawliteral";

// Home Page
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
    <title>Home - Smart Home</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f4; color: #333; }
        .header { background-color: #4CAF50; color: white; padding: 15px; text-align: center; }
        .navbar { overflow: hidden; background-color: #333; }
        .navbar a { float: left; display: block; color: white; text-align: center; padding: 14px 20px; text-decoration: none; }
        .navbar a:hover { background-color: #ddd; color: black; }
        .container { padding: 20px; display: flex; flex-wrap: wrap; justify-content: space-around; }
        .card { background-color: white; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); margin: 10px; padding: 20px; flex: 1 1 calc(33% - 40px); box-sizing: border-box; text-align: center; min-width: 250px; display: flex; flex-direction: column; justify-content: space-between; }
        .card h2 { color: #4CAF50; margin-bottom: 10px; }
        .card p { font-size: 1.1em; margin: 5px 0; }
        .card a { display: block; background-color: #008CBA; color: white; padding: 10px 15px; border-radius: 5px; text-decoration: none; margin-top: 15px; }
        .card a:hover { background-color: #005f7c; }
        .status-ok { color: #4CAF50; font-weight: bold; }
        .status-alert { color: #f44336; font-weight: bold; }
        .status-section { margin-top: 10px; }
        .status-section h3 { margin-bottom: 5px; color: #555; }
    </style>
</head>
<body>
    <div class="header">
        <h1>Smart Home Dashboard</h1>
    </div>
    <div class="navbar">
        <a href="/">Home</a>
        <a href="/settings">Settings</a>
        <a href="/help">Help</a>
        <a href="/report">Report</a>
        <a href="#" onclick="logout()">Logout</a>
    </div>
    <div class="container">
        <div class="card">
            <h2>Room 1</h2>
            <p>Control and monitor devices in Room 1.</p>
            <a href="/room1">Go to Room 1 Control</a>
        </div>
        <div class="card">
            <h2>Kitchen</h2>
            <p>Control and monitor devices in Kitchen. (Placeholder)</p>
            <a href="#" onclick="alert('Kitchen is Not cofigured Physically!'); return false;">Configration</a>
        </div>
        <div class="card">
            <h2>Overall System Status</h2>
            <div class="status-section">
                <h3>Overall Load:</h3>
                <p id="overallLoad">Determining status...</p>
            </div>
            <div class="status-section">
                <h3>Fault Status:</h3>
                <p id="faultStatus" class="status-ok">OK</p>
            </div>
            <div class="status-section">
                <h3>Voltage Status:</h3>
                <p id="voltageStatus" class="status-ok">Checking...</p>
            </div>
        </div>
    </div>
<script>
    // Check authentication
    if (sessionStorage.getItem('isAuthenticated') !== 'true') {
        window.location.href = '/login';
    }

    function logout() {
        sessionStorage.removeItem('isAuthenticated');
        window.location.href = '/login';
    }

    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    window.addEventListener('load', onLoad);
    function onLoad(event) {
        initWebSocket();
    }
    function initWebSocket() {
        console.log('Trying to open a WebSocket connection...');
        websocket = new WebSocket(gateway);
        websocket.onopen = onOpen;
        websocket.onclose = onClose;
        websocket.onmessage = onMessage;
    }
    function onOpen(event) {
        console.log('Connection opened');
        websocket.send('{"action":"requestStatus"}');
    }
    function onClose(event) {
        console.log('Connection closed');
        setTimeout(initWebSocket, 2000);
    }
    function onMessage(event) {
        var data = JSON.parse(event.data);
        if (data.type === "statusUpdate") {
            document.getElementById('overallLoad').innerHTML = data.overallLoad;
            document.getElementById('faultStatus').innerHTML = data.faultStatus;
            document.getElementById('faultStatus').className = data.faultStatus === "OK" ? "status-ok" : "status-alert";
            document.getElementById('voltageStatus').innerHTML = data.voltageStatus;
            document.getElementById('voltageStatus').className = data.voltageStatus.includes("Low") ? "status-alert" : "status-ok";
        }
    }
</script>
</body></html>
)rawliteral";

// Room Control Page
const char ROOM1_CONTROL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
    <title>Room 1 Control</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f4; color: #333; }
        .header { background-color: #4CAF50; color: white; padding: 15px; text-align: center; }
        .navbar { overflow: hidden; background-color: #333; }
        .navbar a { float: left; display: block; color: white; text-align: center; padding: 14px 20px; text-decoration: none; }
        .navbar a:hover { background-color: #ddd; color: black; }
        .container { padding: 20px; display: flex; flex-wrap: wrap; justify-content: space-around; }
        .card { background-color: white; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); margin: 10px; padding: 20px; flex: 1 1 calc(50% - 40px); box-sizing: border-box; text-align: center; min-width: 300px; display: flex; flex-direction: column; justify-content: space-between; position: relative; }
        .card h2 { color: #4CAF50; margin-bottom: 10px; }
        .card p { font-size: 1.1em; margin: 5px 0; }
        .toggle-btn { padding: 10px 20px; border-radius: 5px; cursor: pointer; border: none; color: white; font-weight: bold; margin-top: 15px; }
        .toggle-btn.on { background-color: #4CAF50; }
        .toggle-btn.off { background-color: #f44336; }
        .config-btn { background-color: #008CBA; }
        .config-btn:hover { background-color: #005f7c; }
        .warning-text { color: #f44336; font-weight: bold; }
        .optimal-text { color: #4CAF50; font-weight: bold; }
        .schedule-section { margin-top: 20px; }
        .schedule-section select, .schedule-section input { padding: 8px; margin: 5px; border-radius: 5px; border: 1px solid #ccc; }
        .schedule-section button { background-color: #4CAF50; color: white; padding: 8px 15px; border-radius: 5px; border: none; cursor: pointer; }
        .schedule-section button:hover { background-color: #45a049; }
        .countdown { position: absolute; top: 10px; right: 10px; font-size: 0.8em; color: #666; }
    </style>
</head>
<body>
    <div class="header">
        <h1>Room 1 Control</h1>
    </div>
    <div class="navbar">
        <a href="/">Home</a>
        <a href="/room1">Control</a>
        <a href="/room1/stats">Room Stats</a>
        <a href="/report">Report</a>
        <a href="#" onclick="logout()">Logout</a>
    </div>
    <div class="container">
        <div class="card">
            <h2>Light-1-Main</h2>
            <p>Current: <span id="current1">0.00</span> A</p>
            <p>Energy: <span id="energy1">0.0000</span> Wh</p>
            <button class="toggle-btn" id="toggle1" onclick="toggleRelay(1)">Loading...</button>
            <div class="countdown" id="countdown1">No schedule set</div>
            <div class="schedule-section">
                <h3>Schedule Toggle Time</h3>
                <select id="hour1">
                    <option value="">Hour</option>
                    <script>for(let i=1;i<=12;i++) document.write(`<option value="${i}">${i}</option>`);</script>
                </select>
                <select id="minute1">
                    <option value="">Minute</option>
                    <script>for(let i=0;i<60;i+=5) document.write(`<option value="${i}">${i}</option>`);</script>
                </select>
                <select id="ampm1">
                    <option value="AM">AM</option>
                    <option value="PM">PM</option>
                </select>
                <button onclick="setSchedule(1)">Set Schedule</button>
            </div>
        </div>
        <div class="card">
            <h2>Light-2</h2>
            <p>Current: <span id="current2">0.00</span> A</p>
            <p>Energy: <span id="energy2">0.0000</span> Wh</p>
            <button class="toggle-btn" id="toggle2" onclick="toggleRelay(2)">Loading...</button>
            <div class="countdown" id="countdown2">No schedule set</div>
            <div class="schedule-section">
                <h3>Schedule Toggle Time</h3>
                <select id="hour2">
                    <option value="">Hour</option>
                    <script>for(let i=1;i<=12;i++) document.write(`<option value="${i}">${i}</option>`);</script>
                </select>
                <select id="minute2">
                    <option value="">Minute</option>
                    <script>for(let i=0;i<60;i+=1) document.write(`<option value="${i}">${i}</option>`);</script>
                </select>
                <select id="ampm2">
                    <option value="AM">AM</option>
                    <option value="PM">PM</option>
                </select>
                <button onclick="setSchedule(2)">Set Schedule</button>
            </div>
        </div>
        <div class="card">
            <h2>Socket-1</h2>
            <p>Current: <span id="current3">0.00</span> A</p>
            <p>Energy: <span id="energy3">0.0000</span> Wh</p>
            <button class="toggle-btn" id="toggle3" onclick="toggleRelay(3)">Loading...</button>
            <div class="countdown" id="countdown3">No schedule set</div>
            <div class="schedule-section">
                <h3>Schedule Toggle Time</h3>
                <select id="hour3">
                    <option value="">Hour</option>
                    <script>for(let i=1;i<=12;i++) document.write(`<option value="${i}">${i}</option>`);</script>
                </select>
                <select id="minute3">
                    <option value="">Minute</option>
                    <script>for(let i=0;i<60;i+=1) document.write(`<option value="${i}">${i}</option>`);</script>
                </select>
                <select id="ampm3">
                    <option value="AM">AM</option>
                    <option value="PM">PM</option>
                </select>
                <button onclick="setSchedule(3)">Set Schedule</button>
            </div>
        </div>
        <div class="card">
            <h2>TV</h2>
            <p>Current: <span id="currentTV">0.00</span> A</p>
            <p>Energy: <span id="energyTV">0.0000</span> Wh</p>
            <button onclick="alert('TV Not cofigured Physically!')" class="toggle-btn config-btn">Configuration</button>
        </div>
        <div class="card">
            <h2>Electricity Waste</h2>
            <p><span id="electricityWaste">Calculating...</span></p>
            <p style="font-size: 0.9em; color: #666;">(Based on Power Factor)</p>
        </div>
        <div class="card">
            <h2>Appliance Health Status</h2>
            <p><span id="applianceHealth">Checking...</span></p>
            <p style="font-size: 0.9em; color: #666;">(Based on Frequency)</p>
        </div>
    </div>
<script>
    // Check authentication
    if (sessionStorage.getItem('isAuthenticated') !== 'true') {
        window.location.href = '/login';
    }

    function logout() {
        sessionStorage.removeItem('isAuthenticated');
        window.location.href = '/login';
    }

    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    var schedules = [{hour: -1, minute: -1, ampm: ''}, {hour: -1, minute: -1, ampm: ''}, {hour: -1, minute: -1, ampm: ''}];
    window.addEventListener('load', onLoad);
    function onLoad(event) {
        initWebSocket();
        startCountdown();
    }
    function initWebSocket() {
        console.log('Trying to open a WebSocket connection...');
        websocket = new WebSocket(gateway);
        websocket.onopen = onOpen;
        websocket.onclose = onClose;
        websocket.onmessage = onMessage;
    }
    function onOpen(event) {
        console.log('Connection opened');
        websocket.send('{"action":"requestData"}');
        websocket.send('{"action":"requestPZEM"}');
        websocket.send('{"action":"requestSchedules"}');
    }
    function onClose(event) {
        console.log('Connection closed');
        setTimeout(initWebSocket, 2000);
    }
    function onMessage(event) {
        var data = JSON.parse(event.data);
        if (data.type === "sensorData") {
            document.getElementById('current1').innerHTML = data.current1.toFixed(2);
            document.getElementById('energy1').innerHTML = data.energy1.toFixed(4);
            document.getElementById('current2').innerHTML = data.current2.toFixed(2);
            document.getElementById('energy2').innerHTML = data.energy2.toFixed(4);
            document.getElementById('current3').innerHTML = data.current3.toFixed(2);
            document.getElementById('energy3').innerHTML = data.energy3.toFixed(4);
            document.getElementById('currentTV').innerHTML = data.currentTV.toFixed(2);
            document.getElementById('energyTV').innerHTML = data.energyTV.toFixed(4);
        } else if (data.type === "relayState") {
            updateToggleButton(1, data.relay1State);
            updateToggleButton(2, data.relay2State);
            updateToggleButton(3, data.relay3State);
        } else if (data.type === "pzemData") {
            let pf = data.powerFactor;
            let frequency = data.frequency;
            let wasteText = "Calculating...";
            let healthText = "Checking...";
            let healthClass = "";
            if (pf < 0.60 && pf > 0) {
                wasteText = "40% Electricity Waste";
            } else if (pf < 0.70 && pf >= 0.60) {
                wasteText = "30% Electricity Waste";
            } else if (pf < 0.80 && pf >= 0.70) {
                wasteText = "20% Electricity Waste";
            } else if (pf < 0.90 && pf >= 0.80) {
                wasteText = "10% Electricity Waste";
            } else if (pf >= 0.90) {
                wasteText = "Optimal (0% Waste)";
            } else {
                wasteText = "N/A (No Power)";
            }
            document.getElementById('electricityWaste').innerHTML = wasteText;
            if (frequency < 45 && frequency > 0) {
                healthText = "Some Appliances are damaged and not Working correctly. See, it may cause of any damages.";
                healthClass = "warning-text";
            } else if (frequency >= 45) {
                healthText = "All Appliances are working optimally.";
                healthClass = "optimal-text";
            } else {
                healthText = "N/A (No Frequency Data)";
            }
            document.getElementById('applianceHealth').innerHTML = healthText;
            document.getElementById('applianceHealth').className = healthClass;
        } else if (data.type === "scheduleUpdate") {
            var relayNum = data.relay;
            schedules[relayNum - 1].hour = data.hour;
            schedules[relayNum - 1].minute = data.minute;
            schedules[relayNum - 1].ampm = data.ampm;
            updateCountdown(relayNum);
        } else if (data.type === "schedulesData") {
            for (let i = 0; i < 3; i++) {
                schedules[i].hour = data.schedules[i].hour;
                schedules[i].minute = data.schedules[i].minute;
                schedules[i].ampm = data.schedules[i].ampm;
                updateCountdown(i + 1);
            }
        }
    }
    function toggleRelay(relayNum) {
        var button = document.getElementById('toggle' + relayNum);
        var newState = button.classList.contains('on') ? "OFF" : "ON";
        websocket.send('{"action":"toggleRelay", "relay":"' + relayNum + '", "state":"' + newState + '"}');
    }
    function updateToggleButton(relayNum, state) {
        var button = document.getElementById('toggle' + relayNum);
        if (state === "ON") {
            button.classList.remove('off');
            button.classList.add('on');
            button.innerHTML = "ON";
        } else {
            button.classList.remove('on');
            button.classList.add('off');
            button.innerHTML = "OFF";
        }
    }
    function setSchedule(relayNum) {
        var hour = document.getElementById('hour' + relayNum).value;
        var minute = document.getElementById('minute' + relayNum).value;
        var ampm = document.getElementById('ampm' + relayNum).value;
        if (hour === "" || minute === "" || ampm === "") {
            alert("Please select hour, minute, and AM/PM.");
            return;
        }
        websocket.send('{"action":"setSchedule", "relay":"' + relayNum + '", "hour":' + hour + ', "minute":' + minute + ', "ampm":"' + ampm + '"}');
        alert("Schedule set for relay " + relayNum + " at " + hour + ":" + (minute < 10 ? "0" : "") + minute + " " + ampm);
    }
    function startCountdown() {
        setInterval(() => {
            for (let i = 1; i <= 3; i++) {
                updateCountdown(i);
            }
        }, 1000);
    }
    function updateCountdown(relayNum) {
        var schedule = schedules[relayNum - 1];
        var countdownElement = document.getElementById('countdown' + relayNum);
        if (schedule.hour === -1 || schedule.minute === -1 || schedule.ampm === '') {
            countdownElement.innerHTML = "No schedule set";
            return;
        }
        var now = new Date();
        var scheduleHour = parseInt(schedule.hour);
        if (schedule.ampm === "PM" && scheduleHour !== 12) {
            scheduleHour += 12;
        } else if (schedule.ampm === "AM" && scheduleHour === 12) {
            scheduleHour = 0;
        }
        var scheduleTime = new Date();
        scheduleTime.setHours(scheduleHour, parseInt(schedule.minute), 0, 0);
        if (scheduleTime < now) {
            scheduleTime.setDate(scheduleTime.getDate() + 1);
        }
        var diffMs = scheduleTime - now;
        var diffSec = Math.floor(diffMs / 1000);
        var hours = Math.floor(diffSec / 3600);
        var minutes = Math.floor((diffSec % 3600) / 60);
        var seconds = diffSec % 60;
        countdownElement.innerHTML = `Next toggle in ${hours}h ${minutes}m ${seconds}s`;
    }
</script>
</body></html>
)rawliteral";

// Room Stats Page
const char ROOM1_stats_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
    <title>Room 1 Stats</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f4; color: #333; }
        .header { background-color: #4CAF50; color: white; padding: 15px; text-align: center; }
        .navbar { overflow: hidden; background-color: #333; }
        .navbar a { float: left; display: block; color: white; text-align: center; padding: 14px 20px; text-decoration: none; }
        .navbar a:hover { background-color: #ddd; color: black; }
        .container { padding: 20px; display: flex; flex-wrap: wrap; justify-content: space-around; }
        .card { background-color: white; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); margin: 10px; padding: 20px; flex: 1 1 calc(33% - 40px); box-sizing: border-box; text-align: center; min-width: 280px; position: relative; display: flex; flex-direction: column; justify-content: center; align-items: center; }
        .card h2 { color: #4CAF50; margin-bottom: 10px; }
        .card p { font-size: 2.5em; font-weight: bold; margin: 0; display: flex; align-items: baseline; }
        .unit { position: absolute; bottom: 10px; right: 15px; font-size: 0.8em; color: #666; font-weight: normal; }
        .large-unit { font-size: 0.5em; margin-left: 5px; font-weight: normal; }
    </style>
</head>
<body>
    <div class="header">
        <h1>Home Stat</h1>
    </div>
    <div class="navbar">
        <a href="/">Home</a>
        <a href="/room1">Control</a>
        <a href="/room1/stats">Room Stats</a>
        <a href="/report">Report</a>
        <a href="#" onclick="logout()">Logout</a>
    </div>
    <div class="container">
        <div class="card">
            <h2>Voltage</h2>
            <p><span id="voltage">0.00</span><span class="large-unit">V</span></p>
        </div>
        <div class="card">
            <h2>Current</h2>
            <p><span id="current">0.00</span><span class="large-unit">A</span></p>
        </div>
        <div class="card">
            <h2>Power</h2>
            <p><span id="power">0.00</span><span class="large-unit">W</span></p>
        </div>
        <div class="card">
            <h2>Home Energy</h2>
            <p><span id="energy">0.000</span><span class="unit">kWh</span></p>
        </div>
        <div class="card">
            <h2>Frequency</h2>
            <p><span id="frequency">0.00</span><span class="unit">Hz</span></p>
        </div>
        <div class="card">
            <h2>Power Factor</h2>
            <p><span id="powerFactor">0.00</span></p>
        </div>
    </div>
<script>
    // Check authentication
    if (sessionStorage.getItem('isAuthenticated') !== 'true') {
        window.location.href = '/login';
    }

    function logout() {
        sessionStorage.removeItem('isAuthenticated');
        window.location.href = '/login';
    }

    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    window.addEventListener('load', onLoad);
    function onLoad(event) {
        initWebSocket();
    }
    function initWebSocket() {
        console.log('Trying to open a WebSocket connection...');
        websocket = new WebSocket(gateway);
        websocket.onopen = onOpen;
        websocket.onclose = onClose;
        websocket.onmessage = onMessage;
    }
    function onOpen(event) {
        console.log('Connection opened');
        websocket.send('{"action":"requestPZEM"}');
    }
    function onClose(event) {
        console.log('Connection closed');
        setTimeout(initWebSocket, 2000);
    }
    function onMessage(event) {
        var data = JSON.parse(event.data);
        if (data.type === "pzemData") {
            document.getElementById('voltage').innerHTML = data.voltage.toFixed(2);
            document.getElementById('current').innerHTML = data.current.toFixed(2);
            document.getElementById('power').innerHTML = data.power.toFixed(2);
            document.getElementById('energy').innerHTML = data.energy.toFixed(3);
            document.getElementById('frequency').innerHTML = data.frequency.toFixed(2);
            document.getElementById('powerFactor').innerHTML = data.powerFactor.toFixed(2);
        }
    }
</script>
</body></html>
)rawliteral";

// Settings Page
const char SETTINGS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
    <title>Settings</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f4; color: #333; }
        .header { background-color: #4CAF50; color: white; padding: 15px; text-align: center; }
        .navbar { background-color: #333; }
        .navbar a { color: white; padding: 14px 20px; text-decoration: none; display: inline-block; }
        .navbar a:hover { background-color: #ddd; color: black; }
        .container { padding: 20px; text-align: center; }
        .button { background-color: #008CBA; color: white; padding: 10px 15px; border-radius: 5px; text-decoration: none; margin-top: 15px; border: none; cursor: pointer; }
        .button:hover { background-color: #005f7c; }
        .factory-reset-btn { background-color: #f44336; }
        .factory-reset-btn:hover { background-color: #d32f2f; }
    </style>
</head>
<body>
    <div class="header">
        <h1>Settings</h1>
    </div>
    <div class="navbar">
        <a href="/">Home</a>
        <a href="/settings">Settings</a>
        <a href="/help">Help</a>
        <a href="/report">Report</a>
        <a href="#" onclick="logout()">Logout</a>
    </div>
    <div class="container">
        <h2>Energy Reset</h2>
        <p>Click the button below to reset the PZEM energy accumulator.</p>
        <button class="button" onclick="resetPZEM()">Reset PZEM Energy</button>
        <p id="resetMessage"></p>
        <h2>Factory Reset</h2>
        <p>Click the button below to reset all appliance data (current, energy, and on-time).</p>
        <button class="button factory-reset-btn" onclick="factoryReset()">Factory Reset</button>
        <p id="factoryResetMessage"></p>
    </div>
<script>
    // Check authentication
    if (sessionStorage.getItem('isAuthenticated') !== 'true') {
        window.location.href = '/login';
    }

    function logout() {
        sessionStorage.removeItem('isAuthenticated');
        window.location.href = '/login';
    }

    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    window.addEventListener('load', onLoad);
    function onLoad(event) {
        initWebSocket();
    }
    function initWebSocket() {
        console.log('Trying to open a WebSocket connection...');
        websocket = new WebSocket(gateway);
        websocket.onopen = onOpen;
        websocket.onclose = onClose;
        websocket.onmessage = onMessage;
    }
    function onOpen(event) {
        console.log('Connection opened');
    }
    function onClose(event) {
        console.log('Connection closed');
        setTimeout(initWebSocket, 2000);
    }
    function onMessage(event) {
        var data = JSON.parse(event.data);
        if (data.type === "resetStatus") {
            document.getElementById('resetMessage').innerHTML = data.message;
        } else if (data.type === "factoryResetStatus") {
            document.getElementById('factoryResetMessage').innerHTML = data.message;
        }
    }
    function resetPZEM() {
        websocket.send('{"action":"resetEnergy"}');
    }
    function factoryReset() {
        if (confirm("Are you sure you want to perform a factory reset? This will clear all appliance data.")) {
            websocket.send('{"action":"factoryReset"}');
        }
    }
</script>
</body></html>
)rawliteral";

// Help Page
const char HELP_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
    <title>Help</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f4; color: #333; }
        .header { background-color: #4CAF50; color: white; padding: 15px; text-align: center; }
        .navbar { background-color: #333; }
        .navbar a { color: white; padding: 14px 20px; text-decoration: none; display: inline-block; }
        .navbar a:hover { background-color: #ddd; color: black; }
        .container { padding: 20px; text-align: center; }
    </style>
</head>
<body>
    <div class="header">
        <h1>Help</h1>
    </div>
    <div class="navbar">
        <a href="/">Home</a>
        <a href="/settings">Settings</a>
        <a href="/help">Help</a>
        <a href="/report">Report</a>
        <a href="#" onclick="logout()">Logout</a>
    </div>
    <div class="container">
        <h2>Welcome to the Help Page!</h2>
        <p>This section will provide guidance on how to use your SmartHome system.</p>
        <p>For now, please refer to our documentation or contact support.</p>
    </div>
<script>
    // Check authentication
    if (sessionStorage.getItem('isAuthenticated') !== 'true') {
        window.location.href = '/login';
    }

    function logout() {
        sessionStorage.removeItem('isAuthenticated');
        window.location.href = '/login';
    }
</script>
</body></html>
)rawliteral";

// Report Page
const char REPORT_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
    <title>Smart Home Reports</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 0; padding: 0; background-color: #f4f4f4; color: #333; }
        .header { background-color: #4CAF50; color: white; padding: 15px; text-align: center; }
        .navbar { background-color: #333; }
        .navbar a { color: white; padding: 14px 20px; text-decoration: none; display: inline-block; }
        .navbar a:hover { background-color: #ddd; color: black; }
        .container { padding: 20px; display: flex; flex-wrap: wrap; justify-content: center; gap: 20px; }
        .card { background-color: white; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); padding: 20px; flex: 1 1 calc(45% - 40px); box-sizing: border-box; min-width: 300px; max-width: 500px; }
        .card h2 { color: #4CAF50; text-align: center; margin-bottom: 15px; font-size: 1.8em; }
        ul, ol { margin: 0; padding: 0; list-style: none; }
        li { display: flex; justify-content: space-between; align-items: center; padding: 10px 0; border-bottom: 1px solid #eee; }
        li:last-child { border-bottom: none; }
        .appliance-name { font-weight: bold; flex: 1; text-align: left; }
        .value { color: #008CBA; font-weight: bold; margin-left: 10px; }
        .appliance-units { margin-left: 10px; color: #666; }
        .ranking-list li { padding: 12px 0; }
        .number-rank { font-weight: bold; margin-right: 10px; color: #4CAF50; width: 30px; text-align: right; }
        .bill-estimate { margin-top: 20px; text-align: center; font-size: 1.2em; color: #4CAF50; font-weight: bold; }
        .graph-container { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 10px; }
        canvas { background: #fff; border: 1px solid #ccc; border-radius: 8px; padding: 5px; width: 100%; height: 100px; }
        @media (max-width: 600px) {
            .card { flex: 1 1 100%; max-width: 100%; }
            .appliance-name { font-size: 0.9em; }
            .value, .appliance-units { font-size: 0.9em; }
            .graph-container { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>Smart Home Reports</h1>
    </div>
    <div class="navbar">
        <a href="/">Home</a>
        <a href="/settings">Settings</a>
        <a href="/help">Help</a>
        <a href="/report">Report</a>
        <a href="#" onclick="logout()">Logout</a>
    </div>
    <div class="container">
        <div class="card">
            <h2>Appliance ON-Time</h2>
            <ul id="timeList">
                <li><span class="appliance-name">Light-1-Main:</span> <span class="value" id="onTime1">0h 0m</span></li>
                <li><span class="appliance-name">Light-2:</span> <span class="value" id="onTime2">0h 0m</span></li>
                <li><span class="appliance-name">Socket-1:</span> <span class="value" id="onTime3">0h 0m</span></li>
                <li><span class="appliance-name">TV:</span> <span class="value">N/A</span></li>
            </ul>
        </div>
        <div class="card">
            <h2>Units Consumed Today</h2>
            <ol id="unitsList" class="ranking-list"></ol>
            <div class="bill-estimate" id="billEstimate">Estimated Bill: Calculating...</div>
        </div>
        <div class="card">
            <h2>Real-Time Sensor Graphs</h2>
            <div class="graph-container">
                <canvas id="voltageChart"></canvas>
                <canvas id="currentChart"></canvas>
                <canvas id="powerChart"></canvas>
            </div>
        </div>
    </div>
<script>
    // Check authentication
    if (sessionStorage.getItem('isAuthenticated') !== 'true') {
        window.location.href = '/login';
    }

    function logout() {
        sessionStorage.removeItem('isAuthenticated');
        window.location.href = '/login';
    }

    // Initialize graph data arrays
    var voltageData = [], currentData = [], powerData = [];
    var maxPoints = 100;

    // Draw graph on canvas
    function drawGraph(canvasId, data, color, label) {
        var canvas = document.getElementById(canvasId);
        var ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);

        // Calculate max value for dynamic scaling
        var maxValue = Math.max(1, ...data) * 1.2; // Add 20% headroom
        if (data.length === 0) maxValue = 100; // Default if no data

        // Draw grid and labels
        ctx.strokeStyle = '#ccc';
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (var i = 0; i <= 4; i++) {
            var y = canvas.height - (i * canvas.height / 4);
            ctx.moveTo(0, y);
            ctx.lineTo(canvas.width, y);
            ctx.fillText((maxValue * i / 4).toFixed(1), 5, y - 5);
        }
        ctx.stroke();

        // Draw data line
        if (data.length > 1) {
            ctx.strokeStyle = color;
            ctx.lineWidth = 2;
            ctx.beginPath();
            for (var i = 0; i < data.length; i++) {
                var x = (i / (maxPoints - 1)) * canvas.width;
                var y = canvas.height - (data[i] / maxValue) * canvas.height;
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            }
            ctx.stroke();
        }

        // Draw label
        ctx.fillStyle = color;
        ctx.fillText(label, 5, 15);
    }

    // Add data to graph
    function addData(dataArray, value, canvasId, color, label) {
        console.log(`Adding ${label}: ${value}`); // Debug log
        dataArray.push(value);
        if (dataArray.length > maxPoints) dataArray.shift();
        drawGraph(canvasId, dataArray, color, label);
    }

    // WebSocket connection
    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;
    window.addEventListener('load', onLoad);
    function onLoad(event) {
        initWebSocket();
    }
    function initWebSocket() {
        console.log('Trying to open a WebSocket connection...');
        websocket = new WebSocket(gateway);
        websocket.onopen = onOpen;
        websocket.onclose = onClose;
        websocket.onmessage = onMessage;
    }
    function onOpen(event) {
        console.log('Connection opened');
        websocket.send('{"action":"requestReport"}');
        websocket.send('{"action":"requestPZEM"}');
    }
    function onClose(event) {
        console.log('Connection closed');
        setTimeout(initWebSocket, 2000);
    }
    function onMessage(event) {
        console.log('Received: ' + event.data); // Debug log
        var data = JSON.parse(event.data);
        if (data.type === "reportData") {
            document.getElementById('onTime1').innerHTML = formatTime(data.onTime1);
            document.getElementById('onTime2').innerHTML = formatTime(data.onTime2);
            document.getElementById('onTime3').innerHTML = formatTime(data.onTime3);
            var unitsList = [
                { name: "Light-1-Main", units: data.units1 },
                { name: "Light-2", units: data.units2 },
                { name: "Socket-1", units: data.units3 }
            ];
            unitsList.sort((a, b) => b.units - a.units);
            var unitsListHTML = "";
            unitsList.forEach((item, index) => {
                unitsListHTML += `<li><span class="number-rank">${index + 1}.</span><span class="appliance-name">${item.name}:</span> <span class="value">${item.units.toFixed(2)}</span><span class="appliance-units">kWh</span></li>`;
            });
            document.getElementById('unitsList').innerHTML = unitsListHTML;
            var totalEnergy = data.totalEnergy;
            var estimatedBill = totalEnergy * 22; // Daily energy * rate
            document.getElementById('billEstimate').innerHTML = `Estimated Bill This Month: RS${estimatedBill.toFixed(2)}`;
        } else if (data.type === "pzemData") {
            addData(voltageData, data.voltage || 0, 'voltageChart', 'blue', 'Voltage (V)');
            addData(currentData, data.current || 0, 'currentChart', 'green', 'Current (A)');
            addData(powerData, data.power || 0, 'powerChart', 'red', 'Power (W)');
        }
    }
    function formatTime(minutes) {
        if (minutes < 0) minutes = 0;
        var hours = Math.floor(minutes / 60);
        var mins = Math.floor(minutes % 60);
        return `${hours}h ${mins}m`;
    }
</script>
</body></html>
)rawliteral";

// WebSocket Event Handler
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.println("WebSocket client connected");
        StaticJsonDocument<200> doc;
        doc["type"] = "relayState";
        doc["relay1State"] = relay1State ? "ON" : "OFF";
        doc["relay2State"] = relay2State ? "ON" : "OFF";
        doc["relay3State"] = relay3State ? "ON" : "OFF";
        char buffer[200];
        serializeJson(doc, buffer);
        client->text(buffer);
        // Send schedule data
        StaticJsonDocument<300> scheduleDoc;
        scheduleDoc["type"] = "schedulesData";
        JsonArray schedulesArray = scheduleDoc.createNestedArray("schedules");
        for (int i = 0; i < 3; i++) {
            JsonObject schedule = schedulesArray.createNestedObject();
            schedule["hour"] = schedules[i].hour;
            schedule["minute"] = schedules[i].minute;
            schedule["ampm"] = schedules[i].ampm;
            schedule["active"] = schedules[i].active;
        }
        serializeJson(scheduleDoc, buffer);
        client->text(buffer);
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.println("WebSocket client disconnected");
    } else if (type == WS_EVT_DATA) {
        String message = "";
        for (size_t i = 0; i < len; i++) {
            message += (char)data[i];
        }
        Serial.println("Received WS message: " + message);

        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, message);
        if (error) {
            Serial.print(F("deserializeJson() failed: "));
            Serial.println(error.c_str());
            return;
        }

        String action = doc["action"];
        if (action == "toggleRelay") {
            int relayNum = doc["relay"].as<int>();
            String state = doc["state"];
            if (!lowVoltageAlert) {
                if (relayNum == 1) {
                    relay1State = (state == "ON");
                    digitalWrite(relay1Pin, relay1State ? HIGH : LOW);
                } else if (relayNum == 2) {
                    relay2State = (state == "ON");
                    digitalWrite(relay2Pin, relay2State ? HIGH : LOW);
                } else if (relayNum == 3) {
                    relay3State = (state == "ON");
                    digitalWrite(relay3Pin, relay3State ? HIGH : LOW);
                }
                StaticJsonDocument<200> responseDoc;
                responseDoc["type"] = "relayState";
                responseDoc["relay1State"] = relay1State ? "ON" : "OFF";
                responseDoc["relay2State"] = relay2State ? "ON" : "OFF";
                responseDoc["relay3State"] = relay3State ? "ON" : "OFF";
                char buffer[200];
                serializeJson(responseDoc, buffer);
                ws.textAll(buffer);
            }
        } else if (action == "requestData") {
            sendSensorData();
        } else if (action == "requestPZEM") {
            sendPZEMDataToClients();
        } else if (action == "requestStatus") {
            sendStatusToClients();
        } else if (action == "requestReport") {
            sendReportData();
        } else if (action == "resetEnergy") {
            bool success = pzem.resetEnergy();
            StaticJsonDocument<200> responseDoc;
            responseDoc["type"] = "resetStatus";
            responseDoc["message"] = success ? "PZEM Energy Reset Successful!" : "PZEM Reset Failed!";
            char buffer[200];
            serializeJson(responseDoc, buffer);
            ws.textAll(buffer);
        } else if (action == "setSchedule") {
            int relayNum = doc["relay"].as<int>();
            int hour = doc["hour"].as<int>();
            int minute = doc["minute"].as<int>();
            String ampm = doc["ampm"].as<String>();
            if (relayNum >= 1 && relayNum <= 3 && (ampm == "AM" || ampm == "PM")) {
                schedules[relayNum - 1].hour = hour;
                schedules[relayNum - 1].minute = minute;
                schedules[relayNum - 1].ampm = ampm;
                schedules[relayNum - 1].active = true;
                Serial.printf("Schedule set for relay %d at %02d:%02d %s\n", relayNum, hour, minute, ampm.c_str());
                StaticJsonDocument<200> responseDoc;
                responseDoc["type"] = "scheduleUpdate";
                responseDoc["relay"] = relayNum;
                responseDoc["hour"] = hour;
                responseDoc["minute"] = minute;
                responseDoc["ampm"] = ampm;
                char buffer[200];
                serializeJson(responseDoc, buffer);
                ws.textAll(buffer);
            }
        } else if (action == "requestSchedules") {
            StaticJsonDocument<300> scheduleDoc;
            scheduleDoc["type"] = "schedulesData";
            JsonArray schedulesArray = scheduleDoc.createNestedArray("schedules");
            for (int i = 0; i < 3; i++) {
                JsonObject schedule = schedulesArray.createNestedObject();
                schedule["hour"] = schedules[i].hour;
                schedule["minute"] = schedules[i].minute;
                schedule["ampm"] = schedules[i].ampm;
                schedule["active"] = schedules[i].active;
            }
            char buffer[300];
            serializeJson(scheduleDoc, buffer);
            client->text(buffer);
        } else if (action == "factoryReset") {
            energy1 = 0.0;
            energy2 = 0.0;
            energy3 = 0.0;
            totalOnTime1 = 0.0;
            totalOnTime2 = 0.0;
            totalOnTime3 = 0.0;
            for (int i = 0; i < 3; i++) {
                schedules[i].hour = 0;
                schedules[i].minute = 0;
                schedules[i].ampm = "";
                schedules[i].active = false;
            }
            saveToEEPROM();
            StaticJsonDocument<200> responseDoc;
            responseDoc["type"] = "factoryResetStatus";
            responseDoc["message"] = "Factory Reset Successful!";
            char buffer[200];
            serializeJson(responseDoc, buffer);
            ws.textAll(buffer);
            // Send updated schedules after reset
            StaticJsonDocument<300> scheduleDoc;
            scheduleDoc["type"] = "schedulesData";
            JsonArray schedulesArray = scheduleDoc.createNestedArray("schedules");
            for (int i = 0; i < 3; i++) {
                JsonObject schedule = schedulesArray.createNestedObject();
                schedule["hour"] = schedules[i].hour;
                schedule["minute"] = schedules[i].minute;
                schedule["ampm"] = schedules[i].ampm;
                schedule["active"] = schedules[i].active;
            }
            serializeJson(scheduleDoc, buffer);
            ws.textAll(buffer);
        }
    }
}

// Send Sensor Data to WebSocket
void sendSensorData() {
    StaticJsonDocument<500> doc;
    doc["type"] = "sensorData";
    doc["current1"] = readRMSCurrent(currentPin1, offset1);
    doc["energy1"] = energy1;
    doc["current2"] = readRMSCurrent(currentPin2, offset2);
    doc["energy2"] = energy2;
    doc["current3"] = readRMSCurrent(currentPin3, offset3);
    doc["energy3"] = energy3;
    doc["currentTV"] = 0.0;
    doc["energyTV"] = 0.0;
    char buffer[500];
    serializeJson(doc, buffer);
    ws.textAll(buffer);
}

// Send PZEM Data to WebSocket
void sendPZEMDataToClients() {
    StaticJsonDocument<500> doc;
    doc["type"] = "pzemData";
    float voltage = pzem.voltage();
    float current = pzem.current();
    float power = pzem.power();
    float energy = pzem.energy();
    float frequency = pzem.frequency();
    float pf = pzem.pf();
    if (!isnan(voltage) && !isnan(current) && !isnan(power) && !isnan(energy) && !isnan(frequency) && !isnan(pf)) {
        doc["voltage"] = voltage;
        doc["current"] = current;
        doc["power"] = power;
        doc["energy"] = energy;
        doc["frequency"] = frequency;
        doc["powerFactor"] = pf;
    } else {
        doc["voltage"] = 0.0;
        doc["current"] = 0.0;
        doc["power"] = 0.0;
        doc["energy"] = 0.0;
        doc["frequency"] = 0.0;
        doc["powerFactor"] = 0.0;
    }
    char buffer[500];
    serializeJson(doc, buffer);
    ws.textAll(buffer);
}

// Send Report Data to WebSocket
void sendReportData() {
    StaticJsonDocument<500> doc;
    doc["type"] = "reportData";
    doc["onTime1"] = totalOnTime1;
    doc["onTime2"] = totalOnTime2;
    doc["onTime3"] = totalOnTime3;
    doc["units1"] = energy1 / 1000.0;
    doc["units2"] = energy2 / 1000.0;
    doc["units3"] = energy3 / 1000.0;
    doc["totalEnergy"] = pzem.energy(); // Total energy from PZEM in kWh
    char buffer[500];
    serializeJson(doc, buffer);
    ws.textAll(buffer);
}

// Send Overall System Status to WebSocket
void sendStatusToClients() {
    StaticJsonDocument<500> doc;
    doc["type"] = "statusUpdate";
    float totalCurrent = pzem.current();
    if (isnan(totalCurrent) || totalCurrent < 0) {
        totalCurrent = 0.0;
    }
    if (totalCurrent > 5.0) {
        doc["overallLoad"] = "High Load (" + String(totalCurrent, 2) + " A)";
    } else if (totalCurrent > 1.0) {
        doc["overallLoad"] = "Medium Load (" + String(totalCurrent, 2) + " A)";
    } else {
        doc["overallLoad"] = "Low Load (" + String(totalCurrent, 2) + " A)";
    }
    float voltage = pzem.voltage();
    float frequency = pzem.frequency();
    float pf = pzem.pf();
    bool faultDetected = false;
    String faultStatus = "OK";
    if (isnan(frequency) || frequency == 0) {
        faultDetected = true;
        faultStatus = "No Frequency Data";
    } else if (frequency < 48.0 || frequency > 52.0) {
        faultDetected = true;
        faultStatus = "Frequency Out of Range";
    }
    if (!isnan(pf) && pf < 0.5 && pf > 0.0) {
        faultDetected = true;
        if (faultStatus == "OK") {
            faultStatus = "Low Power Factor";
        } else {
            faultStatus += ", Low Power Factor";
        }
    }
    float pzemCurrent = pzem.current();
    if (!isnan(pzemCurrent) && pzemCurrent > 20.0) {
        faultDetected = true;
        if (faultStatus == "OK") {
            faultStatus = "Overcurrent Main";
        } else {
            faultStatus += ", Overcurrent Main";
        }
    }
    doc["faultStatus"] = faultStatus;
    doc["voltageStatus"] = (!isnan(voltage) && voltage < VOLTAGE_THRESHOLD && voltage > 0) ? "Very Low Voltage (" + String(voltage, 1) + " V)" : "Normal Voltage (" + String(voltage, 1) + " V)";
    char buffer[500];
    serializeJson(doc, buffer);
    ws.textAll(buffer);
}

// Save data to EEPROM
void saveToEEPROM() {
    EEPROM.put(ADDR_ENERGY1, energy1);
    EEPROM.put(ADDR_ENERGY2, energy2);
    EEPROM.put(ADDR_ENERGY3, energy3);
    EEPROM.put(ADDR_ONTIME1, totalOnTime1);
    EEPROM.put(ADDR_ONTIME2, totalOnTime2);
    EEPROM.put(ADDR_ONTIME3, totalOnTime3);
    unsigned long currentTime = timeClient.getEpochTime();
    EEPROM.put(ADDR_LAST_RESET, currentTime);
    EEPROM.commit();
}

// Load data from EEPROM
void loadFromEEPROM() {
    EEPROM.get(ADDR_ENERGY1, energy1);
    EEPROM.get(ADDR_ENERGY2, energy2);
    EEPROM.get(ADDR_ENERGY3, energy3);
    EEPROM.get(ADDR_ONTIME1, totalOnTime1);
    EEPROM.get(ADDR_ONTIME2, totalOnTime2);
    EEPROM.get(ADDR_ONTIME3, totalOnTime3);
    unsigned long lastReset;
    EEPROM.get(ADDR_LAST_RESET, lastReset);
    
    // Validate loaded data
    if (isnan(energy1) || energy1 < 0) energy1 = 0.0;
    if (isnan(energy2) || energy2 < 0) energy2 = 0.0;
    if (isnan(energy3) || energy3 < 0) energy3 = 0.0;
    if (isnan(totalOnTime1) || totalOnTime1 < 0) totalOnTime1 = 0.0;
    if (isnan(totalOnTime2) || totalOnTime2 < 0) totalOnTime2 = 0.0;
    if (isnan(totalOnTime3) || totalOnTime3 < 0) totalOnTime3 = 0.0;

    // Check if daily reset is needed
    if (lastReset != 0) {
        time_t now = timeClient.getEpochTime();
        struct tm *timeInfo = localtime(&now);
        struct tm *lastResetTime = localtime((time_t*)&lastReset);
        if (timeInfo->tm_mday != lastResetTime->tm_mday || timeInfo->tm_mon != lastResetTime->tm_mon || timeInfo->tm_year != lastResetTime->tm_year) {
            // New day, reset data
            energy1 = 0.0;
            energy2 = 0.0;
            energy3 = 0.0;
            totalOnTime1 = 0.0;
            totalOnTime2 = 0.0;
            totalOnTime3 = 0.0;
            saveToEEPROM();
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== PZEM-004T & ACS712 Multi-Current Monitor ===");
    
    // Initialize EEPROM
    EEPROM.begin(EEPROM_SIZE);
    
    // Initialize pins
    pinMode(relay1Pin, OUTPUT);
    pinMode(relay2Pin, OUTPUT);
    pinMode(relay3Pin, OUTPUT);
    digitalWrite(relay1Pin, HIGH);
    digitalWrite(relay2Pin, HIGH);
    digitalWrite(relay3Pin, HIGH);
    
    // Initialize Serial2 for PZEM
    Serial2.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);
    
    // Calibrate ACS712 sensors
    Serial.println("Calibrating zero-current offset for ACS712 sensors...");
    offset1 = calibrateOffset(currentPin1);
    offset2 = calibrateOffset(currentPin2);
    offset3 = calibrateOffset(currentPin3);
    Serial.println("=== ACS712 Offset Voltages ===");
    Serial.print("Sensor 1: "); Serial.println(offset1, 3);
    Serial.print("Sensor 2: "); Serial.println(offset2, 3);
    Serial.print("Sensor 3: "); Serial.println(offset3, 3);
    Serial.println("=====================================");
    
    lastUpdateTime = millis();
    
    // Connect to WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi...");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    setupOLED();
    
    // Initialize NTP client
    timeClient.begin();
    timeClient.update();
    
    // Load data from EEPROM
    loadFromEEPROM();
    
    // Setup web server routes
    server.on("/login", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", LOGIN_HTML);
    });

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", INDEX_HTML);
    });

    server.on("/room1/stats", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", ROOM1_stats_HTML);
    });

    server.on("/room1", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", ROOM1_CONTROL_HTML);
    });

    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", SETTINGS_HTML);
    });

    server.on("/help", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", HELP_PAGE);
    });

    server.on("/report", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", REPORT_PAGE);
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not Found");
    });
    
    // Setup WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.begin();
    Serial.println("Server started");
}

void loop() {
    static unsigned long lastPingTime = 0;
    static unsigned long lastDailySummaryTime = 0;
    static unsigned long lastWsSendTime = 0;
    static unsigned long lastWiFiCheckTime = 0;
    static unsigned long lastEEPROMWriteTime = 0;
    const unsigned long pingInterval = 5000;
    const unsigned long wsSendInterval = 1000;
    const unsigned long dailySummaryInterval = 10000;
    const unsigned long wifiCheckInterval = 10000;
    const unsigned long eepromWriteInterval = 30000; // Save to EEPROM every 30 seconds
    
    // Update NTP client
    timeClient.update();
    
    // Check Wi-Fi connection periodically
    if (millis() - lastWiFiCheckTime > wifiCheckInterval) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Wi-Fi disconnected. Reconnecting...");
            WiFi.reconnect();
            unsigned long startTime = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - startTime < 5000) {
                delay(500);
                Serial.print(".");
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("\nWiFi reconnected.");
                Serial.print("IP Address: ");
                Serial.println(WiFi.localIP());
                timeClient.update(); // Re-sync time after reconnect
            } else {
                Serial.println("\nFailed to reconnect to WiFi.");
            }
        }
        lastWiFiCheckTime = millis();
    }
    
    // Check for daily reset
    unsigned long currentTime = timeClient.getEpochTime();
    struct tm *timeInfo = localtime((time_t*)&currentTime);
    unsigned long lastReset;
    EEPROM.get(ADDR_LAST_RESET, lastReset);
    struct tm *lastResetTime = localtime((time_t*)&lastReset);
    if (lastReset == 0 || timeInfo->tm_mday != lastResetTime->tm_mday || 
        timeInfo->tm_mon != lastResetTime->tm_mon || timeInfo->tm_year != lastResetTime->tm_year) {
        energy1 = 0.0;
        energy2 = 0.0;
        energy3 = 0.0;
        totalOnTime1 = 0.0;
        totalOnTime2 = 0.0;
        totalOnTime3 = 0.0;
        saveToEEPROM();
        Serial.println("Daily reset performed.");
    }
    
    // Check voltage and control relays
    float voltage = pzem.voltage();
    if (!isnan(voltage) && voltage < VOLTAGE_THRESHOLD && voltage > 0) {
        if (!lowVoltageAlert) {
            lowVoltageAlert = true;
            relay1State = false;
            relay2State = false;
            relay3State = false;
            digitalWrite(relay1Pin, LOW);
            digitalWrite(relay2Pin, LOW);
            digitalWrite(relay3Pin, LOW);
            StaticJsonDocument<200> doc;
            doc["type"] = "relayState";
            doc["relay1State"] = "OFF";
            doc["relay2State"] = "OFF";
            doc["relay3State"] = "OFF";
            char buffer[200];
            serializeJson(doc, buffer);
            ws.textAll(buffer);
            Serial.println("Low voltage detected. All relays turned OFF.");
        }
    } else {
        lowVoltageAlert = false;
    }
    
    // Check schedules
    static unsigned long lastScheduleCheck = 0;
    const unsigned long scheduleCheckInterval = 1000; // Check every second
    if (!lowVoltageAlert && millis() - lastScheduleCheck >= scheduleCheckInterval) {
      struct tm *timeInfo = localtime((time_t*)&currentTime);
      char timeStr[32];
      strftime(timeStr, sizeof(timeStr), "%I:%M:%S %p", timeInfo);
      Serial.printf("Current Time: %s\n", timeStr);
      for (int i = 0; i < 3; i++) {
        if (schedules[i].active && schedules[i].hour != 0 && schedules[i].minute != -1 && schedules[i].ampm != "") {
            int scheduleHour = schedules[i].hour;
            if (schedules[i].ampm == "PM" && scheduleHour != 12) {
                scheduleHour += 12;
            } else if (schedules[i].ampm == "AM" && scheduleHour == 12) {
                scheduleHour = 0;
            }
            if (timeInfo->tm_hour == scheduleHour && timeInfo->tm_min == schedules[i].minute && timeInfo->tm_sec < 2) {
                bool *relayState;
                int relayPin;
                if (i == 0) {
                    relayState = &relay1State;
                    relayPin = relay1Pin;
                } else if (i == 1) {
                    relayState = &relay2State;
                    relayPin = relay2Pin;
                } else {
                    relayState = &relay3State;
                    relayPin = relay3Pin;
                }
                // Toggle relay state
                *relayState = !*relayState;
                digitalWrite(relayPin, *relayState ? HIGH : LOW);
                StaticJsonDocument<200> doc;
                doc["type"] = "relayState";
                doc["relay1State"] = relay1State ? "ON" : "OFF";
                doc["relay2State"] = relay2State ? "ON" : "OFF";
                doc["relay3State"] = relay3State ? "ON" : "OFF";
                char buffer[200];
                serializeJson(doc, buffer);
                ws.textAll(buffer);
                Serial.printf("Relay %d toggled to %s by schedule at %02d:%02d %s\n", i + 1, *relayState ? "ON" : "OFF", schedules[i].hour, schedules[i].minute, schedules[i].ampm.c_str());
                
                // Deactivate schedule after execution
                schedules[i].hour = 0;
                schedules[i].minute = 0;
                schedules[i].ampm = "";
                schedules[i].active = false;

                // Send updated schedule to clients
                StaticJsonDocument<200> responseDoc;
                responseDoc["type"] = "scheduleUpdate";
                responseDoc["relay"] = i + 1;
                responseDoc["hour"] = schedules[i].hour;
                responseDoc["minute"] = schedules[i].minute;
                responseDoc["ampm"] = schedules[i].ampm;
                serializeJson(responseDoc, buffer);
                ws.textAll(buffer);
            }
        }
    }
    lastScheduleCheck = millis();
}
    
    if (millis() - lastPingTime > pingInterval) {
        printPZEMData();
        lastPingTime = millis();
    }
    
    float current1 = readRMSCurrent(currentPin1, offset1);
    float current2 = readRMSCurrent(currentPin2, offset2);
    float current3 = readRMSCurrent(currentPin3, offset3);
    
    unsigned long currentMillis = millis();
    float timeDeltaHours = (currentMillis - lastUpdateTime) / 3600000.0;
    
    if (!isnan(voltage)) {
        energy1 += voltage * current1 * timeDeltaHours;
        energy2 += voltage * current2 * timeDeltaHours;
        energy3 += voltage * current3 * timeDeltaHours;
    }
    lastUpdateTime = currentMillis;
    
    if (current1 > NOISE_THRESHOLD) {
        if (!appliance1_was_on) {
            applianceOnStartTime1 = currentMillis;
            appliance1_was_on = true;
        } else {
            totalOnTime1 += (currentMillis - applianceOnStartTime1) / 60000.0;
            applianceOnStartTime1 = currentMillis;
        }
    } else {
        if (appliance1_was_on) {
            totalOnTime1 += (currentMillis - applianceOnStartTime1) / 60000.0;
            applianceOnStartTime1 = 0;
            appliance1_was_on = false;
        }
    }
    
    if (current2 > NOISE_THRESHOLD) {
        if (!appliance2_was_on) {
            applianceOnStartTime2 = currentMillis;
            appliance2_was_on = true;
        } else {
            totalOnTime2 += (currentMillis - applianceOnStartTime2) / 60000.0;
            applianceOnStartTime2 = currentMillis;
        }
    } else {
        if (appliance2_was_on) {
            totalOnTime2 += (currentMillis - applianceOnStartTime2) / 60000.0;
            applianceOnStartTime2 = 0;
            appliance2_was_on = false;
        }
    }
    
    if (current3 > NOISE_THRESHOLD) {
        if (!appliance3_was_on) {
            applianceOnStartTime3 = currentMillis;
            appliance3_was_on = true;
        } else {
            totalOnTime3 += (currentMillis - applianceOnStartTime3) / 60000.0;
            applianceOnStartTime3 = currentMillis;
        }
    } else {
        if (appliance3_was_on) {
            totalOnTime3 += (currentMillis - applianceOnStartTime3) / 60000.0;
            applianceOnStartTime3 = 0;
            appliance3_was_on = false;
        }
    }
    
    Serial.println("--- Appliance Status ---");
    printApplianceStatus(1, current1, energy1);
    printApplianceStatus(2, current2, energy2);
    printApplianceStatus(3, current3, energy3);
    
    if (current1 < NOISE_THRESHOLD && current2 < NOISE_THRESHOLD && current3 < NOISE_THRESHOLD) {
        if (millis() - lastLowCurrentTime > STABLE_RECAL_TIME * 1000UL) {
            Serial.println("Low currents detected. Recalibrating ACS712 offsets...");
            offset1 = calibrateOffset(currentPin1);
            offset2 = calibrateOffset(currentPin2);
            offset3 = calibrateOffset(currentPin3);
            Serial.print("New offsets: ");
            Serial.print(offset1, 3);
            Serial.print(", ");
            Serial.print(offset2, 3);
            Serial.print(", ");
            Serial.println(offset3, 3);
            lastLowCurrentTime = millis();
        }
    } else {
        lastLowCurrentTime = millis();
    }
    
    // Periodically save to EEPROM
    if (millis() - lastEEPROMWriteTime > eepromWriteInterval) {
        saveToEEPROM();
        lastEEPROMWriteTime = millis();
    }
    
    if (millis() - lastWsSendTime > wsSendInterval) {
        if (WiFi.status() == WL_CONNECTED) {
            sendSensorData();
            sendPZEMDataToClients();
            sendStatusToClients();
            sendReportData();
            
            influxSender.sendApplianceMetrics("light1-main", current1, energy1);
            influxSender.sendApplianceMetrics("light2", current2, energy2);
            influxSender.sendApplianceMetrics("socket1", current3, energy3);
            
            float pzemCurrent = pzem.current();
            float pzemPower = pzem.power();
            float pzemEnergy = pzem.energy();
            float pzemFrequency = pzem.frequency();
            float pzemPf = pzem.pf();
            if (!isnan(voltage) && !isnan(pzemCurrent) && !isnan(pzemPower) && !isnan(pzemEnergy) && !isnan(pzemFrequency) && !isnan(pzemPf)) {
                influxSender.sendHomeMetrics(voltage, pzemCurrent, pzemPower, pzemEnergy, pzemFrequency, pzemPf);
            }
            
            if (millis() - lastDailySummaryTime > dailySummaryInterval) {
                influxSender.sendDailySummary("light1-main", totalOnTime1 / 60.0, energy1 / 1000.0);
                influxSender.sendDailySummary("light2", totalOnTime2 / 60.0, energy2 / 1000.0);
                influxSender.sendDailySummary("socket1", totalOnTime3 / 60.0, energy3 / 1000.0);
                lastDailySummaryTime = millis();
            }
        } else {
            Serial.println("Wi-Fi disconnected. Skipping data send.");
        }
        lastWsSendTime = millis();
    }
    
    delay(100);
}

float calibrateOffset(int pin) {
    long totalADC = 0;
    for (int i = 0; i < numSamples; i++) {
        totalADC += analogRead(pin);
        delayMicroseconds(100);
    }
    float avgADC = totalADC / (float)numSamples;
    float voltage = (avgADC / ADC_MAX) * VOLTAGE_REF;
    return voltage;
}

float readRMSCurrent(int pin, float offsetVoltage) {
    float sumSquares = 0;
    for (int i = 0; i < numSamples; i++) {
        int sample = analogRead(pin);
        float voltage = (sample / ADC_MAX) * VOLTAGE_REF;
        float diff = voltage - offsetVoltage;
        sumSquares += diff * diff;
        delayMicroseconds(100);
    }
    float meanSquare = sumSquares / numSamples;
    float rmsVoltage = sqrt(meanSquare);
    float current = rmsVoltage / ACS_SENSITIVITY;
    if (current < NOISE_THRESHOLD) {
        current = 0.0;
    }
    return current;
}

void printPZEMData() {
    Serial.println("=== PZEM-004T Sensor Data ===");
    float voltage = pzem.voltage();
    float current = pzem.current();
    float power = pzem.power();
    float energy = pzem.energy();
    float frequency = pzem.frequency();
    float pf = pzem.pf();
    if (isnan(voltage)) {
        Serial.println("No voltage data");
    } else {
        Serial.print("Voltage: "); Serial.print(voltage); Serial.println(" V");
    }
    if (isnan(current)) {
        Serial.println("No current data");
    } else {
        Serial.print("Current: "); Serial.print(current); Serial.println(" A");
    }
    if (isnan(power)) {
        Serial.println("No power data");
    } else {
        Serial.print("Power: "); Serial.print(power); Serial.println(" W");
    }
    if (isnan(energy)) {
        Serial.println("No energy data");
    } else {
        Serial.print("Energy: "); Serial.print(energy); Serial.println(" kWh");
    }
    if (isnan(frequency)) {
        Serial.println("No frequency data");
    } else {
        Serial.print("Frequency: "); Serial.print(frequency); Serial.println(" Hz");
    }
    if (isnan(pf)) {
        Serial.println("No power factor data");
    } else {
        Serial.print("Power Factor: "); Serial.println(pf);
    }
    Serial.println("---");
}

void printApplianceStatus(int applianceNum, float current, float energy) {
    Serial.print("Appliance ");
    Serial.print(applianceNum);
    Serial.print(": ");
    Serial.print(current, 2);
    Serial.print(" A | ");
    Serial.print(energy, 4);
    Serial.print(" Wh | ");
    Serial.println(current > NOISE_THRESHOLD ? "ON" : "OFF");
}