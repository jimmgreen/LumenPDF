#pragma once
#include <lumen/TabControl.h>
#include <algorithm>
namespace lpdf {
// Local extension: the dependency is read-only. Keep native overflow, keyboard and drag reorder.
class DocumentTabs final : public lumen::TabControl {
public:
    void Title(std::wstring_view id,std::wstring title){
        for(auto& item:items_)if(item.id==id){if(item.title!=title){item.title=std::move(title);RelayoutParent();Invalidate();}return;}
    }
};
}

namespace lpdf {
class DocumentSplit final : public lumen::Panel {
public:
    void LayoutMode(int mode){mode_=std::clamp(mode,0,2);dragging_=false;Relayout();}
    int LayoutMode()const{return mode_;}
    void Equalize(){ratio_=.5f;Relayout();}
protected:
    lumen::Size Measure(lumen::Size size,const lumen::Theme& theme)override{
        const float w=std::max(0.f,size.w),h=std::max(0.f,size.h);
        for(size_t i=0;i<ChildCount();++i)if(ChildVisible(i))MeasureChildAt(i,{mode_==1?(w-8)*.5f:w,mode_==2?(h-8)*.5f:h},theme);
        return {w,h};
    }
    void Arrange(const lumen::Rect& r)override{
        Control::Arrange(r);if(ChildCount()<2)return;
        const float extent=std::max(0.f,(mode_==1?r.w:r.h)-8),first=extent*ratio_;
        lumen::Rect a{0,0,r.w,r.h},b{};
        if(mode_==1){a.w=first;b={first+8,0,extent-first,r.h};}
        if(mode_==2){a.h=first;b={0,first+8,r.w,extent-first};}
        SetChildBounds(Child(0),a);ArrangeChildAt(0);
        if(mode_){SetChildBounds(Child(1),b);ArrangeChildAt(1);}
    }
    bool Divider(lumen::Point p)const{if(!mode_)return false;const float at=(mode_==1?absolute_.w:absolute_.h)-8;const float v=mode_==1?p.x:p.y;return v>=at*ratio_&&v<=at*ratio_+8;}
    lumen::CursorShape CursorAt(lumen::Point p)const override{return (dragging_||Divider(p))?(mode_==1?lumen::CursorShape::SizeWE:lumen::CursorShape::SizeNS):lumen::CursorShape::Arrow;}
    bool PrefersDragOverPan()const noexcept override{return true;}
    void OnMouseDown(lumen::Point p,uint32_t buttons)override{dragging_=(buttons&1)&&Divider(p);}
    void OnMouseMove(lumen::Point p,uint32_t)override{if(dragging_){const float extent=std::max(1.f,(mode_==1?absolute_.w:absolute_.h)-8);ratio_=std::clamp((mode_==1?p.x:p.y)/extent,.25f,.75f);Relayout();}}
    void OnMouseUp(lumen::Point,uint32_t)override{dragging_=false;}
private:
    int mode_{};float ratio_{.5f};bool dragging_{};
};
}
