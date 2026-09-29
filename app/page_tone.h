#pragma once
// 页面配色：普通（白纸）、护眼（米黄纸）、夜间（深底浅字）。只改变显示，不修改文档。
#include <cstdint>
#include <vector>
namespace lpdf {
enum class PageTone { Normal, Sepia, Night };
// 夜间：亮度反转后压入 [kNightInk, kNightPaper] 区间，避免纯黑纯白的强对比。
inline constexpr int kNightPaper=0x26, kNightInk=0xd6;
inline constexpr uint32_t kSepiaPaper=0xf3ead6;
inline unsigned char ToneChannel(PageTone tone,unsigned char v,int channel){
    switch(tone){
    case PageTone::Sepia:{
        // 乘以纸张颜色：白→纸色，黑保持黑。channel 0=B 1=G 2=R。
        const int paper=static_cast<int>((kSepiaPaper>>(channel*8))&0xff);
        return static_cast<unsigned char>((v*paper+127)/255);
    }
    case PageTone::Night:
        return static_cast<unsigned char>(kNightPaper+((255-v)*(kNightInk-kNightPaper)+127)/255);
    default:return v;
    }
}
// 夜间只反转亮度、保留色相：红色批注仍是（变亮的）红色，而不是青色。
inline void NightPixel(unsigned char& b,unsigned char& g,unsigned char& r){
    const int lum=(r*299+g*587+b*114+500)/1000;
    const int target=kNightPaper+((255-lum)*(kNightInk-kNightPaper)+127)/255;
    const int delta=target-lum;
    auto clamp=[](int v){return static_cast<unsigned char>(v<0?0:v>255?255:v);};
    b=clamp(b+delta);g=clamp(g+delta);r=clamp(r+delta);
}
// 就地变换 BGRA 位图（不透明页面瓦片，alpha 不变）。
inline void ApplyTone(PageTone tone,std::vector<unsigned char>& bgra,int width,int height,int stride){
    if(tone==PageTone::Normal||width<=0||height<=0)return;
    unsigned char table[3][256];
    if(tone==PageTone::Sepia)for(int c=0;c<3;++c)for(int v=0;v<256;++v)table[c][v]=ToneChannel(tone,static_cast<unsigned char>(v),c);
    for(int y=0;y<height;++y){
        unsigned char* row=bgra.data()+static_cast<size_t>(y)*stride;
        for(int x=0;x<width;++x){unsigned char* p=row+x*4;
            if(tone==PageTone::Night)NightPixel(p[0],p[1],p[2]);
            else{p[0]=table[0][p[0]];p[1]=table[1][p[1]];p[2]=table[2][p[2]];}}
    }
}
inline uint32_t ToneColor(PageTone tone,uint32_t rgb){
    unsigned char b=rgb&0xff,g=(rgb>>8)&0xff,r=(rgb>>16)&0xff;
    if(tone==PageTone::Night)NightPixel(b,g,r);
    else{b=ToneChannel(tone,b,0);g=ToneChannel(tone,g,1);r=ToneChannel(tone,r,2);}
    return (static_cast<uint32_t>(r)<<16)|(static_cast<uint32_t>(g)<<8)|b;
}
}
