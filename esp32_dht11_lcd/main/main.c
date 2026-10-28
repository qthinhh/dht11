#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ======================== Phan cung ======================== */

#define DHT11_GPIO              GPIO_NUM_4

#define I2C_PORT                I2C_PORT_NUM_0
#define I2C_SDA_GPIO            GPIO_NUM_21
#define I2C_SCL_GPIO            GPIO_NUM_22
#define I2C_FREQ_HZ             100000
#define LCD_I2C_ADDRESS         0x27    // Thu 0x3F neu module cua ban dung dia chi nay

/* ======================== Loc gia tri DHT11 ======================== */

#define DHT11_FILTER_WINDOW             3
#define DHT11_MIN_TEMPERATURE_C         0
#define DHT11_MAX_TEMPERATURE_C         50
#define DHT11_MIN_HUMIDITY_PERCENT      0
#define DHT11_MAX_HUMIDITY_PERCENT      100
#define DHT11_MAX_TEMPERATURE_STEP_C    3
#define DHT11_MAX_HUMIDITY_STEP_PERCENT 10
#define DHT11_EMA_ALPHA_TEMPERATURE    0.30f
#define DHT11_EMA_ALPHA_HUMIDITY       0.20f
#define DHT11_STALE_TIMEOUT_US          10000000LL

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
    i2c_master_dev_handle_t device;
    bool backlight;
} lcd_i2c_t;

typedef enum {
    DHT11_FILTER_ACCEPTED,
    DHT11_FILTER_REJECTED_RANGE,
    DHT11_FILTER_REJECTED_SPIKE,
} dht11_filter_result_t;

typedef struct {
    int temperature_history[DHT11_FILTER_WINDOW];
    int humidity_history[DHT11_FILTER_WINDOW];
    uint8_t history_count;
    uint8_t history_next;

    bool initialized;
    bool pending_large_change;
    int last_accepted_temperature;
    int last_accepted_humidity;

    float filtered_temperature;
    float filtered_humidity;
    int64_t last_output_us;
} dht11_filter_t;

/* ======================== I2C ======================== */

