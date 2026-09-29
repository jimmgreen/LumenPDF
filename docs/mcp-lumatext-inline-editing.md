# 内容自适应、双击编辑与 LumaText 页内文字

> 后续补充：真实 Mouse-in-Pointer 双击识别及进入编辑时不切换工作区／工具的修复，见 [真实双击原位编辑记录](mcp-double-click-inplace.md)。本文中的定向窗口消息测试不能替代该后续真实鼠标验证。

日期：2026-09-23。范围为已有 FreeText 文字批注，不扩展成 PDF 正文重排、OCR、表单编辑或完整 Acrobat 协作系统。用户授权继续完善 LUMENUI，PDF 数据和事务仍留在 LumenPDF core/app。

## 交互基准与尺寸

用户指定按 Acrobat 的常规操作。Adobe 的文字批注说明明确包括：点击添加文字、文字到框右边界自动换行、选择边框调整尺寸，以及双击文本框编辑内容和属性。[2](https://helpx.adobe.com/sg/acrobat/using/commenting-pdfs.html)

本轮把这些行为落实为：

- **单击选中，双击文字进入编辑。** 文字工具点击已有文字不再重复创建批注，也不在第一次单击时抢走编辑焦点。
- 点击空白处添加的短文字按内容决定宽高，不再继承 220×36 的预设框，更不把“水电费”三个字塞在数百点高的大矩形中。
- 拖出文本框或横向调整宽度时，指定宽度成为换行约束，输入／删除和改字号会重新计算所需高度。
- 显式调整上下／角点仍保留手动尺寸意图，框尺寸不缩放字号；需要时用「适合文字」恢复内容自适应。自动排版受当前 PDF 页面边界限制，不向下一页续排。
- 进入编辑仅改变视图排版，不立即改写 PDF。取消或没有修改就完成，源文件与文档事务不变。
- 内边距使用 2 PDF pt，选框来自内容布局，拖动期间的方向光标仍由 LUMEN 输入路由提供。

这里没有把表单的 Auto 字号、Callout 或正文编辑工具混为同一种行为；也不以社区反馈替代所有版本的 Acrobat 实机验收。

## 为什么 12 pt 在适合页面时较小

PDF 的 1 pt = 1/72 英寸；界面的 1 DIP = 1/96 英寸。**100% 页面比例时，12 pt 对应 16 DIP**，再由屏幕 DPI 转换成物理像素。适合整页时页面往往低于 100%，文字自然随页面缩小。

本轮把「适合页面」与真实比例区分开，页脚显示实际百分比并提供 **100%** 入口。文字编辑期间也可缩放，字号保持 PDF pt，不把 12 pt 偷偷改大后保存。版面缩放和屏幕 DPI 只应用各自的一次转换。

## 同一套文字布局与绘制

- `app/inline_editor.h/.cpp` 不再创建或加载 RichEdit。输入控件是挂在 PDF 画布子树上的 LUMEN TextBox；不再有覆盖页面的白色原生文本窗口。
- 新增 LUMEN 公共 `TextLayout` / `TextTypography`，以一份段落布局提供换行、字形位置、点击命中、选区矩形和 IME 插入符位置。
- 段落布局由现有文本服务构建，字形栅格化通过 **LumaText 的 shaped-layout 绘制路径**完成；不是把 DirectWrite 回退改个名称。任意系统字体、粗体、斜体及下划线可使用同一条绘制路径。
- TextBox 的段落能力是显式开启的：`Typography(...)`、`WordWrap(true)`、`ContentPadding(...)`、`Chrome(false)` 等。旧的单行输入、NumberBox、ComboBox 编辑区保持原有默认行为。
- 可安全合成的 Lumen 文字批注在显示态和编辑态使用相同 TextLayout 与 LumaText 绘制。正文、扫描页面、复杂外来批注仍由 MuPDF 处理。
- 对存在后续遮挡的文字批注采用保守叠放保护：保留 PDF 原外观，不让 UI 文字层错误地浮到后画的矩形、图片或其他批注之上。编辑态仍是 LumaText，但不声称所有外来复杂外观都与 PDF 引擎像素相同。
- IME 预编辑串在共享布局中显示，不预先删除被替换的选区；取消保留原文，提交才替换，替换可一次撤销。

控件字体和绘制能力在 LUMENUI，PDF 字体解析、真实嵌入、批注顺序及保存全部仍在 core/worker。

## 实现位置

| 模块 | 位置 |
| --- | --- |
| 公共段落布局 | `../LUMENUI/include/lumen/TextLayout.h`、`../LUMENUI/src/core/text_layout.cpp` |
| 字体/布局服务与 LumaText 桥 | `../LUMENUI/src/core/text_service.*`、`../LUMENUI/src/core/lumatext_bridge.*` |
| TextBox 段落、命中、选区、IME | `../LUMENUI/include/lumen/TextBox.h`、`../LUMENUI/src/controls/text_box.cpp` |
| 页内输入与 pt/DIP 配置 | `app/inline_editor.*`、`app/text_layout.h` |
| 文字层、选择、双击、缩放 | `app/canvas.*` |
| 草稿尺寸与完成/取消 | `app/application.*`、`app/annotation_tools.cpp` |
| PDF 绘制排除与尺寸元数据 | `core/document.*` |

新增 `LumenTextSizing` 元数据区分自动、限定宽度和手动尺寸；标准 PDF 文字内容、字体、外观及编辑能力仍然存在，不依赖私有 UI 才能查看。进入编辑不保存，失败不覆盖源，PDF 引擎访问继续由 worker 串行处理。

## 验证入口与当前记录

```bash
MSYS_NO_PATHCONV=1 cmd.exe /c build.bat
MSYS_NO_PATHCONV=1 cmd.exe /c "..\LUMENUI\build.bat"
# 从 LUMENUI/build 执行针对性的像素/输入检查
./lumen_visual_test.exe --paragraph
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/text_smoke.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/editor_smoke.ps1 -Case cancel
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/editor_smoke.ps1 -Case unchanged
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/editor_smoke.ps1 -Case commit
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/annotation_smoke.ps1
```

最终版本已完成以下验证。增加批注叠放保护后重新构建并重跑了应用 CTest、完整文字 GUI、取消／无改动及六工具 GUI；不引用上轮 EXE 的结果替代本轮。

| 验证 | 本轮结果 |
| --- | --- |
| 应用构建 | 成功，编译日志无新增 warning/error |
| CTest | 4/4 通过，共 40.10 s；canvas 122、annotation 91、text 75 项断言 |
| 文字完整 GUI | 18 项界面断言通过；保存重开额外 7 项核心断言通过 |
| 取消／无改动 | 各 10 项界面断言通过，源 PDF 哈希不变 |
| 单独提交流程 | 10 项界面断言及保存核对通过；最终完整 GUI 再次验证提交保存 |
| 六工具 GUI | 便签、高亮、矩形、箭头、手绘、图片流程通过；保存重开 13 项断言 |
| LUMENUI visual | 1589 项 PASS，0 项 FAIL，包含 12 次 HostCycle |
| LUMENUI perf | 300 帧平均 5.097 ms，最慢 32.693 ms；平均值满足 <8 ms 预算，不声称每帧均小于 8 ms |
| anim / api / table-api | 全部 exit 0；命名检查通过 |
| Gallery | Input 页 150% 紧凑/低聚光与 200% 常规/满聚光截图完成 |
| 源码一致性 | 本轮应用与库修改均经 SHA-256 校验匹配；库保留原字节备份 |

实机为 150% DPI。`水电费` 三个字在适合页面时的输入区为 **44×20 px**，不再是数百像素的大框；切换 100% 后字号仍然是 12 pt。输入增长、显式换行、删除收缩及字号改变均通过 UI 检查。

程序：`build/app/LumenPDF.exe`，SHA-256：

```text
afc8796d1897f9fd042d2003f575a03e881a5ec84109e0ded5905ff68c662abb
```

主要实拍：
- `build/app/inline-luma-output/01-short-text-selected.png`：短文字的内容选框。
- `build/app/inline-luma-output/02-lumatext-inline-fit.png`：保持页面比例进入编辑。
- `build/app/inline-luma-output/03-12pt-at-100-percent.png`：12 pt、100% 页面比例。
- `build/app/inline-luma-output/04-content-sized-two-lines.png`：换行后的自适应框。
- `build/app/inline-luma-output/05-same-renderer-after-commit.png`：完成后仍走一致的文字绘制。

库的原有异常观察器会把 Windows `0x40010006` / `0x4001000A` DebugPrint 通知标成 `[crash]`；上述库进程全部 exit 0、ALL PASS，并非未处理的访问违规退出。本轮没有为清理这些既有日志而扩大修改范围。


LUMEN 段落回归在 100% / 150% / 200% 渲染比例比较显示态和编辑态的字形像素，均一致；同时检查真实 FreeType/LumaText 绘制计数、普通/斜体字体没有进入外层 DirectWrite 回退。另有软换行行尾亲和性、上下/Home 导航、跨行选区、IME 取消及一次撤销回归。

新的文字脚本按 UIA 和指定进程窗口消息操作，**不移动鼠标、不更改全局键盘状态**，但这不等于真实双击手感与真人 IME 候选已完成验收。GUI 产物在 `build/app/inline-luma-output/`；库结果及 Gallery 在 `build/mcp-inline-text/`。

## 仍需人工验证的范围

- 真人中文／日文输入法候选选择、长时间连续输入、拖选手感与触控/笔。
- 跨显示器 DPI 切换、非常密集的批注、复杂脚本及特殊字体。
- 外来混排 FreeText、旋转文字、表单遮挡和 Acrobat 互操作的完整验收。
- 格式仍按整条批注应用，不承诺选区混排或外部字体文件导入。

不把定向窗口消息、单次截图或“能启动”当成上述人工项目的验收。
