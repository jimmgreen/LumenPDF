#pragma once
// 主页“最近打开”视图：继续阅读卡片（首页缩略图）+ 按时间分组的文件列表。
// 所有文件系统访问与 PDF 渲染由宿主通过 request_info 放到引擎线程完成，UI 线程只绘制。
#include "recent_files.h"
#include "core/document.h"
#include "core/conversion.h"
#include <lumen/Control.h>
#include <lumen/Painter.h>
#include <d2d1_3.h>
#include <wrl/client.h>
#include <functional>
#include <optional>
namespace lpdf {
struct RecentInfo {
    enum class State { Unknown, Present, Missing };
    State state{State::Unknown};
    uint64_t bytes{};
    int pages{-1};
    bool locked{};      // 需要密码，无法生成缩略图
    Bitmap thumb;       // 首页缩略图（BGRA），可能为空
};
class RecentView final : public lumen::Control {
public:
    RecentView();
    void Entries(const std::vector<RecentEntry>& entries);
    void Filter(std::wstring text);
    void AcceptInfo(const fs::path& path,int64_t opened,RecentInfo info);
    size_t VisibleCount()const noexcept{return rows_.size();}
    size_t TotalCount()const noexcept{return items_.size();}
    std::function<void(const fs::path&)> open,remove,reveal,copy_path;
    std::function<void(const fs::path&,bool)> pin;
    std::function<void(const fs::path&,int64_t)> request_info;
    std::function<void()> browse;   // 空状态里的“打开文档”
protected:
    lumen::Size Measure(lumen::Size available,const lumen::Theme&) override;
    void Arrange(const lumen::Rect&) override;
    void Prepare(lumen::Painter&) override;
    void Draw(lumen::Painter&,const lumen::Theme&) override;
    bool OnAnimate(float dt) override;
    bool OnWheel(float delta) override;
    bool OnKey(uint32_t key) override;
    void OnMouseMove(lumen::Point,uint32_t) override;
    void OnMouseDown(lumen::Point,uint32_t) override;
    void OnMouseUp(lumen::Point,uint32_t) override;
    void OnMouseLeave() override;
    bool ShowContextMenu(lumen::Point window_dip) override;
    lumen::CursorShape CursorAt(lumen::Point) const override;
    bool Focusable()const noexcept override{return true;}
    void OnFocusChanged(bool focused) override;
private:
    friend struct RecentViewTestAccess;
    struct Item {
        RecentEntry entry; InputFileKind kind{}; std::wstring name,folder,lowered;
        RecentInfo info; bool requested{};
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        mutable std::wstring shortFolder; mutable float shortWidth{-1};
    };
    enum class Part { None, Card, Row, Pin, Reveal, Remove, Empty };
    struct Hit { Part part{Part::None}; int item{-1}; };
    struct Slot { int item; lumen::Rect rect; };
    struct Header { std::wstring text; float y; };
    void Layout();
    Hit HitTest(lumen::Point local)const;
    lumen::Rect ActionRect(const lumen::Rect& row,int index)const;
    lumen::Rect Screen(const lumen::Rect& content)const{return {absolute_.x+content.x,absolute_.y+content.y-scroll_,content.w,content.h};}
    void DrawCard(lumen::Painter&,const lumen::Theme&,const Slot&)const;
    void DrawRow(lumen::Painter&,const lumen::Theme&,const Slot&)const;
    void DrawEmpty(lumen::Painter&,const lumen::Theme&)const;
    void Activate(int item,Part part);
    void Reveal(int row);
    void ScrollTo(float y,bool smooth=true);
    const std::wstring& ShortFolder(lumen::Painter&,const Item&,float width)const;
    std::vector<Item> items_;
    std::vector<Slot> cards_,rows_;
    std::vector<Header> headers_;
    std::wstring filter_;
    float scroll_{},target_{},content_{},cardsTop_{},emptyButtonY_{};
    int64_t now_{};
    Hit hover_,pressed_;
    int selected_{-1};   // rows_ 下标（键盘焦点）
    bool focused_{};
    void* device_{};
};
std::wstring FormatBytes(uint64_t bytes);
}
