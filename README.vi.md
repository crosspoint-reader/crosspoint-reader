# CrossVi

[English](README.md) | **Tiếng Việt**

CrossVi là phần mềm hệ thống (firmware) mã nguồn mở dành cho máy đọc sách màn hình e-paper Xteink X3 và X4. Dự án được phát triển độc lập từ mã nguồn của [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), tập trung vào độ ổn định, tốc độ và các tính năng mới.

CrossVi có giao diện tiếng Việt và được khởi xướng bởi một lập trình viên Việt Nam, nhưng **không chỉ dành cho người Việt**. Firmware hỗ trợ 30 ngôn ngữ và chào đón cả người dùng lẫn người đóng góp trên toàn thế giới.

> **Tình trạng hiện tại:** mã nguồn đã vượt qua kiểm thử tự động, biên dịch firmware thành công và giao diện có thể chạy trong trình mô phỏng riêng cho X3/X4. Dự án vẫn chưa được kiểm tra đầy đủ trên máy thật, nên hiện chưa có bản phát hành ổn định được khuyến nghị cho người dùng phổ thông.

## CrossVi làm được gì?

- Đọc sách EPUB, TXT, XTC/XTCH và ảnh BMP. CrossVi hỗ trợ chính thức tập con XTC/XTCH v1.0 không nén, trang dựng sẵn 480×800: X4 hiển thị đúng 1:1, còn X3 thu vừa toàn trang, giữ tỷ lệ và căn giữa nên không cắt nội dung.
- Ghi nhớ sách đang đọc, vị trí đọc, dấu trang và lịch sử sách gần đây.
- Tra từ điển StarDict, gồm tệp đồng nghĩa `.syn` tùy chọn và lịch sử tối đa 15 lần tra thành công; đồng bộ tiến độ đọc với máy chủ tương thích KOReader.
- Đổi phông chữ, cỡ chữ, khoảng cách dòng, căn lề và giao diện.
- Cài thêm phông chữ từ thẻ nhớ.
- Chuyển sách qua Wi-Fi bằng trình duyệt hoặc Calibre.
- Tải sách từ thư viện trực tuyến OPDS.
- Đổi chức năng các nút bấm, màn hình ngủ và thanh trạng thái.
- Hỗ trợ 30 ngôn ngữ, gồm tiếng Việt và các ngôn ngữ viết từ phải sang trái. Các tính năng đọc mới đã có đầy đủ tiếng Anh và tiếng Việt; một số nhãn mới ở ngôn ngữ khác tạm dùng bản tiếng Anh.

## Các tính năng đọc mới

CrossVi bổ sung một số tiện ích dễ dùng, nhưng không tự ý thay đổi sách hoặc vị trí đọc của bạn:

Thống kê trong trình đọc dùng được cho EPUB, TXT/Markdown và XTC/XTCH. EPUB và TXT/Markdown đều có menu công cụ, dấu trang, tra cứu từ điển, đoạn trích và cài đặt hiển thị riêng cho từng sách. Với TXT/Markdown, dấu trang và đoạn trích bám theo vị trí byte trong tệp, nên đổi phông, lề hoặc hướng màn hình không làm chúng âm thầm trỏ sang đoạn khác. Các chức năng phụ thuộc cấu trúc EPUB như chọn chương, chú thích chân trang và đồng bộ vị trí KOReader không xuất hiện trong menu văn bản thuần. XTC/XTCH là ảnh trang cố định nên không đổi phông/cỡ chữ, không tra từ theo vùng chọn, không clipping và không đồng bộ vị trí KOReader; xem [contract định dạng](lib/Xtc/README).

