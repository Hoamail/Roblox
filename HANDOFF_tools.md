# HANDOFF — Tab Tools

> Trạng thái: Phase 1–3 đã có implementation trong source; Macro Recorder hiện
> là MVP keyboard-only và vẫn cần smoke test trong Roblox. Memory Scanner đã
> build độc lập và được kiểm tra compile.
>
> Phạm vi hiện tại: thay placeholder của tab Tools bằng Instance Explorer
> read-only, SaveInstance-lite, Memory Scanner độc lập và Macro Recorder.

## 1. Mục tiêu và nguyên tắc

## 1A. Trạng thái triển khai

- Phase 1: Instance Explorer read-only đã tích hợp vào overlay.
- Phase 2: SaveInstance-lite có worker, giới hạn depth/node, cancel và
  rename-on-success.
- Phase 3: `ext_scan.exe` là scanner `u64` read-only độc lập; không link
  `ntwrite.cpp`.
- Phase 4 MVP: Macro Recorder keyboard-only có giới hạn 30 giây/2000 event,
  yêu cầu Roblox là foreground window, cancel khi mất focus và replay bounded.
  Mouse capture và lưu/load macro JSON vẫn là phần mở rộng chưa triển khai.
- Lifecycle audit: snapshot state có mutex; dump bị cancel/join trước khi
  session identity bị invalidate hoặc process handle đóng.
- Gate đã chạy: `build_fast.bat`, `build.bat`, `-aimtest`, `-srvtest 0`,
  positive offsets và negative offsets (negative phải fail). Chưa xác nhận UI,
  dump output và macro replay trong game thật.

Tab Tools hiện chỉ là placeholder trong `src\ovl\tab_tools.cpp`. Việc triển khai
phải giữ nguyên hành vi của Body, Combat, Visual, Teleport, World, Server và
Misc.

Nguyên tắc bắt buộc:

- UI chỉ dựng giao diện và gửi request; không walk cây hoặc scan memory trong
  main loop.
- Worker không được gọi ImGui.
- Địa chỉ instance chỉ có giá trị trong place/server hiện tại. `placeId` thay đổi
chưa đủ để nhận diện session; phải đổi generation khi `jobId` hoặc DataModel
pointer đổi.
- Đổi `placeId`, `jobId` hoặc DataModel phải invalidate cache.
- Mọi giới hạn depth, node, bytes, event và thời gian phải được kiểm tra trước
  khi chạy.
- Read-only phải hoàn thành và pass gate trước khi thêm write/freeze.
- Không dump cookie, token, clipboard, script content hoặc dữ liệu tài khoản.
- Tool mới phải được thêm tường minh vào build script; không dùng glob `src\*.cpp`.

## 2. Phạm vi MVP

MVP chỉ gồm:

1. Instance Explorer read-only.
2. Save Workspace JSON.

Memory Scanner write/freeze và Macro Recorder triển khai sau, không gộp vào
đợt MVP vì có rủi ro state/thread và test lớn hơn.

## 3. Kiến trúc đề xuất

### 3.1. File

```text
src\ovl\ovl_tools.h
src\ovl\ovl_tools.cpp
src\ovl\tools_types.h
src\ovl\tab_tools.cpp
src\scan.cpp                  (giai đoạn scanner độc lập)
HANDOFF_tools.md
```

### 3.2. API và include boundary

```cpp
void toolsInit();
void toolsShutdown();
void toolsTick(Memory& m, SDK& sdk, const Offsets& o,
               uint64_t place, const std::string& jobId,
               uint64_t sessionGeneration, DWORD now);
```

`toolsTick` chỉ làm công việc bounded, non-blocking. Các thao tác walk/dump
dài phải chạy worker và publish snapshot qua mutex hoặc message queue.

`ovl_tools.h` không include `ovl_tabs.h` và không expose hàm nhận `MenuCtx`;
tránh include vòng giữa module logic và UI. Các hàm `toolsDraw*` là helper
private trong `tab_tools.cpp`, hoặc nhận một snapshot type thuần dữ liệu.
`tab_tools.cpp` include cả `ovl_tabs.h` và `ovl_tools.h`.

