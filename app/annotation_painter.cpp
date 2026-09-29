#include "annotation_painter.h"
#include <d2d1_3.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

namespace lpdf {
using Microsoft::WRL::ComPtr;
void DrawAnnotationPreview(lumen::Painter& painter,const Annotation& a,float scale,
                           const std::function<lumen::Point(Point)>& screen) {
    auto* dc=painter.DeviceContext();if(!dc)return;
    ComPtr<ID2D1Factory> factory;dc->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> geometry;ComPtr<ID2D1GeometrySink> sink;
    if(FAILED(factory->CreatePathGeometry(&geometry))||FAILED(geometry->Open(&sink)))return;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    auto point=[&](Point p){const auto q=screen(p);return D2D1::Point2F(q.x,q.y);};
    auto begin=[&](Point p,bool fill){sink->BeginFigure(point(p),fill?D2D1_FIGURE_BEGIN_FILLED:D2D1_FIGURE_BEGIN_HOLLOW);};
    auto line=[&](Point p){sink->AddLine(point(p));};
    bool fill=false,stroke=true;float markupWidth=-1;
    if(a.type==Tool::Rectangle){
        // PDF Square's Rect includes the stroke; keep the preview inside it too.
        const float inset=std::min({a.style.lineWidth/2,a.bounds.w/2,a.bounds.h/2});
        const auto& r=a.bounds;begin({r.x+inset,r.y+inset},a.style.filled);
        line({r.x+r.w-inset,r.y+inset});line({r.x+r.w-inset,r.y+r.h-inset});line({r.x+inset,r.y+r.h-inset});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);fill=a.style.filled;
    }else if(a.type==Tool::Ellipse){
        const float inset=std::min({a.style.lineWidth/2,a.bounds.w/2,a.bounds.h/2});
        const auto& r=a.bounds;const float rx=r.w/2-inset,ry=r.h/2-inset,cx=r.x+r.w/2,cy=r.y+r.h/2;
        begin({cx+rx,cy},a.style.filled);
        for(int i=1;i<=64;++i){const float t=6.2831853f*i/64;line({cx+rx*std::cos(t),cy+ry*std::sin(t)});}
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);fill=a.style.filled;
    }else if(IsLineTool(a.type)&&a.points.size()==2){
        begin(a.points[0],false);line(a.points[1]);sink->EndFigure(D2D1_FIGURE_END_OPEN);
        for(int end=0;end<2&&a.type==Tool::Arrow;++end){
            const int cap=end?a.style.endEnding:a.style.startEnding;if(cap!=4&&cap!=5)continue;
            const auto tip=a.points[end],other=a.points[1-end];
            const float length=std::hypot(other.x-tip.x,other.y-tip.y);if(length<.01f)continue;
            const float dx=(other.x-tip.x)/length,dy=(other.y-tip.y)/length,r=std::max(1.0f,a.style.lineWidth);
            // Same proportions as MuPDF's standard OpenArrow/ClosedArrow AP.
            begin({tip.x+8.8f*r*dx-4.5f*r*dy,tip.y+8.8f*r*dy+4.5f*r*dx},cap==5);
            line(tip);line({tip.x+8.8f*r*dx+4.5f*r*dy,tip.y+8.8f*r*dy-4.5f*r*dx});
            sink->EndFigure(cap==5?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN);fill|=cap==5;
        }
    }else if(a.type==Tool::Ink&&!a.points.empty()){
        size_t offset=0;
        const auto counts=a.strokes.empty()?std::vector<int>{static_cast<int>(a.points.size())}:a.strokes;
        for(int count:counts){
            if(count>0&&offset<a.points.size()){
                begin(a.points[offset],false);
                for(int i=1;i<count&&offset+static_cast<size_t>(i)<a.points.size();++i)line(a.points[offset+i]);
                if(count==1)line(a.points[offset]);
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
            }
            offset+=std::max(0,count);
        }
    }else if(a.type==Tool::Highlight){
        for(const auto& q:a.quads){begin(q.ul,true);line(q.ur);line(q.lr);line(q.ll);sink->EndFigure(D2D1_FIGURE_END_CLOSED);}
        fill=true;stroke=false;
    }else if(a.type==Tool::Underline||a.type==Tool::StrikeOut){
        // 与 MuPDF 外观相同的位置：下划线贴近底边，删除线位于中线。
        for(const auto& q:a.quads){
            const float t=a.type==Tool::Underline?.93f:.55f;
            const Point l{q.ul.x+(q.ll.x-q.ul.x)*t,q.ul.y+(q.ll.y-q.ul.y)*t},r{q.ur.x+(q.lr.x-q.ur.x)*t,q.ur.y+(q.lr.y-q.ur.y)*t};
            begin(l,false);line(r);sink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        markupWidth=0;
        for(const auto& q:a.quads)markupWidth=std::max(markupWidth,std::hypot(q.ll.x-q.ul.x,q.ll.y-q.ul.y)/14);
        markupWidth=std::max(.5f,markupWidth);
    }
    if(FAILED(sink->Close()))return;
    auto color=[](uint32_t rgb,float alpha){return D2D1::ColorF(rgb,alpha);};
    ComPtr<ID2D1SolidColorBrush> ink,interior;
    if(FAILED(dc->CreateSolidColorBrush(color(a.style.color,a.opacity),&ink)))return;
    if(fill){
        const uint32_t rgb=IsBoxShape(a.type)?a.style.fillColor:a.style.color;
        if(SUCCEEDED(dc->CreateSolidColorBrush(color(rgb,a.opacity),&interior)))dc->FillGeometry(geometry.Get(),interior.Get());
    }
    if(stroke){
        const bool round=a.type==Tool::Ink;
        auto props=D2D1::StrokeStyleProperties(round?D2D1_CAP_STYLE_ROUND:D2D1_CAP_STYLE_FLAT,
            round?D2D1_CAP_STYLE_ROUND:D2D1_CAP_STYLE_FLAT,D2D1_CAP_STYLE_FLAT,
            round?D2D1_LINE_JOIN_ROUND:D2D1_LINE_JOIN_MITER,10,
            a.style.dashed&&markupWidth<0?D2D1_DASH_STYLE_CUSTOM:D2D1_DASH_STYLE_SOLID,0);
        const float width=markupWidth>0?markupWidth:std::max(.1f,a.style.lineWidth);
        const float dashes[]{4/width,3/width};ComPtr<ID2D1StrokeStyle> style;
        const bool dashed=a.style.dashed&&markupWidth<0;
        factory->CreateStrokeStyle(props,dashed?dashes:nullptr,dashed?2:0,&style);
        dc->DrawGeometry(geometry.Get(),ink.Get(),width*scale,style.Get());
    }
}
}