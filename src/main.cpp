#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "DHT.h"
#include <EEPROM.h>

// WIFI
const char* ssid = "iPhone";
const char* password = "1234567890";

// THINGSBOARD
const char* mqtt_server = "demo.thingsboard.io";
const char* token = "ju7vl5WQZY29H61RRgnM";

WiFiClient espClient;
PubSubClient client(espClient);

// CẤU HÌNH CHÂN
#define DHTPIN 15
#define DHTTYPE DHT11

#define MQ135_PIN 35

// GP2Y1010AU0F
#define DUST_LED_PIN 14
#define DUST_OUT_PIN 33

// LED AQI
#define LED_BLUE   19
#define LED_GREEN  18
#define LED_YELLOW 5
#define LED_RED    4

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// KHỞI TẠO
DHT dht(DHTPIN, DHTTYPE);
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// EEPROM (lưu baseline)
#define EEPROM_SIZE 64
#define BASE_ADDR 0
#define BASE_FLAG_ADDR 4

// BIẾN TOÀN CỤC
float global_temp = 0.0;
float global_hum = 0.0;
float global_gas = 0.0;
float global_pm25 = 0.0;
int global_aqi = 0;

// AQI AVERAGE
float pm25Sum = 0;
int pm25Count = 0;
unsigned long lastAQIUpdate = 0;

SemaphoreHandle_t xDataMutex;

// BASELINE CHO GP2Y
bool baselineReady = false;
float initBuffer[200];
int initIndex = 0;

// Các bộ lọc 
float vFast = 0;
float vMedian[5] = {0};
int medianIdx = 0;
float baseline = 0.0;
#define ALPHA_FAST 0.2

// Hàm đọc/lưu baseline từ EEPROM
bool loadBaselineFromEEPROM() {
    EEPROM.begin(EEPROM_SIZE);
    byte flag;
    EEPROM.get(BASE_FLAG_ADDR, flag);
    if (flag == 0x01) {
        EEPROM.get(BASE_ADDR, baseline);
        Serial.print("Loaded baseline from EEPROM: ");
        Serial.println(baseline, 4);
        return true;
    }
    return false;
}

void saveBaselineToEEPROM() {
    EEPROM.put(BASE_ADDR, baseline);
    EEPROM.put(BASE_FLAG_ADDR, (byte)0x01);
    EEPROM.commit();
    Serial.println("Baseline saved to EEPROM.");
}

// Học baseline (10 phút)
void initBaseline(float v) {
    if (baselineReady) return;
    initBuffer[initIndex++] = v;
    if (initIndex >= 200) {
        float temp[200];
        memcpy(temp, initBuffer, sizeof(initBuffer));
        for (int i = 0; i < 199; i++) {
            for (int j = i+1; j < 200; j++) {
                if (temp[i] > temp[j]) {
                    float t = temp[i];
                    temp[i] = temp[j];
                    temp[j] = t;
                }
            }
        }
        int idx = 200 * 10 / 100; // 10th percentile
        baseline = temp[idx];
        baselineReady = true;
        Serial.print("Baseline learned (10 minutes): ");
        Serial.println(baseline, 4);
        saveBaselineToEEPROM();
        // Reset AQI buffer để không bị ảnh hưởng bởi các giá trị 0 trong lúc học
        pm25Sum = 0;
        pm25Count = 0;
        lastAQIUpdate = millis();
        Serial.println("AQI buffer reset.");
    }
}

// HÀM ĐỌC BỤI
float readDustUG(float humidity) {
    long sum = 0;
    for (int i = 0; i < 20; i++) {
        digitalWrite(DUST_LED_PIN, LOW);
        delayMicroseconds(280);
        sum += analogRead(DUST_OUT_PIN);
        delayMicroseconds(40);
        digitalWrite(DUST_LED_PIN, HIGH);
        delayMicroseconds(9680);
    }
    float adcAvg = sum / 20.0;
    float voltage = adcAvg * (3.3 / 4095.0);

    // EMA filter
    vFast = ALPHA_FAST * voltage + (1 - ALPHA_FAST) * vFast;

    // Median filter (5 mẫu)
    vMedian[medianIdx] = vFast;
    medianIdx = (medianIdx + 1) % 5;
    float temp[5];
    memcpy(temp, vMedian, sizeof(vMedian));
    for (int i = 0; i < 4; i++)
        for (int j = i+1; j < 5; j++)
            if (temp[i] > temp[j]) {
                float t = temp[i];
                temp[i] = temp[j];
                temp[j] = t;
            }
    float vFiltered = temp[2];

    // Nếu chưa có baseline (lần đầu hoặc vừa xóa EEPROM) thì học
    if (!baselineReady) {
        initBaseline(vFiltered);
        return 0;   // trong thời gian học, trả về 0
    }

    // Tính điện áp do bụi
    float vDust = vFiltered - baseline;
    if (vDust < 0) vDust = 0;

    float dust_mg = (vDust / 0.5) * 0.1;
    float dust_ug = dust_mg * 1000.0;

    // Bù ẩm
    if (humidity > 70.0f) {
        float correction = 1.0f + 0.01f * (humidity - 70.0f);
        dust_ug /= correction;
    }

    if (dust_ug > 800) dust_ug = 800;
    return dust_ug;
}

