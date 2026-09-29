#include "font_catalog.h"
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <tuple>

namespace lpdf {
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT result){if(FAILED(result))throw std::runtime_error("Windows font service could not read this font");}
ComPtr<IDWriteFontCollection> Collection(){
    ComPtr<IDWriteFactory> factory;
    Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
    ComPtr<IDWriteFontCollection> fonts;Check(factory->GetSystemFontCollection(&fonts,FALSE));return fonts;
}
}
SystemFontSource ResolveSystemFont(std::wstring_view family,bool bold,bool italic){
    if(family.empty()||family.size()>=LF_FACESIZE)throw std::runtime_error("Choose an installed font family");
    static std::mutex mutex;
    static std::map<std::tuple<std::wstring,bool,bool>,SystemFontSource> cache;
    std::lock_guard lock(mutex);
    auto key=std::tuple(std::wstring(family),bold,italic);
    if(auto it=cache.find(key);it!=cache.end())return it->second;
    auto fonts=Collection();UINT32 index=0;BOOL exists=FALSE;
    Check(fonts->FindFamilyName(std::get<0>(key).c_str(),&index,&exists));
    if(!exists)throw std::runtime_error("This font is not installed. Choose an available font.");
    ComPtr<IDWriteFontFamily> selected;Check(fonts->GetFontFamily(index,&selected));
    ComPtr<IDWriteFont> font;Check(selected->GetFirstMatchingFont(bold?DWRITE_FONT_WEIGHT_BOLD:DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,italic?DWRITE_FONT_STYLE_ITALIC:DWRITE_FONT_STYLE_NORMAL,&font));
    ComPtr<IDWriteFontFace> face;Check(font->CreateFontFace(&face));
    const void* table=nullptr;UINT32 size=0;void* context=nullptr;BOOL present=FALSE;
    Check(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('O','S','/','2'),&table,&size,&context,&present));
    unsigned rights=0;
    if(present&&size>=10){const auto* bytes=static_cast<const unsigned char*>(table);rights=(bytes[8]<<8)|bytes[9];}
    if(context)face->ReleaseFontTable(context);
    // Editable annotations are reflowed and subset on save. Do not silently
    // violate Restricted/Preview-only/No-subsetting/Bitmap-only embedding.
    if((rights&0x302)||((rights&4)&&!(rights&8)))
        throw std::runtime_error("This font does not permit editable subset embedding. Choose another font.");
    UINT32 count=0;Check(face->GetFiles(&count,nullptr));
    if(count!=1)throw std::runtime_error("This font is not a supported local OpenType font");
    ComPtr<IDWriteFontFile> file;Check(face->GetFiles(&count,file.GetAddressOf()));
    const void* reference=nullptr;UINT32 referenceSize=0;Check(file->GetReferenceKey(&reference,&referenceSize));
    ComPtr<IDWriteFontFileLoader> loader;Check(file->GetLoader(&loader));
    ComPtr<IDWriteLocalFontFileLoader> local;Check(loader.As(&local));
    UINT32 length=0;Check(local->GetFilePathLengthFromKey(reference,referenceSize,&length));
    std::wstring path(length+1,L'\0');Check(local->GetFilePathFromKey(reference,referenceSize,path.data(),length+1));path.resize(length);
    const auto simulations=face->GetSimulations();
    SystemFontSource result{fs::path(path),face->GetIndex(),
        (simulations&DWRITE_FONT_SIMULATIONS_BOLD)!=0||(bold&&font->GetWeight()<DWRITE_FONT_WEIGHT_SEMI_BOLD),
        (simulations&DWRITE_FONT_SIMULATIONS_OBLIQUE)!=0||(italic&&font->GetStyle()==DWRITE_FONT_STYLE_NORMAL)};
    cache.emplace(std::move(key),result);return result;
}
std::vector<std::wstring> InstalledFontFamilies(){
    std::vector<std::wstring> result;auto fonts=Collection();
    for(UINT32 i=0;i<fonts->GetFontFamilyCount();++i){
        ComPtr<IDWriteFontFamily> family;Check(fonts->GetFontFamily(i,&family));
        ComPtr<IDWriteLocalizedStrings> names;Check(family->GetFamilyNames(&names));
        UINT32 index=0;BOOL exists=FALSE;names->FindLocaleName(L"en-us",&index,&exists);if(!exists)index=0;
        UINT32 length=0;Check(names->GetStringLength(index,&length));
        if(length==0||length>=LF_FACESIZE)continue;
        std::wstring name(length+1,L'\0');Check(names->GetString(index,name.data(),length+1));name.resize(length);
        if(name.front()!=L'@')result.push_back(std::move(name));
    }
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
}
