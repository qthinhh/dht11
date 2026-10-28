# Quy trình lọc nhiễu dữ liệu DHT11

Tài liệu này mô tả quy trình xử lý dữ liệu nhiệt độ và độ ẩm trong
`esp32_dht11_lcd_arduino.ino`.

## Mục đích

DHT11 có độ phân giải thấp và giá trị đọc có thể dao động hoặc xuất hiện
một mẫu bất thường. Chương trình sử dụng nhiều lớp lọc phần mềm để:

- loại bỏ dữ liệu nằm ngoài phạm vi hợp lệ;
- giảm ảnh hưởng của nhiễu đột biến;
- làm cho giá trị hiển thị ổn định hơn;
- vẫn giữ giá trị cuối cùng trong trường hợp lỗi đọc tạm thời.

## Các tham số lọc

Các tham số được khai báo ở đầu file chương trình:

| Tham số | Giá trị | Ý nghĩa |
|---|---:|---|
| `DHT11_FILTER_WINDOW` | `3` | Số mẫu trong cửa sổ lọc trung vị |
| `DHT11_MIN_TEMPERATURE_C` | `0.0 °C` | Nhiệt độ nhỏ nhất hợp lệ |
| `DHT11_MAX_TEMPERATURE_C` | `50.0 °C` | Nhiệt độ lớn nhất hợp lệ |
| `DHT11_MIN_HUMIDITY_PERCENT` | `0 %` | Độ ẩm nhỏ nhất hợp lệ |
| `DHT11_MAX_HUMIDITY_PERCENT` | `100 %` | Độ ẩm lớn nhất hợp lệ |
| `DHT11_MAX_TEMPERATURE_STEP_C` | `3.0 °C` | Mức thay đổi lớn cần xác nhận |
| `DHT11_MAX_HUMIDITY_STEP_PERCENT` | `10 %` | Mức thay đổi độ ẩm lớn cần xác nhận |
| `DHT11_EMA_ALPHA_TEMPERATURE` | `0.30` | Hệ số EMA cho nhiệt độ |
| `DHT11_EMA_ALPHA_HUMIDITY` | `0.20` | Hệ số EMA cho độ ẩm |
| `DHT11_STALE_TIMEOUT_MS` | `10000 ms` | Thời gian tối đa giữ đầu ra cũ |

## Luồng xử lý

Mỗi vòng lặp, chương trình đọc nhiệt độ và độ ẩm từ DHT11. Khoảng cách
giữa hai lần đọc là 2 giây, phù hợp với yêu cầu của DHT11.

```text
Đọc DHT11
    |
    +-- Đọc lỗi/NaN ------> Giữ giá trị lọc cũ nếu còn mới
    |                         hoặc báo lỗi đọc
    |
    +-- Đọc hợp lệ -------> Kiểm tra phạm vi
                              |
                              +-- Ngoài phạm vi -> Loại mẫu
                              |
                              +-- Trong phạm vi
                                      |
                                      v
                              Cập nhật cửa sổ 3 mẫu
                                      |
                                      v
                              Tính trung vị Median3
                                      |
                                      v
                              Kiểm tra bước nhảy lớn
                                      |
                                      v
                              Làm mượt bằng EMA
                                      |
                                      v
                              Hiển thị giá trị đã lọc
```

## 1. Kiểm tra lỗi đọc

Nếu `dht.readHumidity()` hoặc `dht.readTemperature()` trả về `NaN`, mẫu
được xem là lỗi và không đưa vào bộ lọc.

- Bộ đếm lỗi liên tiếp được tăng lên.
- Nếu giá trị lọc trước đó còn mới trong vòng 10 giây, chương trình tiếp
  tục hiển thị giá trị đó và báo trạng thái `STALE`.
- Nếu không còn giá trị lọc hợp lệ, LCD hiển thị `DHT11: LOI DOC`.

Thư viện DHT cũng thực hiện kiểm tra tính hợp lệ của khung dữ liệu, bao
gồm checksum, trước khi trả về kết quả đọc.

## 2. Loại bỏ giá trị ngoài phạm vi

Mẫu bị loại ngay nếu thỏa một trong các điều kiện:

- nhiệt độ nhỏ hơn `0 °C` hoặc lớn hơn `50 °C`;
- độ ẩm nhỏ hơn `0 %` hoặc lớn hơn `100 %`.

