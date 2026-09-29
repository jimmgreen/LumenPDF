# 文字工具：页内编辑、字体与八向缩放

> 上一阶段记录：本文中的原生格式条、应用层光标补丁与构建验收数据已由后续 [LumenUI 控件整合](mcp-lumen-text-format.md) 更新。以下测试数字及旧截图仅保留作历史记录，不代表当前构建。

日期：2026-09-23。范围：现有 FreeText 文字批注的直接编辑、整条批注的字体格式，以及框尺寸交互；不扩展原 PDF 正文编辑或完整 Acrobat 审阅体系。

## 使用方式

1. 选择「文字」后，点击已有文字批注即可进入编辑，不会再叠加新文本框。选择工具下仍可双击编辑。
2. 在文字框旁的浮动条中选择或输入本机字体，调整字号、粗体、斜体、下划线、颜色及左／中／右对齐。原文字选区在操作格式条后保留。主要文字编辑不依赖右侧属性面板。
3. Enter 换行；点击页面空白、「完成」或 Ctrl+Enter 提交；Esc／「取消」放弃整个草稿。打开字体列表或调色器不会触发提交。保存和切换工作区会先提交有效草稿。
4. Ctrl+B/I/U 改整条批注的粗体／斜体／下划线，Ctrl+L/E/R 改对齐。编辑中的 Ctrl+Z/Y 撤销／重做文字输入；布局和整条格式的程序化应用不占用文字撤销栈。提交整个草稿只产生一个 PDF 撤销事务。
5. **上、下中点只调高度；左、右中点只调宽度；四角同时调宽高。** 调框不缩放字号。控制点按实际方向显示 NS、WE、NWSE、NESW 光标，拖出画布时仍保持当前方向。
6. 手动缩放或拖框创建的文字使用固定框，保存重开、换字体、改字号后都保留手调宽高。纯移动不误记为手动缩放。内容超出固定框时，浮动条提示拉高框或点击「适合文字」；后者保持宽度、调整高度，并受页面边界限制。
7. 点击新建的非固定框在输入时自动增高，保留旧版本的自动贴合行为。居中、右对齐时不再收紧换行宽度，以免完成后文字横向跳动。

进入可见文字框不会自动放大或滚动页面；如果从面板编辑先前已滚出视区的对象，仅将它带回视区，不改缩放比例。

## 根因与修正

| 原问题 | 修正 |
| --- | --- |
| 选中对象所有控制点都返回横向光标 | 统一八向命中映射；应用自己的 Win32 subclass 处理鼠标／鼠标指针消息并提供对角光标，不修改只读 LUMENUI |
| 高度拖动后被核心自动排版收紧 | 模型与 PDF 保存 `fixedTextBox` 标记，固定框跳过自动收紧 |
| 编辑器固定微软雅黑，PDF 仅使用默认 Helv | 增加持久化 `TextFormat`，字体从 DirectWrite 定位，再由 MuPDF 生成真实嵌入字体的 FreeText 外观 |
| 文字工具点已有文字仍走新建 | 先命中已有文字，进入原对象的编辑事务并传入点击位置 |
| 格式仅在侧栏操作 | 新增框旁原生浮动格式条；取消、键盘、选区及字体弹层单独处理 |
| RichEdit 纯文本模式无法应用段落对齐 | 使用支持段落格式的编辑模式，但粘贴限定纯文本，格式针对整条批注 |

## 实现位置与保存语义

- `core/font_catalog.h/.cpp`：已安装本地字体枚举、字体文件定位、TTC face index、缺少字形样式时的合成粗／斜体、嵌入权限检查与缓存。
- `core/document.h/.cpp`：`TextFormat`、固定框状态、字体加载回调、FreeText 标准 `/RC`、`/DS`、`/DA`、`/Q` 及 Lumen 编辑元数据。字体进入 PDF 外观资源，不是只改变窗口字体。
- `app/text_format_bar.h/.cpp`：字体与字号输入、B/I/U、颜色、对齐、适合文字、完成／取消、溢出提示、紧凑窗口定位与原生键盘操作。
- `app/inline_editor.h/.cpp`：真实 RichEdit 光标、Unicode 输入、选区、换行、纯文本粘贴、段落格式、文字撤销，以及点入位置映射。
- `app/canvas.h/.cpp`：已有文字直接命中、空白处提交、八方向尺寸计算、固定框与纯移动区分、拖拽期间的方向光标。
- `app/application.h/.cpp`、`app/annotation_tools.cpp`：草稿事务、提交／取消、失败保留草稿、浮动条与编辑器的位置联动。

字体必须在本机可用且许可允许可编辑嵌入及子集化。不接受受限制、仅预览打印、禁止子集或仅位图嵌入的字体；不可用时拒绝提交并保留草稿，而不是静默宣称保存了该字体。未提供直接导入外部 TTF/OTF 文件的界面。

