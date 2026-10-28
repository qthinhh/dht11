# ESP32 + DHT11 + LCD 20x4 - Arduino IDE

## Thu vien can cai dat

Trong Arduino IDE, vao **Tools > Manage Libraries...** va cai:

1. **DHT sensor library** by Adafruit
2. **Adafruit Unified Sensor** by Adafruit
3. **LiquidCrystal I2C**

## Ket noi

| Thiet bi | Chan | ESP32 DevKit 30 chan |
|---|---|---|
| DHT11 | VCC | 3V3 |
| DHT11 | GND | GND |
| DHT11 | DATA | GPIO4 |
| LCD I2C | SDA | GPIO21 |
| LCD I2C | SCL | GPIO22 |
| LCD I2C | GND | GND |

Neu DHT11 khong co san dien tro, them dien tro keo len 4.7 kOhm--10 kOhm tu DATA len 3V3.

Code mac dinh dia chi LCD la `0x27`. Neu LCD khong hien thi, thu doi thanh `0x3F` trong file `.ino`.

## Xu ly loc gia tri

Chuong trinh loc theo thu tu: kiem tra doc hop le, kiem tra mien gia tri, median 3 mau, xac nhan thay doi lon qua 2 lan doc lien tiep va EMA. He so EMA la `0.30` cho nhiet do va `0.20` cho do am. Neu doc loi, gia tri cu duoc giu toi da 10 giay va hien thi trang thai `STALE`.

ESP32 khong chiu muc logic 5 V. Neu module PCF8574 duoc cap 5 V va co pull-up SDA/SCL len 5 V, can dung mach chuyen muc logic hai chieu hoac dam bao pull-up I2C chi len 3V3.

## Nap chuong trinh

1. Mo file `esp32_dht11_lcd_arduino.ino` bang Arduino IDE.
2. Chon board trong **Tools > Board > esp32 > ESP32 Dev Module**.
3. Chon dung cong tai **Tools > Port**.
4. Bam **Upload**.

Mo **Serial Monitor** o toc do `115200` de xem ket qua doc cam bien.