// CÁC HÀM KHÁC (WiFi, MQTT, AQI, LED, OLED)
void connectWiFi() {
    WiFi.begin(ssid, password);
    Serial.print("Dang ket noi WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
}

void reconnectMQTT() {
    if (client.connected()) return;
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    Serial.print("Dang ket noi ThingsBoard...");
    String clientId = "ESP32-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), token, NULL)) {
        Serial.println("Thanh cong!");
    } else {
        Serial.print("That bai rc=");
        Serial.println(client.state());
    }
}

void sendToThingsBoard(float t, float h, float pm25, int aqi, float gas) {
    String payload = "{";
    payload += "\"temperature\":"; payload += String(t, 1);
    payload += ",\"humidity\":";    payload += String(h, 1);
    payload += ",\"pm25\":";        payload += String(pm25, 1);
    payload += ",\"aqi\":";         payload += String(aqi);
    payload += ",\"gas\":";         payload += String(gas, 1);
    payload += ",\"baseline\":";    payload += String(baseline, 4);
    payload += "}";
    if (client.connected()) client.publish("v1/devices/me/telemetry", payload.c_str());
    Serial.println("Gui ThingsBoard:");
    Serial.println(payload);
}

float truncatePM25(float value) {
    return floor(value * 10.0) / 10.0;
}

int calculateAQI(float pm25) {
    float BP_Lo, BP_Hi;
    int I_Lo, I_Hi;
    if (pm25 >= 0.0 && pm25 <= 9.0) { BP_Lo=0.0; BP_Hi=9.0; I_Lo=0; I_Hi=50; }
    else if (pm25 <= 35.4) { BP_Lo=9.1; BP_Hi=35.4; I_Lo=51; I_Hi=100; }
    else if (pm25 <= 55.4) { BP_Lo=35.5; BP_Hi=55.4; I_Lo=101; I_Hi=150; }
    else if (pm25 <= 125.4) { BP_Lo=55.5; BP_Hi=125.4; I_Lo=151; I_Hi=200; }
    else if (pm25 <= 225.4) { BP_Lo=125.5; BP_Hi=225.4; I_Lo=201; I_Hi=300; }
    else { BP_Lo=225.5; BP_Hi=500.4; I_Lo=301; I_Hi=500; }
    float aqi = ((I_Hi - I_Lo) / (BP_Hi - BP_Lo)) * (pm25 - BP_Lo) + I_Lo;
    return round(aqi);
}

void updateAQILED(int aqi) {
    digitalWrite(LED_BLUE, LOW);
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_YELLOW, LOW);
    digitalWrite(LED_RED, LOW);
    if (aqi <= 50) digitalWrite(LED_BLUE, HIGH);
    else if (aqi <= 100) digitalWrite(LED_GREEN, HIGH);
    else if (aqi <= 200) digitalWrite(LED_YELLOW, HIGH);
    else digitalWrite(LED_RED, HIGH);
}

