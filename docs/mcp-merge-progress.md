# 合并队列：文件类型图标与真实阶段进度

日期：2026-09-23。用户要求保留易用的列表交互，区分不同格式，并为转换和合并提供明确进度。本轮不增加新的文件格式或在线服务。

## 界面

合并队列仍使用 **LumenUI ListView**，没有用自绘画布替换列表的选择、滚动或重排交互。

- PDF 使用带 PDF 标记的文件图标；Word 使用 W 文件图标；TXT 使用文本页图标；PNG/JPG/JPEG 使用图片图标。扩展名不区分大小写，未知格式保留通用图标并在处理时明确报错。
- 文件名作为主行，状态／类型／页码作为副行。长文件名省略显示不会把处理状态挤出可见区域。
- 保留拖动排序和上移／下移。页码范围与密码在选中项切换、排序和任务开始前读取实际输入值，不只依赖 TextChanged 事件，避免程序赋值或自动化输入未落入队列。
- 进度卡片显示阶段、当前文件、文件序号、已用时间、实际页面计数与取消按钮。任务期间锁定输入队列和转换设置，但不禁用进度区的取消操作。
- 完成后保留文件/页数与输出路径；取消及错误显示不同终态，并标明未处理或失败的文件。

所有控件外观继续使用 LUMEN 暗色单色主题。文件类型通过不同字形和文字区分，不依赖彩色主题。

## 进度含义

| 阶段 | 显示内容 | 进度处理 |
| --- | --- | --- |
| 1 / 3 准备与转换 | 当前文件 n/N、转换器、读取/文本分页/图片处理/页码检查 | 无可靠总量时使用不定进度；文本分页可显示已生成页数 |
| 2 / 3 合并页面 | 已合并页数 / 真实选中总页数；各文件已合并页数 | 使用按页面实际完成量推进的确定进度 |
| 3 / 3 写入与校验 | PDF 写入、验证、字体整理、安全发布 | 不定进度，不能把页面复制结束当作导出完成 |
| 完成 | 输出文件、文件数、页数、总用时 | 仅在安全写入成功后显示完成 |

Word / LibreOffice 的外部转换调用没有可用的精确百分比，所以界面显示当前文件、后端与已用时间，不用动画伪造百分比。合并前先核对页码与选中页数，再以实际页面复制量计算第二阶段进度。原有 `Document::Merge` 的按文件回调仍保持兼容。

转换得到的中间 PDF、最终 PDF 都经过既有保存校验。导出直接使用原有原子保存流程，避免为了进度显示重复写入两遍最终文档。取消在原子发布前仍可阻止替换；若安全写入已成功，不把随后到来的取消误报为“未导出”。

## 线程和控件改动

- `core/progress.h`：明确的阶段与实际计数，`total == 0` 代表分母未知。
- `core/conversion.*`：格式识别和转换事件。
- `core/document.*`：预检、按页合并及写入/校验/发布阶段事件；回调在 MuPDF 的 C 异常边界外执行。
- `app/merge_progress.h`：带锁的纯数据快照；worker 不直接改 UI。
- `app/merge_tools.cpp`：队列状态、阶段描述、取消与成功/失败处理。UI 通过窗口帧回调节流合并高频页面事件，避免为每页排入大量 UI 回调。
- `app/application.*`、`app/ui.h`：进度卡片、不同文件字形与队列设置同步。
- `../LUMENUI/include/lumen/ListView.h`、`../LUMENUI/src/controls/list_view.cpp`：新增通用 `ItemSecondaryText` 和 `RefreshItems`。副文本行高至少 52 DIP，测量、命中和滚动一致；刷新外部状态不重置选中、顺序或滚动，并使缓存绘制失效。

PDF 逻辑仍在 LumenPDF core/app；库只增加通用的列表呈现与刷新能力。所有原有 LumaText 工作和其他源码改动均保留，发布使用版本/SHA 保护与库原字节备份。

## 验证入口

```bash
MSYS_NO_PATHCONV=1 cmd.exe /c build.bat
MSYS_NO_PATHCONV=1 cmd.exe /c "..\LUMENUI\build.bat"
build/app/merge_tests.exe build/app/merge-output
# 仅在安装 Word 且没有正在使用的 Word 实例时执行真实 Office 检查
build/app/merge_tests.exe build/app/merge-office-output --office
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/merge_smoke.ps1 -Case export
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/merge_smoke.ps1 -Case preview
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/merge_smoke.ps1 -Case cancel
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/merge_smoke.ps1 -Case failure
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/merge_smoke.ps1 -Case word
```

新增核心回归覆盖：扩展名分类、文本/图片转换阶段、真实页数进度、原有文件进度回调兼容、合并顺序/页码范围/目录、取消、原子发布前取消、密码/范围失败及源文件不变。

列表回归覆盖双行命中高度、重排后的数据索引、状态刷新像素变化、选中项保留及副文本可访问名称。库 visual/perf/anim/api 与 Collections Gallery 按库规范验证。

GUI 测试使用 UIA 与指定进程窗口消息，不移动全局鼠标。它验证范围输入/排序、混合文件导出、预览、处理中取消、错误状态与源文件哈希；真实拖动手感、LibreOffice、复杂 Word 文档和极大型合并仍不由这些测试替代。

当前应用已编译且 CTest 5/5 通过；最终 UI/Office/库结果在全部完成后补充，不混用旧构建的结论。

产物目录：`build/app/merge-output/ui/`、`build/mcp-merge-progress/`。测试只使用项目自带夹具及其副本，不修改用户截图中所列的原始文件。
