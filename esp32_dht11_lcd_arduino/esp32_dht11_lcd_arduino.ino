#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <string.h>

// ======================== Cau hinh chan ========================
#define DHT_PIN             4
#define DHT_TYPE            DHT11
#define I2C_SDA_PIN         21
#define I2C_SCL_PIN         22
#define LCD_I2C_ADDRESS     0x27    // Doi thanh 0x3F neu LCD cua ban dung dia chi nay

#define LCD_COLUMNS         20
#define LCD_ROWS            4

DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, LCD_COLUMNS, LCD_ROWS);

// Hien thi mot dong va xoa cac ky tu thua cua lan hien thi truoc.
void printLine(uint8_t row, const char *text)
{
  lcd.setCursor(0, row);

  size_t length = strlen(text);
  for (uint8_t column = 0; column < LCD_COLUMNS; column++) {
    if (column < length) {
      lcd.print(text[column]);
    } else {
      lcd.print(' ');
    }
  }
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
  // DHT11 khong nen doc nhanh hon khoang 1 giay.
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Khong doc duoc DHT11");

    printLine(0, "DHT11: LOI DOC");
    printLine(1, "Kiem tra DATA/VCC");
    printLine(2, "Thu lai sau 2 giay");
    printLine(3, "");
  } else {
    char line[21];

    Serial.print("Nhiet do: ");
    Serial.print(temperature, 1);
    Serial.print(" C | Do am: ");
    Serial.print(humidity, 1);
    Serial.println(" %");

    snprintf(line, sizeof(line), "Nhiet do: %5.1f C", temperature);
    printLine(0, line);

    snprintf(line, sizeof(line), "Do am   : %5.1f %%", humidity);
    printLine(1, line);

    printLine(2, "Trang thai: OK");
    printLine(3, "Cap nhat moi 2 giay");
  }

  delay(2000);
}