// TASK ĐỌC CẢM BIẾN
void TaskReadSensors(void *pvParameters) {
    for (;;) {
        float h = dht.readHumidity();
        float t = dht.readTemperature();
        if (isnan(h) || isnan(t)) {
            Serial.println("Loi doc DHT11!");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }
        int adc = analogRead(MQ135_PIN);
        float gas_percent = (adc / 4095.0) * 100.0;
        float dust_ug = readDustUG(h);   // giá trị tức thời (chỉ dùng để cộng dồn)

        static int aqi = 0;
        pm25Sum += dust_ug;
        pm25Count++;
        if (millis() - lastAQIUpdate >= 15000) {
            float pm25Avg = pm25Sum / pm25Count;
            pm25Avg = truncatePM25(pm25Avg);
            aqi = calculateAQI(pm25Avg);
            updateAQILED(aqi);
            sendToThingsBoard(t, h, pm25Avg, aqi, gas_percent);
            if (xSemaphoreTake(xDataMutex, portMAX_DELAY) == pdTRUE) {
                global_pm25 = pm25Avg;
                xSemaphoreGive(xDataMutex);
            }
            pm25Sum = 0;
            pm25Count = 0;
            lastAQIUpdate = millis();
        }

        if (xSemaphoreTake(xDataMutex, portMAX_DELAY) == pdTRUE) {
            global_hum = h;
            global_temp = t;
            global_gas = gas_percent;
            global_aqi = aqi;
            xSemaphoreGive(xDataMutex);
        }
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

// TASK HIỂN THỊ
void TaskDisplay(void *pvParameters) {
    char buf[32];
    for (;;) {
        float t, h, pm25, g;
        int aqi;
        if (xSemaphoreTake(xDataMutex, portMAX_DELAY) == pdTRUE) {
            t = global_temp; h = global_hum; g = global_gas;
            pm25 = global_pm25; aqi = global_aqi;
            xSemaphoreGive(xDataMutex);
        }
        String status = "TOT";
        if (aqi <= 50) status = "TOT";
        else if (aqi <= 100) status = "TRUNG BINH";
        else if (aqi <= 150) status = "NHAY CAM";
        else if (aqi <= 200) status = "CO HAI";
        else if (aqi <= 300) status = "RAT CO HAI";
        else status = "NGUY HIEM";

        Serial.println("======================================");
        Serial.printf("Nhiet do : %.1f C\n", t);
        Serial.printf("Do am    : %.1f %%\n", h);
        Serial.printf("PM2.5    : %.1f ug/m3\n", pm25);
        Serial.printf("AQI      : %d\n", aqi);
        Serial.printf("Gas Level: %.1f %%\n", g);
        Serial.printf("Trang thai: %s\n", status.c_str());
        Serial.println("======================================");

        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);
        sprintf(buf, "T:%.1fC H:%.1f%%", t, h);
        oled.print(buf);
        oled.drawLine(0, 10, 128, 10, SSD1306_WHITE);
        oled.setCursor(0, 15);
        oled.print("AQI:");
        oled.setTextSize(2);
        oled.setCursor(45, 15);
        oled.print(aqi);
        oled.setTextSize(1);
        oled.setCursor(0, 40);
        sprintf(buf, "PM2.5: %.1f ug/m3", pm25);
        oled.print(buf);
        oled.setCursor(0, 50);
        sprintf(buf, "Gas: %.1f%%", g);
        oled.print(buf);
        oled.display();

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

// SETUP
void setup() {
    Serial.begin(115200);
    pinMode(DUST_LED_PIN, OUTPUT);
    digitalWrite(DUST_LED_PIN, HIGH);
    analogReadResolution(12);
    analogSetPinAttenuation(DUST_OUT_PIN, ADC_11db);
    analogSetPinAttenuation(MQ135_PIN, ADC_11db);

    pinMode(LED_BLUE, OUTPUT); pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_YELLOW, OUTPUT); pinMode(LED_RED, OUTPUT);
    digitalWrite(LED_BLUE, LOW); digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_YELLOW, LOW); digitalWrite(LED_RED, LOW);

    dht.begin();
    Wire.begin();
    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("Khong tim thay OLED!");
    }
    connectWiFi();
    client.setServer(mqtt_server, 1883);
    client.setBufferSize(512);
    client.setKeepAlive(60);

    // PHẦN XỬ LÝ BASELINE
    EEPROM.begin(EEPROM_SIZE);
    
    #define FORCE_CALIB false  // Đổi thành true để xóa baseline cũ và học lại
    if (FORCE_CALIB) {
        Serial.println("Force calibrate mode: erasing stored baseline.");
        EEPROM.put(BASE_FLAG_ADDR, (byte)0x00);
        EEPROM.commit();
    }
    
    bool hasBaseline = loadBaselineFromEEPROM();
    if (hasBaseline) {
        baselineReady = true;
        Serial.println("Using stored baseline. No learning needed.");
    } else {
        Serial.println("No baseline found. Will learn baseline in 10 minutes...");
        baselineReady = false;
    }

    xDataMutex = xSemaphoreCreateMutex();
    if (xDataMutex != NULL) {
        xTaskCreatePinnedToCore(TaskReadSensors, "Sensors", 4096, NULL, 1, NULL, 0);
        xTaskCreatePinnedToCore(TaskDisplay, "Display", 4096, NULL, 1, NULL, 1);
    }
    Serial.println("System started.");
}

// LOOP
void loop() {
    if (!client.connected()) reconnectMQTT();
    client.loop();
    vTaskDelay(10 / portTICK_PERIOD_MS);
}