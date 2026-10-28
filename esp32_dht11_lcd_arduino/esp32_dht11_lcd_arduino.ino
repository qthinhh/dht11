#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

// ======================== Cau hinh chan ========================
#define DHT_PIN             4
#define DHT_TYPE            DHT11
#define I2C_SDA_PIN         21
#define I2C_SCL_PIN         22
#define LCD_I2C_ADDRESS     0x27    // Doi thanh 0x3F neu LCD cua ban dung dia chi nay

#define LCD_COLUMNS         20
#define LCD_ROWS            4

// ======================== Loc gia tri DHT11 ========================

/*
 * Cac lop xu ly nhieu:
 * 1. Thu vien DHT doc khung va kiem tra du lieu/checksum.
 * 2. Loai mau nam ngoai mien nhiet do/do am cho phep.
 * 3. Median 3 mau de giam anh huong cua mot mau dot bien.
 * 4. Yeu cau hai mau lien tiep xac nhan buoc nhay lon.
 * 5. EMA de lam muot gia tri truoc khi hien thi.
 * 6. Giu gia tri hop le cu trong thoi gian ngan neu doc loi (STALE).
 */
#define DHT11_FILTER_WINDOW             3
#define DHT11_MIN_TEMPERATURE_C         0.0f
#define DHT11_MAX_TEMPERATURE_C         50.0f
#define DHT11_MIN_HUMIDITY_PERCENT      0.0f
#define DHT11_MAX_HUMIDITY_PERCENT      100.0f
#define DHT11_MAX_TEMPERATURE_STEP_C    3.0f
#define DHT11_MAX_HUMIDITY_STEP_PERCENT 10.0f
#define DHT11_EMA_ALPHA_TEMPERATURE    0.30f
#define DHT11_EMA_ALPHA_HUMIDITY       0.20f
#define DHT11_STALE_TIMEOUT_MS          10000UL

DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, LCD_COLUMNS, LCD_ROWS);

enum Dht11FilterResult {
  DHT11_FILTER_ACCEPTED,
  DHT11_FILTER_REJECTED_RANGE,
  DHT11_FILTER_REJECTED_SPIKE
};

struct Dht11Filter {
  float temperatureHistory[DHT11_FILTER_WINDOW] = {0.0f};
  float humidityHistory[DHT11_FILTER_WINDOW] = {0.0f};
  uint8_t historyCount = 0;
  uint8_t historyNext = 0;

  bool initialized = false;
  bool pendingLargeChange = false;
  float lastAcceptedTemperature = 0.0f;
  float lastAcceptedHumidity = 0.0f;

  float filteredTemperature = 0.0f;
  float filteredHumidity = 0.0f;
  uint32_t lastOutputMs = 0;
};

Dht11Filter dhtFilter;
uint32_t consecutiveReadErrors = 0;

// Hien thi mot dong va xoa cac ky tu thua cua lan hien thi truoc.
void printLine(uint8_t row, const char *text)
{
  lcd.setCursor(0, row);

  size_t length = text != nullptr ? strlen(text) : 0;
  for (uint8_t column = 0; column < LCD_COLUMNS; column++) {
    if (column < length) {
      lcd.print(text[column]);
    } else {
      lcd.print(' ');
    }
  }
}

void showMeasurement(float temperature,
                    float humidity,
                    const char *status,
                    const char *detail)
{
  char line[21];

  snprintf(line, sizeof(line), "Nhiet do: %5.1f C", temperature);
  printLine(0, line);

  snprintf(line, sizeof(line), "Do am   : %5.1f %%", humidity);
  printLine(1, line);

  printLine(2, status);
  printLine(3, detail);
}

void showReadError()
{
  printLine(0, "DHT11: LOI DOC");
  printLine(1, "Kiem tra DATA/VCC");
  printLine(2, "Cho lan doc tiep");
  printLine(3, "");
}

float medianOfSamples(const float *values, uint8_t count)
{
  float sorted[DHT11_FILTER_WINDOW] = {0.0f};

  for (uint8_t i = 0; i < count; ++i) {
    sorted[i] = values[i];
  }

  for (uint8_t i = 0; i < count; ++i) {
    for (uint8_t j = i + 1; j < count; ++j) {
      if (sorted[j] < sorted[i]) {
        float temporary = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = temporary;
      }
    }
  }

  return sorted[count / 2];
}

