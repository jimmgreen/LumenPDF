#pragma once
// 签名图片：手写笔迹 / 输入文字 / 导入图片 → 透明背景 PNG（放置时作为图片批注写入 PDF）。
// 与 PDF 无关的图像处理，使用 GDI+；所有函数可在任意线程调用。
#include "document.h"
namespace lpdf {
enum class SignatureKind { Drawn, Typed, Image };
// 返回的位图均为“非预乘”BGRA（与 PNG 一致）；界面绘制前用 Premultiplied() 转换。
// strokes：任意单位的折线（例如签名板 DIP 坐标）；penWidth 同单位；scale 为输出像素 / 单位。
Bitmap RenderDrawnSignature(const std::vector<std::vector<Point>>& strokes, float penWidth, uint32_t rgb, float scale = 4);
// 文字签名：family 未安装时依次回退到 楷体 / 微软雅黑。pixelHeight 为字号（像素）。
Bitmap RenderTypedSignature(std::wstring_view text, std::wstring_view family, uint32_t rgb, float pixelHeight = 160);
// 本机是否安装了该字体族（GDI+ 可用的常规字形）。
bool SignatureFontInstalled(std::wstring_view family);
// 导入图片（PNG/JPG/BMP）：长边缩到 maxSide 以内；removeBackground 时把纸张底色（估计的亮背景）变透明，
// 并去除边缘白边；最后裁掉四周透明区域。找不到笔迹时抛出异常。
Bitmap ImportSignatureImage(const fs::path& image, bool removeBackground, int maxSide = 1600);
// 裁掉 alpha≤threshold 的四周并留 margin 像素；全透明时返回空位图。
Bitmap CropTransparent(const Bitmap& source, int margin = 4, unsigned char threshold = 8);
// 先写临时文件再原子替换；失败不会留下半个文件。
void SaveSignaturePng(const Bitmap& bitmap, const fs::path& destination);
Bitmap LoadSignaturePng(const fs::path& file);
Bitmap Premultiplied(Bitmap bitmap);
// 缩放到 maxW×maxH 以内（保持比例，只缩小），用于列表预览。
Bitmap ScaledToFit(const Bitmap& source, int maxW, int maxH);

// 签名库：folder 下的 sig-<kind>-<毫秒时间戳>.png，按创建时间新→旧排列。
inline constexpr size_t MaxSignatures = 8;
struct SignatureEntry { fs::path file; SignatureKind kind{}; int64_t created{}; };
std::vector<SignatureEntry> ListSignatures(const fs::path& folder);
// 保存到签名库；已达 MaxSignatures 时抛出异常（不自动删除旧签名）。
SignatureEntry AddSignature(const fs::path& folder, SignatureKind kind, const Bitmap& bitmap);
void RemoveSignature(const SignatureEntry& entry);
// ✓ / ✗ 标记的笔画（页面坐标），box 为标记所占方框。
std::vector<std::vector<Point>> CheckMarkStrokes(Rect box);
std::vector<std::vector<Point>> CrossMarkStrokes(Rect box);
}
