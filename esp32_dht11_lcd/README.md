# ESP32 + DHT11 + LCD 20x4 (ESP-IDF)

Vi du nay doc DHT11 va hien thi nhiet do/do am tren LCD 20x4 thong qua module I2C PCF8574.

Code su dung I2C master driver moi (`driver/i2c_master.h`), phu hop voi ESP-IDF 6.x.

## Ket noi

| Thiet bi | Chan | ESP32 DevKit 30 chan |
|---|---|---|
| DHT11 | VCC | 3V3 |
| DHT11 | GND | GND |
| DHT11 | DATA | GPIO4 |
| LCD I2C | SDA | GPIO21 |
| LCD I2C | SCL | GPIO22 |
| LCD I2C | GND | GND |

DATA cua DHT11 can dien tro keo len 4.7 kOhm--10 kOhm lên 3V3 neu module chua co san.

Mac dinh code dung dia chi LCD `0x27`; neu module dung `0x3F`, sua `LCD_I2C_ADDRESS` trong `main/main.c`.

Luu y: ESP32 khong chiu muc logic 5 V. Neu module PCF8574 cap 5 V va co dien tro pull-up I2C len 5 V, can dung mach chuyen muc logic hai chieu, hoac cau hinh bus I2C de pull-up len 3V3 an toan.

## Bien dich va nap

```bash
cd /home/hieunt/esp32_dht11_lcd
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Neu dung cong serial khac, thay `/dev/ttyUSB0` bang cong tuong ung.