Dht11FilterResult updateFilter(float temperature, float humidity)
{
  // Lop 2: Loai mau nam ngoai mien gia tri hop ly.
  if (temperature < DHT11_MIN_TEMPERATURE_C ||
      temperature > DHT11_MAX_TEMPERATURE_C ||
      humidity < DHT11_MIN_HUMIDITY_PERCENT ||
      humidity > DHT11_MAX_HUMIDITY_PERCENT) {
    return DHT11_FILTER_REJECTED_RANGE;
  }

  // Lop 3: Cap nhat cua so va tinh Median 3 mau.
  dhtFilter.temperatureHistory[dhtFilter.historyNext] = temperature;
  dhtFilter.humidityHistory[dhtFilter.historyNext] = humidity;
  dhtFilter.historyNext =
      (dhtFilter.historyNext + 1) % DHT11_FILTER_WINDOW;

  if (dhtFilter.historyCount < DHT11_FILTER_WINDOW) {
    ++dhtFilter.historyCount;
  }

  float medianTemperature = medianOfSamples(
      dhtFilter.temperatureHistory, dhtFilter.historyCount);
  float medianHumidity = medianOfSamples(
      dhtFilter.humidityHistory, dhtFilter.historyCount);

  if (!dhtFilter.initialized) {
    dhtFilter.filteredTemperature = medianTemperature;
    dhtFilter.filteredHumidity = medianHumidity;
    dhtFilter.lastAcceptedTemperature = medianTemperature;
    dhtFilter.lastAcceptedHumidity = medianHumidity;
    dhtFilter.lastOutputMs = millis();
    dhtFilter.initialized = true;
    return DHT11_FILTER_ACCEPTED;
  }

  // Lop 4: Xac nhan buoc nhay lon qua hai lan doc lien tiep.
  bool largeChange =
      fabsf(medianTemperature - dhtFilter.lastAcceptedTemperature) >
          DHT11_MAX_TEMPERATURE_STEP_C ||
      fabsf(medianHumidity - dhtFilter.lastAcceptedHumidity) >
          DHT11_MAX_HUMIDITY_STEP_PERCENT;

  if (largeChange) {
    // Can hai mau lien tiep cung xac nhan truoc khi chap nhan buoc nhay lon.
    if (!dhtFilter.pendingLargeChange) {
      dhtFilter.pendingLargeChange = true;
      return DHT11_FILTER_REJECTED_SPIKE;
    }
    dhtFilter.pendingLargeChange = false;
  } else {
    dhtFilter.pendingLargeChange = false;
  }

  // Lop 5: Lam muot gia tri median bang bo loc EMA.
  dhtFilter.filteredTemperature =
      DHT11_EMA_ALPHA_TEMPERATURE * medianTemperature +
      (1.0f - DHT11_EMA_ALPHA_TEMPERATURE) *
          dhtFilter.filteredTemperature;
  dhtFilter.filteredHumidity =
      DHT11_EMA_ALPHA_HUMIDITY * medianHumidity +
      (1.0f - DHT11_EMA_ALPHA_HUMIDITY) *
          dhtFilter.filteredHumidity;
  dhtFilter.lastAcceptedTemperature = medianTemperature;
  dhtFilter.lastAcceptedHumidity = medianHumidity;
  dhtFilter.lastOutputMs = millis();

  return DHT11_FILTER_ACCEPTED;
}

bool hasFreshFilteredOutput()
{
  return dhtFilter.initialized &&
         (uint32_t)(millis() - dhtFilter.lastOutputMs) <=
             DHT11_STALE_TIMEOUT_MS;
}

void setup()
{
  Serial.begin(115200);

  // ESP32 cho phep chi dinh GPIO SDA va SCL khi khoi tao I2C.
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  dht.begin();

  printLine(0, "ESP32 DHT11 LCD");
  printLine(1, "Dang khoi dong...");
  printLine(2, "");
  printLine(3, "");

  delay(2000);
}

void loop()
{
  // DHT11 can khoang cach doc toi thieu 2 giay.
  // Lop 1: Thu vien DHT doc khung va kiem tra checksum truoc khi loc.
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) {
    if (consecutiveReadErrors < UINT32_MAX) {
      ++consecutiveReadErrors;
    }

    Serial.println("Khong doc duoc DHT11");

    // Lop 6: Neu doc loi, tam giu gia tri loc hop le gan nhat.
    if (hasFreshFilteredOutput()) {
      char detail[21];
      snprintf(detail, sizeof(detail), "Loi doc: %2lu lan",
               (unsigned long)consecutiveReadErrors);

      showMeasurement(dhtFilter.filteredTemperature,
                      dhtFilter.filteredHumidity,
                      "Trang thai: STALE",
                      detail);
    } else {
      showReadError();
    }
  } else {
    consecutiveReadErrors = 0;

    Dht11FilterResult filterResult = updateFilter(temperature, humidity);

    if (filterResult == DHT11_FILTER_ACCEPTED) {
      Serial.print("Raw: ");
      Serial.print(temperature, 1);
      Serial.print(" C, ");
      Serial.print(humidity, 1);
      Serial.print(" % | Loc: ");
      Serial.print(dhtFilter.filteredTemperature, 1);
      Serial.print(" C, ");
      Serial.print(dhtFilter.filteredHumidity, 1);
      Serial.println(" %");

      showMeasurement(dhtFilter.filteredTemperature,
                      dhtFilter.filteredHumidity,
                      "Trang thai: OK",
                      "Median3 + EMA");
    } else if (hasFreshFilteredOutput()) {
      const char *detail = filterResult == DHT11_FILTER_REJECTED_SPIKE
                               ? "Cho mau lap lai"
                               : "Mau vuot gioi han";

      Serial.print("Bo qua mau bat thuong: ");
      Serial.print(temperature, 1);
      Serial.print(" C, ");
      Serial.print(humidity, 1);
      Serial.println(" %");

      showMeasurement(dhtFilter.filteredTemperature,
                      dhtFilter.filteredHumidity,
                      "Trang thai: LOC",
                      detail);
    } else {
      Serial.println("Chua co mau DHT11 hop le sau loc");
      showReadError();
    }
  }

  delay(2000);
}
