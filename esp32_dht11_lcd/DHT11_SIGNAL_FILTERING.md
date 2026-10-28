# Quy trình xử lý tín hiệu DHT11 trong chương trình ESP-IDF

Tài liệu này mô tả quá trình đọc, kiểm tra và lọc giá trị nhiệt độ/độ ẩm trong `main/main.c`.

## 1. Sơ đồ xử lý

```text
DHT11 gửi khung 40 bit
        |
        v
Đo thời gian xung và kiểm tra timeout
        |
        v
Kiểm tra checksum
        |
        v
Kiểm tra miền giá trị
        |
        v
Lưu vào bộ đệm 3 mẫu
        |
        v
Median 3 mẫu
        |
        v
Xác nhận thay đổi lớn qua 2 lần đọc
        |
        v
EMA
        |
        v
Hiển thị LCD và ghi log
```

## 2. Đọc và kiểm tra khung DHT11

Hàm `dht11_read()` thực hiện các bước sau:

1. Kéo DATA xuống thấp trong 20 ms để gửi tín hiệu bắt đầu.
2. Nhả bus DATA và chờ tín hiệu phản hồi của DHT11.
3. Đo độ dài mức HIGH của 40 bit dữ liệu.
4. Phân biệt bit `0` và `1` bằng ngưỡng 45 micro-giây.
5. Kiểm tra checksum của 5 byte nhận được.

Nếu timeout hoặc checksum không đúng, mẫu bị loại hoàn toàn và không được đưa vào bộ lọc.

DHT11 được đọc cách nhau 2 giây tại cuối vòng lặp:

```c
vTaskDelay(pdMS_TO_TICKS(2000));
```

## 3. Kiểm tra miền giá trị

Các giá trị hợp lệ hiện tại:

| Đại lượng | Miền cho phép |
|---|---:|
| Nhiệt độ | `0–50 °C` |
| Độ ẩm | `0–100 %RH` |

Mẫu nằm ngoài miền trên được đánh dấu `DHT11_FILTER_REJECTED_RANGE` và không cập nhật giá trị lọc.

Các giới hạn được định nghĩa tại đầu file:

```c
#define DHT11_MIN_TEMPERATURE_C         0
#define DHT11_MAX_TEMPERATURE_C         50
#define DHT11_MIN_HUMIDITY_PERCENT      0
#define DHT11_MAX_HUMIDITY_PERCENT      100
```

Đây là kiểm tra an toàn cơ bản. Có thể thay đổi miền giá trị theo môi trường ứng dụng thực tế.

## 4. Bộ lọc median 3 mẫu

Ba mẫu hợp lệ gần nhất được lưu trong:

```c
temperature_history[3]
humidity_history[3]
```

Sau mỗi lần đọc, chương trình sắp xếp các mẫu và chọn giá trị ở giữa. Ví dụ:

```text
Nhiệt độ: 25, 25, 45  -> median = 25
Độ ẩm:   50, 51, 50  -> median = 50
```

Nhờ đó, một mẫu đột biến đơn lẻ thường không làm thay đổi giá trị đầu ra.

Trong giai đoạn khởi động, khi chưa đủ 3 mẫu, chương trình sử dụng số mẫu hiện có để tính median.

## 5. Loại bỏ thay đổi đột biến

Sau median, chương trình so sánh mẫu hiện tại với median đã được chấp nhận gần nhất.

Ngưỡng hiện tại:

```c
#define DHT11_MAX_TEMPERATURE_STEP_C    3
#define DHT11_MAX_HUMIDITY_STEP_PERCENT 10
```

Nếu nhiệt độ thay đổi quá 3 °C hoặc độ ẩm thay đổi quá 10 %RH trong một chu kỳ đọc, mẫu đầu tiên chưa được chấp nhận. Chương trình chờ mẫu kế tiếp xác nhận:

```text
Mẫu 1 thay đổi lớn -> DHT11_FILTER_REJECTED_SPIKE
Mẫu 2 vẫn thay đổi lớn -> chấp nhận thay đổi
```

Điều này giúp phân biệt một nhiễu đột biến với một thay đổi môi trường thực sự kéo dài.

Một thay đổi lớn vẫn tạo ra độ trễ tối thiểu khoảng một chu kỳ đọc, tức khoảng 2 giây.

