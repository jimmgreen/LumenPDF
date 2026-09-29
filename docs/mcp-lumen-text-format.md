# 文字格式条：统一到 LumenUI 控件体系

> 阶段性记录：下文保留当时的格式条/颜色弹层改造与测试。页内 RichEdit 随后已被 LumaText 控件替换；最新自动尺寸、双击与字号比例见 [页内文字体验记录](mcp-lumatext-inline-editing.md)。

日期：2026-09-23。用户已明确授权修改其 LUMENUI 项目中的通用控件问题。本轮不扩展 PDF 工具集，不把 PDF 逻辑搬入 UI 库。

> 当前状态：LumenUI 改造及颜色弹层双层外框修复已构建。应用 CTest、库完整回归和颜色单项验证通过；鼠标驱动的最终组合回归因检测到指针位置变化而安全停止，剩余桌面回归暂缓。下文明确区分最终构建、外观微调前的成功 GUI 流程，以及尚未重跑的项目。

## 应用侧

- `app/text_format_bar.h/.cpp` 改为有 owner 的无标题栏 LUMEN `Window`，由 Row/Column 布局。不再创建原生字体 ComboBox、格式按钮或系统颜色选择对话框。
- 字体使用可编辑 ComboBox，字号使用 NumberBox，粗体／斜体／下划线与对齐使用 ToggleButton；颜色使用 ColorPicker、常用色和使用／取消按钮。
- 外观遵循 LUMEN 的主题 token、圆角、焦点和 hover 反馈。与主窗口共用 `app/ui.h` 中的聚光强度；字形通过图标注册绘制，颜色块仅用于呈现用户选择的数据颜色。
- 格式条不抢页内输入焦点。鼠标格式操作返回原文字选区；键盘操作保留格式控件焦点。字体下拉可以超出小格式窗，不在窗口底部被截掉。
- 颜色实时预览；取消、Esc 或外点关闭时恢复打开色板前的颜色。颜色弹层只由 ShowPopup 绘制外壳，内容 Column 不再叠加 Card(Flyout)，避免重复背景、圆角和边框。字号的显示精度不改写原文档的未编辑小数字号。
- 空白／无效字体、非法字号拒绝提交并保留草稿。Ctrl+Enter 完成、Esc 取消；下拉打开时先处理弹层键盘。
- RichEdit 只保留为页内文字输入、选区、Unicode/IME、纯文本粘贴与文字撤销的实现，不再承担整套格式面板的外观。

## LUMENUI 中的通用修正

| 位置 | 行为 |
| --- | --- |
| `Window::Show(bool activate = true)` | `Show(false)` 显示非模态工具窗而不抢原生焦点；默认行为及显示事件保持兼容 |
| ComboBox | 按真实可用高度选择窗内 overlay 或独立弹出窗；展开异步化，UIA Expand 不等待用户关闭列表 |
| ComboBox | Tab 提交并离开下拉，Esc 先收起；取消尚未执行的展开不应让弹层复活；精确字体名提交同步选中行 |
| ComboBox 内嵌编辑区 | 复用一个控件边框；编辑区的焦点统一反映到外框，而不是画两个输入框 |
| `TextBox::ImeEnabled(bool)` | 为严格 ASCII 字段临时停用本 LUMEN HWND 的 IME，离开后恢复原有上下文，不切换全局语言或宿主窗口设置 |
| NumberBox / ColorPicker hex | 默认不用组字，避免输入十六进制字母时进入中文候选、随后吞掉 Tab／Enter |
| CursorShape / 窗口输入路由 | 增加两种对角、移动、十字光标，保留原有枚举值；应用移除自己的 Win32 光标 subclass |

公开使用说明同步到 `../LUMENUI/skills/lumen/references/use.md`。库侧改动不涉及 MuPDF、批注数据、字体嵌入或文档事务。

## 生命周期与保存边界

长期格式条是 owned Window，短期下拉／颜色选择才使用借用内容的 popup。延后工作使用会话令牌或 WeakRef；原生消息观察回调只排队，不在回调内销毁控件或泵消息。

文字、样式和框几何继续使用原有应用／core 事务。手调宽高、固定框、空白提交、取消、文档撤销与文字撤销的语义不变。字体仍从本机解析并真实嵌入 PDF；只支持整条批注样式，不提供选区混排或外部字体文件导入。失败保存不覆盖源文件。

