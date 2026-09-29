#pragma once
#include "document.h"
#include <algorithm>
#include <cmath>

namespace lpdf {
inline float PointDistance(Point a, Point b) { return std::hypot(a.x-b.x,a.y-b.y); }
inline Rect PointsRect(Point a, Point b) {
    return {std::min(a.x,b.x),std::min(a.y,b.y),std::max(.01f,std::abs(a.x-b.x)),std::max(.01f,std::abs(a.y-b.y))};
}
inline Point ClampPoint(Point p,const PageInfo& page) {
    return {std::clamp(p.x,page.originX,page.originX+page.width),std::clamp(p.y,page.originY,page.originY+page.height)};
}
inline Point ConstrainEndpoint(Point start,Point end,Tool tool,bool shift) {
    if(!shift)return end;
    const float dx=end.x-start.x,dy=end.y-start.y;
    if(IsBoxShape(tool)){
        const float edge=std::max(std::abs(dx),std::abs(dy));
        return {start.x+std::copysign(edge,dx),start.y+std::copysign(edge,dy)};
    }
    if(IsLineTool(tool)){
        constexpr float step=3.14159265358979323846f/4;
        const float angle=std::round(std::atan2(dy,dx)/step)*step,length=std::hypot(dx,dy);
        return {start.x+length*std::cos(angle),start.y+length*std::sin(angle)};
    }
    return end;
}
inline Point ConstrainToPage(Point start,Point end,Tool tool,bool shift,const PageInfo& page) {
    if(!shift||(!IsBoxShape(tool)&&!IsLineTool(tool)))return ClampPoint(end,page);
    end=ConstrainEndpoint(start,end,tool,true);
    const float dx=end.x-start.x,dy=end.y-start.y;float factor=1;
    if(dx>0)factor=std::min(factor,(page.originX+page.width-start.x)/dx);
    if(dx<0)factor=std::min(factor,(page.originX-start.x)/dx);
    if(dy>0)factor=std::min(factor,(page.originY+page.height-start.y)/dy);
    if(dy<0)factor=std::min(factor,(page.originY-start.y)/dy);
    factor=std::clamp(factor,0.0f,1.0f);
    return {start.x+dx*factor,start.y+dy*factor};
}
// Two endpoint-preserving Chaikin passes: no overshoot, no jagged joins, and
// exactly the same sampled curve in the live preview and the PDF InkList.
inline std::vector<Point> SmoothInk(const std::vector<Point>& raw) {
    std::vector<Point> points;points.reserve(raw.size());
    for(auto p:raw)if(points.empty()||PointDistance(points.back(),p)>.05f)points.push_back(p);
    if(points.empty())return {};
    if(points.size()==1)return {points.front(),points.front()};
    for(int pass=0;pass<2&&points.size()>2;++pass){
        std::vector<Point> next;next.reserve(points.size()*2);next.push_back(points.front());
        for(size_t i=1;i<points.size();++i){
            const auto a=points[i-1],b=points[i];
            next.push_back({a.x*.75f+b.x*.25f,a.y*.75f+b.y*.25f});
            next.push_back({a.x*.25f+b.x*.75f,a.y*.25f+b.y*.75f});
        }
        next.push_back(points.back());points=std::move(next);
    }
    return points;
}
inline Annotation TransformAnnotation(const Annotation& original,Rect to) {
    Annotation value=original;value.bounds=to;value.geometryEdited=true;
    const auto from=original.bounds;
    auto map=[&](Point p){return Point{to.x+(p.x-from.x)*to.w/std::max(.01f,from.w),to.y+(p.y-from.y)*to.h/std::max(.01f,from.h)};};
    for(auto& p:value.points)p=map(p);
    for(auto& q:value.quads){q.ul=map(q.ul);q.ur=map(q.ur);q.ll=map(q.ll);q.lr=map(q.lr);}
    return value;
}
inline float SegmentDistance(Point p,Point a,Point b) {
    const float dx=b.x-a.x,dy=b.y-a.y,len=dx*dx+dy*dy;
    const float t=len>.000001f?std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/len,0.0f,1.0f):0;
    return PointDistance(p,{a.x+t*dx,a.y+t*dy});
}
inline bool InQuad(Point p,const Quad& q,float tolerance=0) {
    const Point vertices[]{q.ul,q.ur,q.lr,q.ll};bool positive=false,negative=false;
    for(int i=0;i<4;++i){
        const auto a=vertices[i],b=vertices[(i+1)%4];
        if(SegmentDistance(p,a,b)<=tolerance)return true;
        const float cross=(b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);
        positive|=cross>.001f;negative|=cross<-.001f;
    }
    return !(positive&&negative);
}
inline bool HitAnnotation(const Annotation& a,Point p,float tolerance) {
    if(a.type==Tool::Select||a.readOnly)return false;
    const float pad=tolerance+a.style.lineWidth/2;
    if(IsLineTool(a.type)&&a.points.size()>=2){
        if(SegmentDistance(p,a.points[0],a.points[1])<=pad)return true;
        // Include arrowheads, not the entire (mostly empty) diagonal bounds.
        for(int end=0;end<2;++end){
            const int cap=a.type==Tool::Line?0:end?a.style.endEnding:a.style.startEnding;if(cap!=4&&cap!=5)continue;
            auto tip=a.points[end],other=a.points[1-end];const float length=PointDistance(tip,other);
            if(length<.01f)continue;
            const float dx=(other.x-tip.x)/length,dy=(other.y-tip.y)/length,r=std::max(1.0f,a.style.lineWidth);
            Point left{tip.x+8.8f*r*dx-4.5f*r*dy,tip.y+8.8f*r*dy+4.5f*r*dx};
            Point right{tip.x+8.8f*r*dx+4.5f*r*dy,tip.y+8.8f*r*dy-4.5f*r*dx};
            if(SegmentDistance(p,left,tip)<=pad||SegmentDistance(p,tip,right)<=pad)return true;
        }
        return false;
    }
    if(a.type==Tool::Ink&&!a.points.empty()){
        size_t offset=0;
        const auto counts=a.strokes.empty()?std::vector<int>{static_cast<int>(a.points.size())}:a.strokes;
        for(int count:counts){
            for(int i=0;i<count&&offset+static_cast<size_t>(i)<a.points.size();++i){
                const auto point=a.points[offset+i],before=i?a.points[offset+i-1]:point;
                if(SegmentDistance(p,before,point)<=pad)return true;
            }
            offset+=std::max(0,count);
        }
        return false;
    }
    if(IsMarkupTool(a.type)&&!a.quads.empty()){
        for(const auto& q:a.quads)if(InQuad(p,q,tolerance))return true;
        return false;
    }
    Rect hit=a.bounds;hit.x-=tolerance;hit.y-=tolerance;hit.w+=2*tolerance;hit.h+=2*tolerance;
    if(!hit.Contains(p))return false;
    if(a.type==Tool::Rectangle&&!a.style.filled){
        return std::min({std::abs(p.x-a.bounds.x),std::abs(p.x-a.bounds.x-a.bounds.w),
                         std::abs(p.y-a.bounds.y),std::abs(p.y-a.bounds.y-a.bounds.h)})<=pad;
    }
    if(a.type==Tool::Ellipse&&!a.style.filled){
        // 归一化半径上的距离近似为到椭圆边的距离。
        const float rx=std::max(.5f,a.bounds.w/2),ry=std::max(.5f,a.bounds.h/2);
        const float dx=p.x-(a.bounds.x+rx),dy=p.y-(a.bounds.y+ry);
        const float k=std::sqrt(dx*dx/(rx*rx)+dy*dy/(ry*ry));
        return std::abs(k-1)*std::min(rx,ry)<=pad;
    }
    if(a.type==Tool::Ellipse){
        const float rx=std::max(.5f,a.bounds.w/2+tolerance),ry=std::max(.5f,a.bounds.h/2+tolerance);
        const float dx=p.x-(a.bounds.x+a.bounds.w/2),dy=p.y-(a.bounds.y+a.bounds.h/2);
        return dx*dx/(rx*rx)+dy*dy/(ry*ry)<=1;
    }
    return true;
}
}