Mẫu bị loại được báo là `Mau vuot gioi han` và không được dùng để cập nhật
giá trị EMA.

## 3. Lọc trung vị 3 mẫu

Chương trình lưu tối đa 3 mẫu gần nhất cho nhiệt độ và độ ẩm. Sau đó các
mẫu được sắp xếp và lấy giá trị ở giữa:

```text
Các mẫu: 24, 24, 40  -> Trung vị: 24
Các mẫu: 65, 25, 25  -> Trung vị: 25
```

Cách này có hiệu quả với một mẫu nhiễu đột biến vì giá trị quá lớn hoặc
quá nhỏ thường không làm thay đổi trung vị của cửa sổ 3 mẫu.

## 4. Phát hiện bước nhảy bất thường

Sau khi tính trung vị, chương trình so sánh với trung vị được chấp nhận
trước đó:

- nhiệt độ thay đổi lớn hơn `3 °C`, hoặc;
- độ ẩm thay đổi lớn hơn `10 %`.

Khi phát hiện bước nhảy lớn lần đầu, mẫu bị đánh dấu là
`DHT11_FILTER_REJECTED_SPIKE` và LCD hiển thị `Trang thai: LOC`.

Chỉ khi bước nhảy lớn tiếp tục xuất hiện ở lần đọc kế tiếp, chương trình
mới chấp nhận thay đổi đó. Cơ chế này giúp phân biệt giữa nhiễu nhất thời
và thay đổi môi trường thực sự.

## 5. Lọc EMA

Giá trị sau trung vị được đưa qua bộ lọc trung bình lũy thừa EMA:

```text
Giá trị lọc mới = alpha × giá trị trung vị
                  + (1 - alpha) × giá trị lọc trước đó
```

Trong chương trình:

- nhiệt độ dùng `alpha = 0.30`;
- độ ẩm dùng `alpha = 0.20`.

Hệ số càng nhỏ thì giá trị càng ổn định nhưng phản ứng càng chậm với thay
đổi thực tế. Vì độ ẩm thường dao động hơn, độ ẩm được dùng hệ số nhỏ hơn.

## 6. Giá trị được hiển thị và ghi Serial

Chương trình không hiển thị trực tiếp giá trị thô. LCD hiển thị hai giá
trị:

- nhiệt độ sau lọc;
- độ ẩm sau lọc.

Khi lọc thành công, LCD hiển thị dòng `Median3 + EMA`. Serial Monitor ghi
cả hai loại dữ liệu để đối chiếu:

```text
Raw:  ... | Loc: ...
```

Trong đó `Raw` là dữ liệu gốc từ DHT11 và `Loc` là dữ liệu sau Median3 và
EMA.

## Trạng thái hiển thị

| Trạng thái | Ý nghĩa |
|---|---|
| `OK` | Mẫu hợp lệ đã được chấp nhận và lọc |
| `LOC` | Mẫu bị xem là bất thường, đang giữ giá trị lọc trước đó |
| `STALE` | Đọc lỗi nhưng vẫn còn giá trị lọc cũ trong thời hạn 10 giây |
| `DHT11: LOI DOC` | Chưa có giá trị lọc hợp lệ hoặc giá trị cũ đã hết hạn |

## Giới hạn của phương pháp

- DHT11 chỉ có độ phân giải khoảng `1 °C` và `1 %`, vì vậy bộ lọc không
  thể tạo ra độ chính xác cao hơn bản thân cảm biến.
- EMA tạo ra độ trễ có chủ ý; thay đổi môi trường sẽ cần vài chu kỳ đọc
  mới thể hiện đầy đủ trên LCD.
- Khoảng cách đọc vẫn phải duy trì ít nhất khoảng 2 giây.
- Đây là lọc bằng phần mềm. Nguồn cấp ổn định, dây DATA ngắn và điện trở
  kéo lên DATA phù hợp vẫn cần thiết để giảm nhiễu điện từ phần cứng.
- Mẫu hợp lệ được đưa vào cửa sổ lịch sử trước khi bước nhảy lớn được
  xác định. Vì vậy, một mẫu spike bị từ chối đầu tiên vẫn có thể ảnh hưởng
  nhẹ đến các lần tính trung vị sau đó; giá trị đầu ra tại thời điểm đó
  vẫn được giữ nguyên.