- **Dashboard:** màn hình chính gọn hơn, hiển thị sách vừa đọc và các con số tổng quan. Khi dữ liệu của EPUB gần nhất đọc được an toàn, bạn có thể mở thẳng phần thống kê đầy đủ từ đây. Bật tại **Cài đặt → Hiển thị → Giao diện → Bảng đọc sách**.
- **Theme CrossVi:** màn hình chính hai cột riêng cho X3/X4, nổi bật sách đang đọc, tiến độ, nút **Đọc tiếp**, sáu lối tắt chính và thẻ tóm tắt **Hôm nay/Mục tiêu** có thể mở phần thống kê đầy đủ. Bạn có thể đặt tên máy tại **Cài đặt → Hiển thị → Tên hiển thị**, bật theme tại **Giao diện → CrossVi** và đặt chỉ tiêu tại **Cài đặt → Trình đọc → Mục tiêu đọc mỗi ngày**.
- **Thiết lập riêng cho từng sách:** mỗi EPUB có thể dùng phông chữ, cỡ chữ, lề, khoảng cách dòng, buộc thụt đầu dòng, hướng màn hình và tốc độ tự lật trang riêng. Ba chế độ hiển thị EPUB là **Đầy đủ**, **Cân bằng** (mặc định) và **Nhẹ**. Nếu một EPUB thật sự hết bộ nhớ khi dựng trang, CrossVi hỏi trước khi thử lại bằng **Chế độ an toàn**; chế độ này chỉ được lưu cho cuốn sách đó sau khi một trang đã hiển thị thành công.
- **Thống kê đọc:** xem thời gian đọc, số trang đã lật, số phiên đọc, chuỗi ngày đọc và tiến độ cho EPUB, TXT/Markdown và XTC/XTCH. Thời gian chỉ được tính khi trang đã hiển thị thành công; thời gian ở menu hoặc lúc máy đang lập chỉ mục không được tính. Với TXT/Markdown, nhấn **Xác nhận** để mở menu rồi chọn **Thống kê đọc**; XTC/XTCH không có chương vẫn mở thống kê trực tiếp, còn XTC/XTCH có chương dùng thao tác giữ **Xác nhận**. Tại trang **Sách này**, nhấn **Xác nhận** để chỉnh ngày giờ bắt đầu/hoàn thành. Tại trang **Thiết bị này**, nhấn **Xác nhận** để mở lịch đọc 730 ngày; nếu vào từ Home, cùng menu này còn có sao lưu và khôi phục tổng thống kê của máy. Ngày nằm ngoài phần lịch sử còn được lưu sẽ được ghi rõ là chưa có dữ liệu, không giả thành ngày không đọc. Vì số trang TXT/Markdown thay đổi theo phông chữ và lề, CrossVi không đưa ra tốc độ hoặc ngày hoàn thành dự kiến thiếu tin cậy.
- **Clipping (lưu đoạn trích):** chọn một đoạn trên trang đang đọc hoặc tiếp tục sang các trang liền kề trong cùng chương, rồi xem lại hoặc xóa sau đó. Bản nháp có giới hạn rõ ràng để không làm đầy bộ nhớ và chỉ được lưu sau khi bạn xác nhận. CrossVi không tìm kiếm và đoán vị trí cũ sau khi bố cục sách thay đổi; nếu không khớp an toàn, đoạn chữ vẫn được giữ nhưng phần đánh dấu sẽ tạm không hiện.
- **Nearby Sync:** hai máy CrossVi ở gần nhau có thể trao đổi vị trí đọc của đúng cùng một tệp EPUB, hoặc lưu một bản thống kê của máy kia. Cả hai bên đều phải mở tính năng và xác nhận; dữ liệu nhận được không tự ghi đè dữ liệu trên máy. Nếu chương đích chưa có dữ liệu bố cục phù hợp hoặc không xác định được đúng đoạn văn, CrossVi sẽ từ chối thay vì đoán một trang gần đúng.

Nearby Sync không cần Internet nhưng dữ liệu truyền gần **không được mã hóa**. Chỉ dùng với một máy đáng tin cậy và kiểm tra mã ghép cặp trên cả hai màn hình.

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

