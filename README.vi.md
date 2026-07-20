# CrossVi

[English](README.md) | **Tiếng Việt**

CrossVi là phần mềm hệ thống (firmware) mã nguồn mở dành cho máy đọc sách màn hình e-paper Xteink X3 và X4. Dự án được phát triển độc lập từ mã nguồn của [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), tập trung vào độ ổn định, tốc độ và các tính năng mới.

CrossVi có giao diện tiếng Việt và được khởi xướng bởi một lập trình viên Việt Nam, nhưng **không chỉ dành cho người Việt**. Firmware hỗ trợ 30 ngôn ngữ và chào đón cả người dùng lẫn người đóng góp trên toàn thế giới.

> **Tình trạng hiện tại:** mã nguồn đã vượt qua kiểm thử tự động và biên dịch firmware thành công, nhưng vẫn chưa được kiểm tra trên máy X3/X4 thật. Hiện chưa có bản phát hành ổn định được khuyến nghị cho người dùng phổ thông.

## CrossVi làm được gì?

- Đọc sách EPUB, TXT, XTC/XTCH và ảnh BMP.
- Ghi nhớ sách đang đọc, vị trí đọc, dấu trang và lịch sử sách gần đây.
- Tra từ điển StarDict và đồng bộ tiến độ đọc với máy chủ tương thích KOReader.
- Đổi phông chữ, cỡ chữ, khoảng cách dòng, căn lề và giao diện.
- Cài thêm phông chữ từ thẻ nhớ.
- Chuyển sách qua Wi-Fi bằng trình duyệt hoặc Calibre.
- Tải sách từ thư viện trực tuyến OPDS.
- Đổi chức năng các nút bấm, màn hình ngủ và thanh trạng thái.
- Hỗ trợ 30 ngôn ngữ, gồm tiếng Việt và các ngôn ngữ viết từ phải sang trái.

## Máy nào được hỗ trợ?

- Xteink X3 dùng ESP32-C3.
- Xteink X4 dùng ESP32-C3.

CrossVi không phải firmware chính thức, không trực thuộc Xteink hoặc bất kỳ nhà sản xuất thiết bị nào.

## Trước khi cài firmware

Một số máy mua từ cửa hàng bên thứ ba có thể bị khóa chức năng nạp firmware qua USB. Theo hướng dẫn của dự án gốc, máy mua trực tiếp từ xteink.com không cần công cụ mở khóa.

> **Cảnh báo quan trọng:** công cụ Xteink Unlocker hiện chỉ hỗ trợ chính thức CrossPoint và CrossInk. Không dùng CrossVi làm firmware mở khóa cho một thiết bị đang bị khóa USB. Làm sai có thể khiến máy không thể khôi phục.

Nếu chưa biết máy có bị khóa hay không:

1. Bật máy và cắm cáp USB-C có truyền dữ liệu.
2. Mở công cụ nạp firmware trên trình duyệt.
3. Kiểm tra xem trình duyệt có nhận cổng USB của máy không.
4. Thử cáp, cổng USB hoặc trình duyệt khác trước khi kết luận máy bị khóa.

## Cách cài CrossVi

Hiện CrossVi chưa có bản phát hành đã được kiểm tra trên phần cứng thật. Người dùng không chuyên nên chờ một bản ổn định xuất hiện tại [trang phát hành CrossVi](https://github.com/tvhdc/crossvi/releases).

Khi đã có bản phù hợp:

1. Tải file `firmware.bin` từ trang Releases.
2. Bật máy, kết nối với máy tính bằng USB-C.
3. Mở [công cụ cài firmware trên web](https://crosspointreader.com/#flash-tools).
4. Chọn đúng X3 hoặc X4.
5. Chọn **Custom .bin** và mở file `firmware.bin`.
6. Không rút cáp hoặc tắt máy trong lúc nạp firmware.

Muốn quay lại firmware CrossPoint chính thức, bạn có thể dùng cùng công cụ và chọn một bản CrossPoint phù hợp.

## Chép sách vào máy

Cách đơn giản nhất là tháo thẻ nhớ, chép sách vào thư mục bạn muốn rồi lắp lại vào máy.

Bạn cũng có thể:

- Mở **File Transfer** trên máy để chuyển sách bằng trình duyệt.
- Dùng tiện ích CrossPoint Reader của Calibre để gửi sách qua Wi-Fi.
- Thêm thư viện OPDS để tải sách trực tiếp.

Các định dạng được hỗ trợ trực tiếp là `.epub`, `.txt`, `.xtc`, `.xtch` và `.bmp`.

## Dữ liệu cũ có dùng lại được không?

Có. CrossVi giữ nguyên thư mục `.crosspoint`, cấu trúc dữ liệu, định dạng file và các giao thức tương thích của CrossPoint. Bạn vẫn nên sao lưu thẻ nhớ trước khi đổi firmware.

## Cập nhật firmware

CrossVi có chức năng kiểm tra cập nhật qua Wi-Fi. Chức năng này chỉ hoạt động sau khi kho GitHub có một bản phát hành CrossVi chứa file `firmware.bin`.

Không đổi tên `firmware.bin` nếu cập nhật từ thẻ nhớ hoặc trang phát hành GitHub.

## Khi gặp lỗi

- Thử khởi động lại máy trước.
- Kiểm tra thẻ nhớ và dung lượng còn trống.
- Nếu máy tạo file nhật ký lỗi (crash log) ở thư mục gốc của thẻ nhớ, hãy giữ lại file đó.
- Báo lỗi tại [GitHub Issues](https://github.com/tvhdc/crossvi/issues) và mô tả thao tác đã gây ra lỗi.
- Trao đổi ý tưởng hoặc đặt câu hỏi tại [GitHub Discussions](https://github.com/tvhdc/crossvi/discussions).

Tài liệu sử dụng chi tiết hiện có tại [CrossVi User Guide](USER_GUIDE.md).

## Dành cho người muốn phát triển

Hướng dẫn build, cấu trúc mã nguồn và kiểm thử được giữ trong [README tiếng Anh](README.md) và [tài liệu đóng góp](docs/contributing/README.md).

## Nguồn gốc và giấy phép

CrossVi là một fork độc lập của CrossPoint Reader. Dự án gốc và cộng đồng CrossPoint là nền tảng của firmware này. CrossVi giữ nguyên giấy phép MIT, thông tin bản quyền và các ghi nhận bắt buộc của tác giả gốc.