`jobId` và `sessionGeneration` là input bắt buộc, không được ngầm lấy từ
global `srv` trong worker. Worker phải gắn cả hai giá trị vào request và chỉ
publish khi chúng vẫn khớp.

### 3.3. State

State Tools nên nằm trong `ovl_tools.cpp`, không copy vào `overlay.cpp`. Nếu
`MenuCtx` cần thêm state, chỉ thêm field ở cuối struct vì `overlay.cpp` đang
dùng aggregate initialization theo vị trí.

State tối thiểu:

```cpp
struct ToolNode {
    uint64_t address = 0;
    std::string name;
    std::string className;
    int childCount = 0;
    bool expanded = false;
    bool childrenLoaded = false;
};

struct ToolProperty {
    std::string name;
    std::string type;
    std::string value;
    bool readable = false;
};

struct ToolState {
    uint64_t generation = 0;
    uint64_t place = 0;
    std::string jobId;
    uint64_t selectedInstance = 0;
    std::vector<ToolNode> nodes;
    std::vector<ToolProperty> properties;
};
```

Mọi snapshot phải gắn `generation`. Worker chỉ được publish kết quả nếu
generation vẫn khớp; kết quả của game cũ phải bị loại bỏ.

## 4. Phase 0 — Hạ tầng và lifecycle

### Việc cần làm

- Tạo `ToolsCtx` sở hữu mutex, worker handle, stop flag và generation.
- Xác định điểm gọi `toolsInit`, `toolsTick`, `toolsShutdown`.
- Khi process/game đổi, cancel worker, clear cache, tăng generation.
- Khi overlay thoát, gọi `toolsShutdown` trước khi đóng `Memory`/SDK hoặc giải
  phóng bất kỳ object mà worker có thể đọc.
- Thêm trạng thái `idle`, `running`, `cancelled`, `failed`, `done`.
- Mọi request phải chụp `pid`, `placeId`, `jobId`, `sessionGeneration`.

### Gate

- Bật/tắt tab nhiều lần không tạo worker trùng.
- Thoát overlay khi worker đang chạy không crash/hang.
- Đổi server khi đang expand/dump không hiển thị node cũ.
- Main loop không chờ worker.
- Shutdown khi request đang ở giữa một batch read phải join sạch, không truy cập
  `Memory` sau khi handle đã đóng.

## 5. Phase 1 — Instance Explorer read-only

### 5.1. Root và lazy loading

Root ưu tiên:

```text
DataModel
├── Workspace
├── Players
├── Lighting
├── ReplicatedFirst
├── ReplicatedStorage
├── ServerScriptService
└── StarterGui
```

Chỉ sử dụng object/offset đã xác minh. Không tự suy đoán offset thiếu.

Khi expand:

1. Kiểm tra address và generation.
2. Đọc một batch bounded, tối đa 100–250 node.
3. Cache snapshot.
4. Hiển thị loading/error rõ ràng.

Không recursive walk toàn bộ hierarchy trong một frame.

### 5.2. UI

```text
[Refresh root] [Collapse all]
Filter: [................]

DataModel
  > Workspace
  > Players
  > Lighting

Selected:
  Name / Class / Address / Child count
```

Dùng `BeginChild("##tools_tree", ...)` để scroll. Property viewer chỉ
read-only trong phase này.

### 5.3. Property được phép hiển thị

- Name
- ClassName
- Parent
- Address
- Child count
- Position khi có offset hợp lệ
- Một số cờ đã được xác minh như Transparency/Enabled/Visible

Property không xác minh phải hiển thị `unavailable`, không đọc offset đoán.

## 6. Phase 2 — SaveInstance-lite

### 6.1. Phạm vi

Bắt đầu từ Workspace, có giới hạn:

- depth
- max nodes
- max output bytes
- timeout

Mở rộng sang Lighting/ReplicatedStorage chỉ sau khi Workspace pass gate.