static esp_err_t i2c_master_init(i2c_master_bus_handle_t *bus_handle,
                                 i2c_master_dev_handle_t *device_handle)
{
    if (bus_handle == NULL || device_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t result = i2c_new_master_bus(&bus_config, bus_handle);
    if (result != ESP_OK) {
        return result;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LCD_I2C_ADDRESS,
        .scl_speed_hz = I2C_FREQ_HZ,
    };

    result = i2c_master_bus_add_device(
        *bus_handle, &device_config, device_handle);
    if (result != ESP_OK) {
        i2c_del_master_bus(*bus_handle);
        *bus_handle = NULL;
    }

    return result;
}

/* ======================== LCD 20x4 ======================== */

static esp_err_t lcd_expander_write(const lcd_i2c_t *lcd, uint8_t value)
{
    uint8_t output = value | (lcd->backlight ? LCD_BACKLIGHT : 0);

    // The device handle already contains the 7-bit LCD address.
    return i2c_master_transmit(lcd->device, &output, 1, 100);
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

static void lcd_print_line(const lcd_i2c_t *lcd, uint8_t row, const char *text)
{
    size_t length = text != NULL ? strlen(text) : 0;

    ESP_ERROR_CHECK(lcd_set_cursor(lcd, row, 0));

    for (uint8_t column = 0; column < 20; ++column) {
        uint8_t character = (column < length)
                                ? (uint8_t)text[column]
                                : (uint8_t)' ';
        ESP_ERROR_CHECK(lcd_data(lcd, character));
    }
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

static int fixed_point_tenths(float value)
{
    return value >= 0.0f
             ? (int)(value * 10.0f + 0.5f)
             : (int)(value * 10.0f - 0.5f);
}

static void lcd_show_measurement(const lcd_i2c_t *lcd,
                                 float temperature,
                                 float humidity,
                                 const char *status,
                                 const char *detail)
{
    char line[21];
    int temperature_tenths = fixed_point_tenths(temperature);
    int humidity_tenths = fixed_point_tenths(humidity);

    snprintf(line, sizeof(line), "Nhiet do: %3d.%d C",
             temperature_tenths / 10, temperature_tenths % 10);
    lcd_print_line(lcd, 0, line);

    snprintf(line, sizeof(line), "Do am   : %3d.%d %%",
             humidity_tenths / 10, humidity_tenths % 10);
    lcd_print_line(lcd, 1, line);

    lcd_print_line(lcd, 2, status);
    lcd_print_line(lcd, 3, detail);
}

static void lcd_show_read_error(const lcd_i2c_t *lcd)
{
    lcd_print_line(lcd, 0, "DHT11: LOI DOC");
    lcd_print_line(lcd, 1, "Kiem tra DATA/VCC");
    lcd_print_line(lcd, 2, "Cho lan doc tiep");
    lcd_print_line(lcd, 3, "");
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

static int absolute_difference(int first, int second)
{
    int difference = first - second;
    return difference < 0 ? -difference : difference;
}

static int median_int(const int *values, uint8_t count)
{
    int sorted[DHT11_FILTER_WINDOW] = {0};

    for (uint8_t i = 0; i < count; ++i) {
        sorted[i] = values[i];
    }

    for (uint8_t i = 0; i < count; ++i) {
        for (uint8_t j = i + 1; j < count; ++j) {
            if (sorted[j] < sorted[i]) {
                int temporary = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = temporary;
            }
        }
    }

    return sorted[count / 2];
}

static dht11_filter_result_t dht11_filter_update(
    dht11_filter_t *filter, int temperature, int humidity)
{
    if (temperature < DHT11_MIN_TEMPERATURE_C ||
        temperature > DHT11_MAX_TEMPERATURE_C ||
        humidity < DHT11_MIN_HUMIDITY_PERCENT ||
        humidity > DHT11_MAX_HUMIDITY_PERCENT) {
        return DHT11_FILTER_REJECTED_RANGE;
    }

    filter->temperature_history[filter->history_next] = temperature;
    filter->humidity_history[filter->history_next] = humidity;
    filter->history_next =
        (filter->history_next + 1) % DHT11_FILTER_WINDOW;

    if (filter->history_count < DHT11_FILTER_WINDOW) {
        ++filter->history_count;
    }

    int median_temperature = median_int(
        filter->temperature_history, filter->history_count);
    int median_humidity = median_int(
        filter->humidity_history, filter->history_count);

    if (!filter->initialized) {
        filter->filtered_temperature = (float)median_temperature;
        filter->filtered_humidity = (float)median_humidity;
        filter->last_accepted_temperature = median_temperature;
        filter->last_accepted_humidity = median_humidity;
        filter->last_output_us = esp_timer_get_time();
        filter->initialized = true;
        return DHT11_FILTER_ACCEPTED;
    }

    bool large_change =
        absolute_difference(median_temperature,
                            filter->last_accepted_temperature) >
            DHT11_MAX_TEMPERATURE_STEP_C ||
        absolute_difference(median_humidity,
                            filter->last_accepted_humidity) >
            DHT11_MAX_HUMIDITY_STEP_PERCENT;

    if (large_change) {
        // Require two consecutive samples before accepting a sudden jump.
        if (!filter->pending_large_change) {
            filter->pending_large_change = true;
            return DHT11_FILTER_REJECTED_SPIKE;
        }
        filter->pending_large_change = false;
    } else {
        filter->pending_large_change = false;
    }

    filter->filtered_temperature =
        DHT11_EMA_ALPHA_TEMPERATURE * (float)median_temperature +
        (1.0f - DHT11_EMA_ALPHA_TEMPERATURE) *
            filter->filtered_temperature;
    filter->filtered_humidity =
        DHT11_EMA_ALPHA_HUMIDITY * (float)median_humidity +
        (1.0f - DHT11_EMA_ALPHA_HUMIDITY) *
            filter->filtered_humidity;
    filter->last_accepted_temperature = median_temperature;
    filter->last_accepted_humidity = median_humidity;
    filter->last_output_us = esp_timer_get_time();

    return DHT11_FILTER_ACCEPTED;
}

static bool dht11_filter_has_fresh_output(const dht11_filter_t *filter)
{
    return filter->initialized &&
           (esp_timer_get_time() - filter->last_output_us) <=
               DHT11_STALE_TIMEOUT_US;
}

/* ======================== Ung dung ======================== */

void app_main(void)
{
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t device_handle;

    ESP_ERROR_CHECK(i2c_master_init(&bus_handle, &device_handle));

    const lcd_i2c_t lcd = {
        .device = device_handle,
        .backlight = true,
    };

    ESP_ERROR_CHECK(dht11_gpio_init());
    ESP_ERROR_CHECK(lcd_init(&lcd));

    lcd_print_line(&lcd, 0, "ESP32 DHT11 LCD");
    lcd_print_line(&lcd, 1, "Dang khoi dong...");
    lcd_print_line(&lcd, 2, "");
    lcd_print_line(&lcd, 3, "");

    dht11_filter_t filter = {0};
    uint32_t consecutive_read_errors = 0;

    while (true) {
        int temperature_c;
        int humidity_percent;
        esp_err_t result = dht11_read(&temperature_c, &humidity_percent);

        if (result == ESP_OK) {
            consecutive_read_errors = 0;

            dht11_filter_result_t filter_result = dht11_filter_update(
                &filter, temperature_c, humidity_percent);

            if (filter_result == DHT11_FILTER_ACCEPTED) {
                int temperature_tenths = fixed_point_tenths(
                    filter.filtered_temperature);
                int humidity_tenths = fixed_point_tenths(
                    filter.filtered_humidity);

                ESP_LOGI(TAG,
                         "Raw: %d C, %d%% | Loc: %d.%d C, %d.%d%%",
                         temperature_c, humidity_percent,
                         temperature_tenths / 10, temperature_tenths % 10,
                         humidity_tenths / 10, humidity_tenths % 10);

                lcd_show_measurement(&lcd,
                                     filter.filtered_temperature,
                                     filter.filtered_humidity,
                                     "Trang thai: OK",
                                     "Median3 + EMA");
            } else if (dht11_filter_has_fresh_output(&filter)) {
                const char *detail = filter_result ==
                                             DHT11_FILTER_REJECTED_SPIKE
                                         ? "Cho mau lap lai"
                                         : "Mau vuot gioi han";

                ESP_LOGW(TAG, "Bo qua mau DHT11 do gia tri bat thuong: %d C, %d%%",
                         temperature_c, humidity_percent);

                lcd_show_measurement(&lcd,
                                     filter.filtered_temperature,
                                     filter.filtered_humidity,
                                     "Trang thai: LOC",
                                     detail);
            } else {
                ESP_LOGW(TAG, "Chua co mau DHT11 hop le sau loc: %d C, %d%%",
                         temperature_c, humidity_percent);
                lcd_show_read_error(&lcd);
            }
        } else {
            if (consecutive_read_errors < UINT32_MAX) {
                ++consecutive_read_errors;
            }

            ESP_LOGW(TAG, "Khong doc duoc DHT11: %s", esp_err_to_name(result));

            if (dht11_filter_has_fresh_output(&filter)) {
                char detail[21];
                snprintf(detail, sizeof(detail), "Loi doc: %2lu lan",
                         (unsigned long)consecutive_read_errors);

                lcd_show_measurement(&lcd,
                                     filter.filtered_temperature,
                                     filter.filtered_humidity,
                                     "Trang thai: STALE",
                                     detail);
            } else {
                lcd_show_read_error(&lcd);
            }
        }

        // DHT11 can khoang cach doc toi thieu 2 giay.
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
