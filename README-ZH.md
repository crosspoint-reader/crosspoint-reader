# CrossPoint Reader CJK

[English](./README.md) | **[中文](./README-ZH.md)** | [日本語](./README-JA.md)

如果你使用 AI 编码 Agent 在本仓库协作开发, 请先阅读 [AGENTS.md](./AGENTS.md).

> 基于 [daveallie/crosspoint-reader](https://github.com/daveallie/crosspoint-reader) 的 **Xteink X4** 墨水屏阅读器固件 CJK 适配版. 

本项目在原版 CrossPoint Reader 的基础上进行了 CJK 适配, 支持多语言界面和 CJK 字体渲染. 

![](./docs/images/cover.jpg)

## ✨ CJK 版新增功能

### 🌏 多语言界面支持 (I18n)

- **完整本地化**：支持中文、英文、日语三种界面语言
- 可在设置中随时切换界面语言
- 所有菜单、提示、设置项均已完全本地化
- 基于字符串 ID 的动态翻译系统

### 📝 CJK 字体系统

- **外置字体支持**：
  - **阅读字体**：用于书籍内容 (可选大小和字体族) 
  - **UI 字体**：用于菜单、标题和界面
  - 字体共享选项：使用阅读字体作为 UI 字体以节省内存
- LRU 缓存优化, 提升 CJK 渲染性能
- 内置 ASCII 字符回退机制, 减少内存占用
- 示例字体：
  - [思源黑体](fonts/SourceHanSansCN-Bold_20_20x20.bin) (UI 字体)
  - [京华老宋体](fonts/KingHwaOldSong_38_33x39.bin) (阅读器字体)

### 🎨 主题与显示

- **Lyra 主题**：现代化 UI 主题, 圆角选中高亮、滚动条分页、精致布局, 所有 CJK 页面已完全适配
- **深色/浅色模式切换**, 应用于阅读器和UI界面
- 无刷新的平滑主题切换

### 📖 阅读布局

- **首行缩进**：通过 CSS `text-indent` 实现段落缩进, 可开关切换
- 缩进宽度基于实际中文字符宽度计算
- **流式 CSS 解析**：处理大型样式表时不会内存溢出
- **CJK 字间距修复**：去除相邻 CJK 字符间的多余空格

## 📦 功能列表

- [x] EPUB 解析与渲染 (EPUB 2 和 EPUB 3)
- [x] EPUB 图片 alt 文本显示
- [x] TXT 纯文本阅读支持
- [x] 阅读进度保存
- [x] 文件浏览器 (支持嵌套文件夹) 
- [x] 自定义休眠画面 (支持封面显示) 
- [x] KOReader 阅读进度同步
- [x] WiFi 文件上传
- [x] WiFi OTA 固件更新
- [x] WiFi 凭证管理 (通过网页界面扫描、保存、删除)
- [x] AP 模式改进 (Captive Portal 支持)
- [x] 深色/浅色模式切换
- [x] 段落首行缩进
- [x] 多语言连字符支持
- [x] 字体、布局、显示样式自定义
  - [x] 外置字体系统 (阅读 + UI 字体)
- [x] 屏幕旋转 (UI/阅读器方向独立设置)
  - [x] 阅读界面屏幕旋转 (竖屏、横屏顺/逆时针、反转)
  - [x] UI 界面屏幕旋转 (竖屏、反转)
- [x] Calibre 无线连接与网页库集成
- [x] 封面图片显示
- [x] Lyra 主题 (所有 CJK 页面已适配)
- [x] KOReader 阅读进度同步
- [x] 流式 CSS 解析 (防止大型 EPUB 样式表内存溢出)

详细操作说明请参阅 [用户指南](./USER_GUIDE.md). 

## 📥 安装方式

### 网页刷写 (推荐)

1. 使用 USB-C 连接 Xteink X4 到电脑
2. 访问 https://xteink-flasher-cjk.vercel.app/ 并点击 **"Flash CrossPoint CJK firmware"** 即可直接刷入最新 CJK 固件

> **提示**：恢复官方固件或原版 CrossPoint 固件可在同一页面操作. 切换启动分区请访问 https://xteink-flasher-cjk.vercel.app/debug

### 手动编译

#### 环境要求

* **PlatformIO Core** (`pio`)
* Python 3.8+
* USB-C 数据线
* 阅星瞳 X4

#### 获取代码

```bash
git clone --recursive https://github.com/aBER0724/crosspoint-reader-cjk

# 若克隆后缺少子模块仓库
git submodule update --init --recursive
```

#### 构建并烧录

连接设备后运行:

```bash
pio run --target upload
```

## 🔠 字体相关

### 字体生成

- `tools/generate_cjk_ui_font.py`

- [点墨](https://apps.apple.com/us/app/dotink-eink-assistant/id6754073002) (推荐使用点墨来生成阅读字体, 便于查看字体在设备上的预览效果)

### 字体配置

1. 在 SD 卡根目录创建 `fonts/` 文件夹
2. 放入 `.bin` 格式的字体文件
3. 在设置中选择 "阅读/阅读器字体" 或 "显示/UI 字体"

**字体文件命名格式**：`FontName_size_WxH.bin`

示例：
- `SourceHanSansCN-Medium_20_20x20.bin` (UI: 20pt, 20x20)
- `KingHwaOldSong_38_33x39.bin` (阅读: 38pt, 33x39)

**字体说明**：
- **阅读字体**：用于书籍内容文本
- **UI 字体**：用于菜单、标题和界面元素

> 考虑到内存压力, 所以内置字体使用中文字符集很小, 仅包含了 UI 界面所需的字符.  
> 建议在 SD 卡中存入更为完整的 UI 字体和阅读字体, 获得更好的使用体验.  
> 若不便生成字体, 可点击下载上面提供的示例字体.

## ℹ️ 常见问题

1. 页数较多的章节首次打开时索引可能较慢. 流式 CSS 解析器已大幅改善此问题, 但超大章节仍可能需要等待数秒.
    > 若重启仍卡在索引中, 且长时间未能完成索引, 无法返回其他页面. 请重新刷入固件.
2. 若在某个界面卡住, 可尝试重启设备.
3. ESP32-C3 内存非常有限, 同时使用大型 CJK 字体文件作为 UI 字体和阅读字体可能导致内存溢出崩溃. 建议 UI 字体选择 20pt 及以下的字号.
4. 添加新书后首次打开主页时, 设备会生成封面缩略图, 可能出现 "加载中" 弹窗并等待数秒, 这是正常现象, 并非设备卡死.

## 🤝 参与贡献

如果你是首次参与本项目, 建议先阅读 [贡献文档](./docs/contributing/README.md).

提交 PR 前建议先在本地完成以下检查:

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

如果改动涉及连字符逻辑, 请额外执行单项测试:

```sh
./test/run_hyphenation_eval.sh english
```

- PR 标题建议使用语义化格式, 并填写 `.github/PULL_REQUEST_TEMPLATE.md`.
- 如果你使用 AI 编码助手/Agent, 请额外阅读 [AGENTS.md](./AGENTS.md).

## 📜 致谢

- [CrossPoint Reader](https://github.com/daveallie/crosspoint-reader) - 原始项目
- [open-x4-sdk](https://github.com/daveallie/open-x4-sdk) - Xteink X4 开发 SDK

---

**声明**：本项目与 Xteink 或 X4 硬件制造商无关, 均为社区项目. 