Không dump script content, bytecode, cookie, token hoặc dữ liệu tài khoản.

### 6.2. Output

```json
{
  "placeId": 123,
  "jobId": "...",
  "timestamp": "...",
  "root": {
    "name": "Workspace",
    "class": "Workspace",
    "address": "0x...",
    "children": []
  }
}
```

File nằm trong `dumps\` với tên `<placeId>_<timestamp>.json`.

Ghi vào `.tmp`, flush, rồi rename thành `.json`. Cancel/error không được để
file cuối có JSON giả hoàn chỉnh.

### 6.3. UI

```text
Root: Workspace
Depth: [5]
Max nodes: [5000]
[Preview] [Save JSON] [Cancel]
Progress: 320 / 5000
Status: walking Workspace...
```

## 7. Phase 3 — Memory Scanner độc lập

Scanner không chạy trong overlay. Tạo `ext_scan.exe` từ `src\scan.cpp`.

### 7.1. Read/filter trước

Hỗ trợ ban đầu:

```text
int32, int64, float, double, pointer, ASCII/UTF-8 string
```

Workflow:

```text
initial scan -> exact filter -> changed/unchanged filter -> result list
```

Mỗi kết quả phải có address, type, old value, current value và region.

Chỉ scan committed/readable region; bỏ `NOACCESS`/guard và có giới hạn tổng
bytes/cancel.

### 7.2. Write/freeze sau read-only

Chỉ thêm sau khi scanner read/filter pass:

- confirmation rõ ràng trước write
- lưu old value để restore
- dừng freeze khi process/game đổi
- interval bounded
- handle/process validity check trước mỗi operation
- tool nào gọi `openWrite()` hoặc `.write` phải link `src\ntwrite.cpp`

## 8. Phase 4 — Macro Recorder

### Phạm vi

Record timestamp tương đối của:

- keyboard down/up
- mouse button
- mouse move khi cần

Không record clipboard, password/token hoặc input khi đang nhập keybind.

State machine:

```text
Idle -> Recording -> Recorded -> Playing -> Stopping
```

Giới hạn bắt buộc:

- max event count
- max duration
- validate `dtMs`
- stop khẩn cấp
- dừng khi Roblox process/game đổi
- chỉ replay khi cửa sổ đích hợp lệ

Lưu tại `macros\<name>.json`, validate schema khi load.

## 9. Phase 5 — UI tích hợp

Thay placeholder bằng 4 section:

```text
Instance Explorer
[Open] [Refresh]

SaveInstance-lite
[Save Workspace]

Memory Scanner
[Open external scanner]

