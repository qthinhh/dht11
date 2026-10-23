#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ======================== Phan cung ======================== */

#define DHT11_GPIO              GPIO_NUM_4

#define I2C_PORT                I2C_NUM_0
#define I2C_SDA_GPIO            GPIO_NUM_21
#define I2C_SCL_GPIO            GPIO_NUM_22
#define I2C_FREQ_HZ             100000
#define LCD_I2C_ADDRESS         0x27    // Thu 0x3F neu module cua ban dung dia chi nay

static const char *TAG = "DHT11_LCD";

/*
 * Mapping pho bien cua module PCF8574-LCD:
 * P0=RS, P1=RW, P2=EN, P3=Backlight, P4..P7=D4..D7.
 */
#define LCD_RS                  0x01
#define LCD_RW                  0x02
#define LCD_EN                  0x04
#define LCD_BACKLIGHT           0x08

typedef struct {
    i2c_port_t port;
    uint8_t address;
    bool backlight;
} lcd_i2c_t;

/* ======================== I2C ======================== */

static esp_err_t i2c_master_init(void)
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &config));
    return i2c_driver_install(I2C_PORT, config.mode, 0, 0, 0);
}

/* ======================== LCD 20x4 ======================== */

static esp_err_t lcd_expander_write(const lcd_i2c_t *lcd, uint8_t value)
{
    uint8_t output = value | (lcd->backlight ? LCD_BACKLIGHT : 0);

    i2c_cmd_handle_t command = i2c_cmd_link_create();
    if (command == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(command);
    i2c_master_write_byte(command, (lcd->address << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(command, output, true);
    i2c_master_stop(command);

    esp_err_t result = i2c_master_cmd_begin(
        lcd->port, command, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(command);
    return result;
}

static esp_err_t lcd_write_4_bits(const lcd_i2c_t *lcd, uint8_t value)
{
    esp_err_t result = lcd_expander_write(lcd, value);
    if (result != ESP_OK) {
        return result;
    }

    result = lcd_expander_write(lcd, value | LCD_EN);
    esp_rom_delay_us(1);
    if (result != ESP_OK) {
        return result;
    }

    result = lcd_expander_write(lcd, value & (uint8_t)~LCD_EN);
    esp_rom_delay_us(50);
    return result;
}

static esp_err_t lcd_send(const lcd_i2c_t *lcd, uint8_t value, uint8_t mode)
{
    esp_err_t result = lcd_write_4_bits(lcd, (value & 0xF0) | mode);
    if (result != ESP_OK) {
        return result;
    }

    return lcd_write_4_bits(lcd, ((value << 4) & 0xF0) | mode);
}

static esp_err_t lcd_command(const lcd_i2c_t *lcd, uint8_t command)
{
    return lcd_send(lcd, command, 0);
}

static esp_err_t lcd_data(const lcd_i2c_t *lcd, uint8_t data)
{
    return lcd_send(lcd, data, LCD_RS);
}

static esp_err_t lcd_set_cursor(const lcd_i2c_t *lcd, uint8_t row, uint8_t column)
{
    // Dia chi DDRAM cua LCD 20x4: 0x00, 0x40, 0x14, 0x54.
    static const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};

    if (row >= 4 || column >= 20) {
        return ESP_ERR_INVALID_ARG;
    }

    return lcd_command(lcd, 0x80 | (row_offsets[row] + column));
}

static esp_err_t lcd_print_line(const lcd_i2c_t *lcd, uint8_t row, const char *text)
{
    esp_err_t result = lcd_set_cursor(lcd, row, 0);
    if (result != ESP_OK) {
        return result;
    }

    // Ghi du 20 ky tu de xoa ky tu thua cua lan hien thi truoc.
    for (uint8_t column = 0; column < 20; ++column) {
        uint8_t character = (text != NULL && text[column] != '\0')
                                ? (uint8_t)text[column]
                                : (uint8_t)' ';
        result = lcd_data(lcd, character);
        if (result != ESP_OK) {
            return result;
        }
    }

    return ESP_OK;
}

static esp_err_t lcd_init(const lcd_i2c_t *lcd)
{
    // Cho LCD co thoi gian khoi dong sau khi cap nguon.
    vTaskDelay(pdMS_TO_TICKS(50));

    // Trinh tu khoi tao HD44780 4-bit.
    ESP_ERROR_CHECK(lcd_write_4_bits(lcd, 0x30));
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_ERROR_CHECK(lcd_write_4_bits(lcd, 0x30));
    esp_rom_delay_us(150);
    ESP_ERROR_CHECK(lcd_write_4_bits(lcd, 0x30));
    esp_rom_delay_us(150);
    ESP_ERROR_CHECK(lcd_write_4_bits(lcd, 0x20));
    esp_rom_delay_us(150);

    ESP_ERROR_CHECK(lcd_command(lcd, 0x28)); // 4-bit, 2 dong, font 5x8
    ESP_ERROR_CHECK(lcd_command(lcd, 0x08)); // Tat hien thi
    ESP_ERROR_CHECK(lcd_command(lcd, 0x01)); // Xoa man hinh
    vTaskDelay(pdMS_TO_TICKS(2));
    ESP_ERROR_CHECK(lcd_command(lcd, 0x06)); // Tu dong tang con tro
    ESP_ERROR_CHECK(lcd_command(lcd, 0x0C)); // Bat hien thi, tat con tro

    return ESP_OK;
}

/* ======================== DHT11 ======================== */

static esp_err_t dht11_gpio_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << DHT11_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&config);
}

/* Cho den khi chan dat muc mong muon, tra ve thoi gian cho theo micro-giay. */
static bool dht11_wait_level(int level, uint32_t timeout_us, uint32_t *duration_us)
{
    int64_t start = esp_timer_get_time();

    while (gpio_get_level(DHT11_GPIO) != level) {
        if ((uint32_t)(esp_timer_get_time() - start) > timeout_us) {
            return false;
        }
    }

    if (duration_us != NULL) {
        *duration_us = (uint32_t)(esp_timer_get_time() - start);
    }
    return true;
}

static esp_err_t dht11_read(int *temperature_c, int *humidity_percent)
{
    uint8_t data[5] = {0};
    uint32_t high_time_us;

    if (temperature_c == NULL || humidity_percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Xung bat dau: keo DATA xuong thap it nhat 18 ms, sau do nha len.
    gpio_set_level(DHT11_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(DHT11_GPIO, 1); // Open-drain: 1 = nha bus
    esp_rom_delay_us(30);

    // DHT11 phan hoi: LOW ~80 us, HIGH ~80 us, sau do LOW ~50 us.
    if (!dht11_wait_level(0, 100, NULL) ||
        !dht11_wait_level(1, 100, NULL) ||
        !dht11_wait_level(0, 100, NULL)) {
        return ESP_ERR_TIMEOUT;
    }

    for (int bit_index = 0; bit_index < 40; ++bit_index) {
        // Moi bit bat dau bang LOW ~50 us.
        if (!dht11_wait_level(1, 100, NULL)) {
            return ESP_ERR_TIMEOUT;
        }

        // Do dai muc HIGH: ~26 us la bit 0, ~70 us la bit 1.
        if (!dht11_wait_level(0, 120, &high_time_us)) {
            return ESP_ERR_TIMEOUT;
        }

        data[bit_index / 8] <<= 1;
        if (high_time_us > 45) {
            data[bit_index / 8] |= 1;
        }
    }

    if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    *humidity_percent = data[0];
    *temperature_c = data[2];
    return ESP_OK;
}

/* ======================== Ung dung ======================== */

void app_main(void)
{
    const lcd_i2c_t lcd = {
        .port = I2C_PORT,
        .address = LCD_I2C_ADDRESS,
        .backlight = true,
    };

    ESP_ERROR_CHECK(i2c_master_init());
    ESP_ERROR_CHECK(dht11_gpio_init());
    ESP_ERROR_CHECK(lcd_init(&lcd));

    ESP_ERROR_CHECK(lcd_print_line(&lcd, 0, "ESP32 DHT11 LCD"));
    ESP_ERROR_CHECK(lcd_print_line(&lcd, 1, "Dang khoi dong..."));
    ESP_ERROR_CHECK(lcd_print_line(&lcd, 2, ""));
    ESP_ERROR_CHECK(lcd_print_line(&lcd, 3, ""));

    while (true) {
        int temperature_c;
        int humidity_percent;
        esp_err_t result = dht11_read(&temperature_c, &humidity_percent);

        if (result == ESP_OK) {
            char line[21];

            ESP_LOGI(TAG, "Nhiet do: %d C, do am: %d%%",
                     temperature_c, humidity_percent);

            snprintf(line, sizeof(line), "Nhiet do: %2d C", temperature_c);
            ESP_ERROR_CHECK(lcd_print_line(&lcd, 0, line));

            snprintf(line, sizeof(line), "Do am   : %2d %%", humidity_percent);
            ESP_ERROR_CHECK(lcd_print_line(&lcd, 1, line));

            ESP_ERROR_CHECK(lcd_print_line(&lcd, 2, "Trang thai: OK"));
            ESP_ERROR_CHECK(lcd_print_line(&lcd, 3, "Cap nhat moi 2 giay"));
        } else {
            ESP_LOGW(TAG, "Khong doc duoc DHT11: %s", esp_err_to_name(result));
            ESP_ERROR_CHECK(lcd_print_line(&lcd, 0, "DHT11: LOI DOC"));
            ESP_ERROR_CHECK(lcd_print_line(&lcd, 1, "Kiem tra DATA/VCC"));
            ESP_ERROR_CHECK(lcd_print_line(&lcd, 2, "Thu lai sau 2 giay"));
            ESP_ERROR_CHECK(lcd_print_line(&lcd, 3, ""));
        }

        // DHT11 khong nen doc nhanh hon khoang 1 giay.
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