文字、格式、透明度及框几何一起进入已有核心事务。失败保存不覆盖原文件；进入／取消编辑不新建 PDF 记录。所有 PDF 引擎访问继续串行在 worker 上；UI 中的字体枚举不调用 PDF 引擎。这一阶段未修改 LUMENUI；后续已按用户授权修正通用控件，见上述整合记录。

## 验证

本机验证通过：`build.bat`、四组 CTest、文字原生界面及旧工具回归。测试环境为 Windows x64、150% DPI；窗口分别为 1320×860 与 1050×720 DIP。

| 验证项 | 结果 |
| --- | --- |
| `pdf_core` | 通过，20.52 s |
| `canvas_interactions` | 121 项断言通过，0.03 s |
| `annotation_tools` | 91 项断言通过，0.13 s |
| `text_editing` | 67 项断言通过，10.64 s |
| 四组 CTest 合计 | 4/4 通过，31.34 s |
| `text_smoke.ps1` | 50 项原生界面断言通过；其保存结果额外通过 10 项核心断言 |
| `annotation_smoke.ps1` | 六工具流程通过；保存重开通过 13 项断言 |
| 既有 `editor_smoke.ps1` | cancel、commit、unchanged 均通过；取消与无改动时源文件哈希不变 |

新文字原生回归随后以绝对鼠标输入连续执行两次，均为 50 项界面断言及 10 项保存重开断言通过。

原生鼠标拖高测试中，编辑窗口从 **403×105 px** 变为 **403×189 px**，左上位置未动，宽度不变；保存后核心同时核对 PDF 框宽、顶边、左边及固定高度状态。

本次程序 SHA-256：

```text
f5898c08f0c78def58d6b9bd1d60099b23c54a557b168e8e5bef070c9111922e
```

以上数字是这一次构建与完整回归的记录，不把历史测试、LSP 无诊断或界面截图单独当成字体保存正确的证据。

测试入口：

```powershell
.\build.bat
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/text_smoke.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/annotation_smoke.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/editor_smoke.ps1 -Case cancel
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/editor_smoke.ps1 -Case commit
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/editor_smoke.ps1 -Case unchanged
```

覆盖包括：

- Arial、Times New Roman、Courier New、Microsoft YaHei 的字体／样式／颜色／对齐往返；单独改变字体会改变真实 PDF 字形渲染。
- PDF 中存在实际 FontFile2/FontFile3 嵌入流；TTC 的非零 face index 与合成斜体保存重开外观一致。
- 固定框的创建、纵向加高／缩短、改字体、移动、撤销／重做与保存重开；无效字体失败不改原批注。
- 每个控制点的独立轴向、对边锚点、字号不变、拖出画布时的光标；小尺寸旧框的垂直调整不擅自加宽。
- 文字工具直接命中不叠框、空白提交、只读批注、进入编辑不跳变、滚出视区对象的无缩放定位。
- Native RichEdit 字体、字号、颜色、B/I/U、对齐、选区与文字撤销；浮动输入实际值提交、非法输入拦截。
- 实际原生程序中的字体输入、Windows 调色器、五种控制点位置的方向检查、下中点拖高、空白提交、取消、文档撤销／重做、两种窗口尺寸及重新启动后的回读。

测试脚本只操作自己启动的实例及测试副本。页内点击与拖高走 Windows 鼠标输入路径，发送前校验落点窗口归属测试 PID；方向验证读取系统当前光标，而不是只检查内部枚举。文字和颜色字段使用跨进程安全的 WM_SETTEXT，不依赖只写窗口标题的调用。Unicode 文本是程序化插入，不等同于真实中文 IME 候选窗测试。

## 产物

- 程序：`build/app/LumenPDF.exe`，仍需同目录运行依赖。
- 字体样例：`build/app/text-output/text-font-samples.pdf`。
- TTC 样例：`build/app/text-output/ttc-face-text.pdf`。
- 原生交互后保存的文档：`build/app/text-output/text-ui.pdf`。
- 截图及量化记录：`build/app/text-output/01-direct-edit.png` 至 `07-saved-and-reopened.png`、`text-ui-result.json`。
- 上一轮六工具样例及截图仍在 `build/app/annotation-output/`。

## 明确边界

- 本轮样式按**整条文字批注**应用，不支持选中部分文字混排字体、字号或颜色，也不声明完整保留外来复杂富文本样式。
- 这是 FreeText 批注，不是原有正文的段落编辑／重排；旋转文字仍有明确的暂不支持提示。
- 不保证每一种系统字体、复杂脚本、可变／符号字体都具有相同排版。所选字体缺字时，Windows 编辑预览和 MuPDF 的回退字体可能不同；中文建议选覆盖目标文字的字体。未声明跨引擎像素级一致。
- 未实测安装版 Acrobat 的编辑互操作、真实输入法候选窗、人工鼠标手感、触控／笔压或跨显示器 DPI 切换。
- 本轮未重新验收 Word／LibreOffice 转换，也不将本次文字测试视为这些功能的新证据。