Macro Recorder
[Open]
```

Chỉ chuyển sang sub-tab ImGui khi nội dung vượt chiều cao panel. Nút phải có
feedback `queued/running/done/failed`, không success-shaped fallback.

## 10. Build và test

### Build

Cập nhật cả:

- `build_fast.bat`
- `build.bat`

Thêm `src\ovl\ovl_tools.cpp` nếu logic nằm trong overlay. Scanner là executable
riêng. Không glob source vì nhiều file trong `src\` có `main()` riêng.

### Gate chung

Mỗi phase phải chạy:

```text
build_fast.bat
ext_overlay.exe -aimtest
ext_overlay.exe -srvtest 0
ext_overlay.exe -offtest offsets_fixtures
ext_overlay.exe -offtest offsets_fixtures\negative
build.bat
```

Expected:

- build exit 0
- aimtest không có `FAIL`
- srvtest kết thúc `=== done (PASS) ===`
- positive offsets `fails=0`
- negative offsets fail với exit 1
- full build không lỗi/link fail

### Test Tools riêng

- root refresh/expand/collapse
- filter name/class
- stale generation sau đổi server
- cancel worker
- shutdown khi worker đang bận
- dump JSON parse được
- giới hạn depth/node/bytes
- scanner fixture read/filter/cancel
- macro record/replay/stop
- lỗi process đóng giữa operation

## 11. Tự đánh giá — lỗ hổng và potential bugs

### 11.1. Rủi ro nghiêm trọng phải xử lý trước code

| Mức | Rủi ro | Nguyên nhân | Biện pháp bắt buộc |
|---|---|---|---|
| Cao | Use-after-free/stale pointer | Cache address qua place/server | generation + clear cache + validate DataModel |
| Cao | Deadlock/hang khi thoát | Worker giữ mutex hoặc block I/O | stop flag, timeout, join có kiểm soát, không I/O vô hạn |
| Cao | Crash do offset sai | Đọc property bằng offset chưa verify | chỉ dùng offset validated, lỗi trả về explicit |
| Cao | File dump hỏng | Cancel/error giữa lúc ghi | `.tmp` rồi atomic rename |
| Cao | Scan vượt RAM/CPU | Scan toàn bộ region không giới hạn | byte budget, batch, cancel, progress |
| Cao | Ghi nhầm process | PID đổi hoặc handle cũ | kiểm tra PID/process identity trước read/write |

### 11.2. Lỗi logic có khả năng xảy ra

| Mức | Potential bug | Cách phát hiện |
|---|---|---|
| Cao | Worker publish kết quả của game cũ | test đổi jobId trong lúc expand/dump |
| Cao | Expand cùng node tạo duplicate children | test click expand liên tục |
| Trung bình | `childCount` và list cache lệch nhau | refresh node rồi so snapshot |
| Trung bình | Filter làm mất selection nhưng vẫn giữ address cũ | test filter/clear filter |
| Trung bình | UI đọc vector trong lúc worker swap | mutex/snapshot test dưới load |
| Trung bình | `DWORD` tick wrap làm timeout sai | dùng signed tick-difference helper |
| Trung bình | JSON string escape sai | tên có quote, slash, Unicode |
| Trung bình | Macro replay chạy khi cửa sổ sai | đổi foreground window trước Play |
| Thấp | File name path traversal | chỉ cho phép tên macro hợp lệ |
| Thấp | Progress đứng ở 99% | cập nhật terminal state trong mọi nhánh lỗi/cancel |

### 11.3. Rủi ro kiến trúc

1. **Nhồi state vào `overlay.cpp`:** làm mất ranh giới module và dễ phá
   aggregate `MenuCtx`. State Tools phải ở `ovl_tools.cpp`.
2. **Dùng ImGui từ worker:** có thể crash ngẫu nhiên. Worker chỉ publish data.
3. **Scan/dump đồng bộ trong main loop:** làm tụt FPS, mất input và tạo
   impression overlay bị treo.
4. **Dùng lại pointer sau teleport/server hop:** có thể đọc vùng nhớ hợp lệ
   nhưng thuộc object khác; generation phải đổi ngay khi job/DataModel đổi.
5. **Bật write/freeze quá sớm:** làm khó phân biệt lỗi scanner với lỗi game;
   read-only là gate bắt buộc trước.
6. **Thêm source bằng glob:** sẽ link trùng `main()` với các tool standalone.
7. **Quên `ntwrite.cpp`:** tool write-capable sẽ link fail hoặc sai đường ghi.
8. **Không giới hạn output:** một dump lớn có thể làm đầy đĩa hoặc treo worker.

## 12. Rollback

- Tách mỗi phase thành commit riêng.
- Nếu Tools làm hỏng build hoặc main loop, tạm trả `drawTabTools` về placeholder.
- Không rollback bằng `git checkout` lên các file đang có thay đổi không liên
  quan.
- Giữ lại log/test output và hash executable của phase cuối pass.

## 13. Definition of Done

- Không còn placeholder.
- Explorer expand được root với cache lazy.
- Save Workspace tạo JSON hợp lệ và cancel sạch.
- Worker shutdown sạch.
- Không stale result sau đổi place/job.
- Scanner read/filter riêng pass fixture trước khi có write/freeze.
- Macro record/replay có giới hạn và emergency stop.
- Tab cũ không đổi hành vi.
- `build_fast.bat` và `build.bat` pass.
- Có log gate và cập nhật tài liệu này sau từng phase.