## 6. Bộ lọc trung bình mũ EMA

Sau khi qua median và kiểm tra đột biến, giá trị được làm trơn bằng EMA:

```text
y[n] = alpha * x[n] + (1 - alpha) * y[n-1]
```

Tham số hiện tại:

```c
#define DHT11_EMA_ALPHA_TEMPERATURE    0.30f
#define DHT11_EMA_ALPHA_HUMIDITY       0.20f
```

Ý nghĩa:

- `alpha` lớn: đáp ứng nhanh hơn nhưng ít làm trơn hơn.
- `alpha` nhỏ: tín hiệu ổn định hơn nhưng phản hồi chậm hơn.
- Độ ẩm dùng `alpha = 0.20` vì thường dao động mạnh hơn nhiệt độ.

Giá trị EMA được lưu trong:

```c
filtered_temperature
filtered_humidity
```

## 7. Xử lý khi đọc lỗi

Nếu DHT11 timeout hoặc checksum sai:

- Không cập nhật bộ đệm median.
- Không cập nhật EMA.
- Tăng bộ đếm lỗi liên tiếp.
- Nếu vẫn còn giá trị hợp lệ mới nhất trong vòng 10 giây, LCD hiển thị giá trị cũ với trạng thái `STALE`.
- Sau hơn 10 giây không có giá trị hợp lệ, LCD hiển thị lỗi đọc cảm biến.

Thời gian giữ giá trị cũ:

```c
#define DHT11_STALE_TIMEOUT_US 10000000LL
```

Giá trị `STALE` chỉ được dùng để hiển thị tạm thời; không nên dùng nó như dữ liệu mới trong các chức năng điều khiển quan trọng.

## 8. Trạng thái hiển thị

| Trạng thái LCD | Ý nghĩa |
|---|---|
| `Trang thai: OK` | Mẫu hợp lệ đã qua bộ lọc |
| `Trang thai: LOC` | Mẫu bị loại do vượt miền hoặc thay đổi đột biến |
| `Trang thai: STALE` | Đang hiển thị giá trị hợp lệ gần nhất vì lần đọc mới bị lỗi |
| `DHT11: LOI DOC` | Chưa có giá trị hợp lệ hoặc giá trị cuối đã quá cũ |

Serial Monitor ghi cả giá trị thô và giá trị sau lọc, ví dụ:

```text
Raw: 25 C, 60% | Loc: 25.0 C, 59.2%
```

## 9. Những loại nhiễu mà bộ lọc xử lý được

- Một mẫu nhiệt độ hoặc độ ẩm nhảy bất thường.
- Một gói dữ liệu bị lỗi checksum hoặc timeout.
- Giá trị nằm ngoài miền hợp lý.
- Dao động nhỏ giữa các lần đọc.

## 10. Những vấn đề bộ lọc không thể khắc phục

- Sai địa chỉ LCD hoặc sai mapping PCF8574.
- Sai trình tự khởi tạo HD44780.
- Pull-up SDA/SCL không phù hợp.
- Cấp mức logic 5 V trực tiếp vào GPIO ESP32.
- Cảm biến đặt gần nguồn nhiệt hoặc bị ngưng tụ nước.
- Độ chính xác vốn có của DHT11.

Bộ lọc chỉ xử lý dữ liệu sau khi DHT11 đã gửi được khung hợp lệ. Nó không thay thế việc đấu dây đúng, nguồn ổn định và điện trở kéo lên phù hợp.

## 11. Các tham số nên điều chỉnh

Nếu tín hiệu vẫn dao động:

- Giảm `DHT11_EMA_ALPHA_TEMPERATURE` hoặc `DHT11_EMA_ALPHA_HUMIDITY`.
- Không nên giảm quá thấp vì sẽ làm tăng độ trễ.
- Có thể tăng kích thước median, nhưng với DHT11 không nên tăng quá nhiều vì cảm biến chỉ đọc được khoảng một lần mỗi 2 giây.

Nếu hệ thống phản ứng quá chậm:

- Tăng alpha EMA lên khoảng `0.4–0.5`.
- Tăng ngưỡng thay đổi lớn nếu môi trường thực sự biến đổi nhanh.

Mọi thay đổi tham số nên được đánh giá cùng dữ liệu thô trong Serial Monitor để tránh che mất thay đổi môi trường thực tế.
