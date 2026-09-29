#pragma once
#include "core/document.h"
#include <lumen/TextLayout.h>
#include <algorithm>
#include <cmath>
namespace lpdf {
inline lumen::TextTypography AnnotationTypography(const Annotation& a,float scale=1){
    lumen::TextTypography style;
    style.family=a.textFormat.family;style.size=a.fontSize*scale;
    style.weight=a.textFormat.bold?700:400;style.italic=a.textFormat.italic;
    style.underline=a.textFormat.underline;style.line_height=a.fontSize*1.2f*scale;
    style.alignment=a.textFormat.alignment==1?lumen::Align::Center:a.textFormat.alignment==2?lumen::Align::Trailing:lumen::Align::Leading;
    return style;
}
// Sizes are PDF points here. Multiplying once by the page's DIP/point scale
// happens when placing/drawing; display DPI is applied only by Painter.
inline Rect FitTextBounds(const Annotation& a,const PageInfo& page,lumen::TextLayout& layout){
    const float right=page.originX+page.width,bottom=page.originY+page.height;
    const float available=std::max(6.0f,right-a.bounds.x);
    float width=a.textSizing==TextSizing::Auto?available:std::clamp(a.bounds.w,6.0f,available);
    auto style=AnnotationTypography(a);
    layout.Layout(a.text,style,std::max(.5f,width-4.0f),true);
    if(a.textSizing==TextSizing::Auto){
        const float minimum=std::min(available,a.text.empty()?a.fontSize+4.0f:6.0f);
        width=std::clamp(std::ceil((layout.ContentSize().w+4.5f)*64.0f)/64.0f,minimum,available);
        layout.Layout(a.text,style,std::max(.5f,width-4.0f),true);
    }
    float height=std::max(a.fontSize*1.2f+4.0f,layout.ContentSize().h+4.0f);
    if(a.textSizing==TextSizing::Fixed)height=std::max(height,a.bounds.h);
    height=std::min(height,page.height);
    float x=a.bounds.x;
    if(a.textSizing==TextSizing::Auto&&a.textFormat.alignment==1)x+=(a.bounds.w-width)*.5f;
    if(a.textSizing==TextSizing::Auto&&a.textFormat.alignment==2)x+=a.bounds.w-width;
    return {std::clamp(x,page.originX,std::max(page.originX,right-width)),
            std::clamp(a.bounds.y,page.originY,std::max(page.originY,bottom-height)),width,height};
}
}