发布库修改时，对全部目标文件先校验 SHA-256，再应用精确字节补丁；逐文件保留原始字节备份并验证输出。未 reset、clean 或覆盖库中已有的 LumaText 工作。

## 验证入口

```bash
MSYS_NO_PATHCONV=1 cmd.exe /c build.bat
MSYS_NO_PATHCONV=1 cmd.exe /c "..\LUMENUI\build.bat"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/text_smoke.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/annotation_smoke.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/editor_smoke.ps1 -Case cancel
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/editor_smoke.ps1 -Case commit
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/editor_smoke.ps1 -Case unchanged
```

库测试在 `../LUMENUI/build` 运行 `lumen_visual_test.exe`、`lumen_perf_test.exe`、`lumen_anim_test.exe`、`lumen_api_test.exe`，并检查 Gallery。GUI 脚本按窗口标题、UIA Value/Toggle 等模式及真实键盘／鼠标操作，不再把旧原生子控件 ID 当成新格式控件。

界面产物位置：`build/app/lumen-text-output/`。最终验收还必须核对保存重开的 PDF 内容与原始文件哈希，不能仅凭截图或 UIA 无错误判定成功。

## 本轮验证记录

| 验证项 | 结果与对应范围 |
| --- | --- |
| 最终应用构建 | 成功；移除颜色内容容器的重复 Card 外框后重新构建 |
| 最终 CTest | 4/4 通过，共 47.81 s；canvas 121、annotation 91、text 70 项断言 |
| LUMENUI 独立构建 | 成功；命名检查通过 |
| 库 visual | 1559 项 PASS，0 项 FAIL；包括 12 次 HostCycle、独立字体弹层、Tab、待展开取消和扩展光标 |
| 库像素检查 | editable ComboBox 单层输入边框在 100% / 150% / 200% 渲染比例通过 |
| 库 perf | 300 帧平均 4.354 ms，低于 8 ms 平均预算；最慢单帧 30.758 ms，不声称每帧均小于 8 ms |
| 库 anim / api / table-api | 均 exit 0，通过 |
| Gallery | 150% 紧凑／低聚光与 200% 常规／满聚光 Input 页截图生成成功 |
| 文字完整 GUI | 单层颜色外框微调之前，60 项界面检查及 10 项保存重开核心断言通过；150% DPI、1320×860 与 1050×720 DIP 窗口 |
| 最终颜色单项验证 | 通过；用指定进程的 UIA／窗口消息打开色板、输入色值、截图、取消色板及文字编辑；源 PDF 哈希未变；不使用全局鼠标／键盘输入 |
| 修改文件一致性 | 应用及库目标文件全部 SHA-256 对齐发布记录；保留原始字节备份 |
| 尚未完成的最终桌面组合回归 | 串联 text / annotation / editor 的最后一次运行在检测到指针移动后安全停止；本轮六类旧工具完整 GUI 与 editor 的三个 GUI 用例未重新执行，不引用上轮成功记录替代 |

文字 GUI 中，框由 403×105 px 拖成 403×189 px，左上点与宽度不变；保存重开检查固定高度与字体嵌入。手动检查了新格式条、完整颜色弹层、字体列表和紧凑布局。

库 visual 的原有异常记录器会把 `0x40010006` / `0x4001000A` 系统 DebugPrint 通知标成 `[crash]`；本轮相应进程为 exit 0、ALL PASS，不是未处理的访问违规退出。未改写这部分已有诊断逻辑。

最终程序：`build/app/LumenPDF.exe`。SHA-256：

```text
598d3dd328e78afae96a30c7c7e0999717618a1e46b75eef2b3089c752eeff68
```

最新颜色弹层实拍：`build/app/lumen-text-output/09-color-single-surface.png`。其内容容器不绘制外框；只保留 ShowPopup 的外层背景、圆角与描边。此前 `08-lumen-color-picker.png` 是双层外框修正前的记录，不应作为最终外观展示。

库完整结果在 `build/mcp-lumen-style/library-results.json`；日志及 Gallery 截图也位于该目录。最终色板的无鼠标捕获脚本为 `build/mcp-lumen-style/capture-palette.ps1`。

## 待人工验收的边界

- 完整中文／日文 IME 候选、特殊字体和复杂脚本编辑体验。
- 跨显示器 DPI 切换、触控／笔输入与长时间使用的拖拽手感。
- 弹层完整读屏体验、Acrobat 互操作与真实大型文件压力测试。

这些项目不因自动化脚本通过而宣称完成。