Các định dạng được hỗ trợ trực tiếp là `.epub`, `.txt`, `.md`, `.xtc`, `.xtch` và `.bmp`. Với XTC/XTCH, hãy xuất trang dọc 480×800, version 1.0 và không nén. Có thể dùng [EPUB to XTC Converter](https://github.com/bigbag/epub-to-xtc-converter); kể cả đọc trên X3, hãy chọn đầu ra X4 480×800 vì đây là kích thước mà firmware kiểm tra và thu vừa an toàn.

## Dữ liệu cũ có dùng lại được không?

Có. CrossVi giữ nguyên thư mục `.crosspoint`, các mã định danh kỹ thuật và giao thức tương thích của CrossPoint. Những định dạng an toàn mới được ghi cạnh dữ liệu cũ thay vì xóa hoặc ghi đè dữ liệu cũ. Bạn vẫn nên sao lưu thẻ nhớ trước khi đổi firmware.

Không nên xóa cả thư mục `.crosspoint` chỉ để sửa lỗi hiển thị sách. Thư mục này không chỉ là bộ nhớ đệm: nó còn chứa cài đặt, vị trí đọc, dấu trang, đoạn trích và thống kê. Hãy dùng chức năng xóa bộ nhớ đệm trong máy trước; nếu phải thao tác thẻ nhớ thủ công, hãy sao lưu rồi chỉ di chuyển thư mục `sections` của đúng cuốn sách cần tạo lại bố cục.

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

## Xem và bấm thử giao diện khi chưa có máy

Bạn có thể chạy chính giao diện CrossVi trên máy tính, không phải một bản vẽ
lại bằng web. Trình mô phỏng dùng đúng mã hiển thị, phông chữ, biểu tượng, giao
diện và cách xử lý nút của firmware.

Mở Terminal trong thư mục CrossVi rồi chạy một trong hai lệnh:

```sh
python3 scripts/run_simulator.py x3
python3 scripts/run_simulator.py x4
```

Trong cửa sổ hiện ra, bạn có thể bấm trực tiếp các nút ở cột bên phải. Bàn phím
cũng dùng được: `Esc` là Quay lại, `Enter` là Chọn, các phím mũi tên là nút điều
hướng/chuyển trang và `P` là nút nguồn. Nhấn `F12` để chụp riêng phần giao diện
đúng từng pixel.

Dữ liệu thử của hai máy nằm riêng trong `.simulator-data/x3` và
`.simulator-data/x4`. Bạn có thể chép sách vào đó rồi mở bằng **Browse Files**.

X3 và X4 được build riêng với đúng kích thước màn hình của từng máy; X3 không
phải ảnh X4 bị co lại. Tuy vậy, máy tính không thể tái tạo màu mực, bóng mờ,
thời gian làm tươi e-paper, nguồn điện, lỗi thẻ nhớ vật lý hoặc sóng Nearby
Sync. Xem [hướng dẫn trình mô phỏng](docs/contributing/simulator.md) nếu bạn muốn
đổi thư mục sách, vị trí ảnh chụp hoặc chạy kiểm thử giao diện.

## Dành cho người muốn phát triển

Hướng dẫn build, cấu trúc mã nguồn và kiểm thử được giữ trong [README tiếng Anh](README.md) và [tài liệu đóng góp](docs/contributing/README.md).

## Nguồn gốc và giấy phép

CrossVi là một fork độc lập của CrossPoint Reader. Dự án gốc và cộng đồng CrossPoint là nền tảng của firmware này. CrossVi giữ nguyên giấy phép MIT, thông tin bản quyền và các ghi nhận bắt buộc của tác giả gốc.

Các ý tưởng Dashboard, lưu đoạn trích, Nearby Sync, thống kê đọc và cài đặt riêng theo sách được tham khảo từ [CrossInk](https://github.com/uxjulia/CrossInk), sau đó được rà soát và triển khai theo các giới hạn an toàn riêng của CrossVi.
