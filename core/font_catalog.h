#pragma once
#include "platform.h"
namespace lpdf {
struct SystemFontSource {
    fs::path path;
    unsigned faceIndex{};
    bool simulatedBold{}, simulatedItalic{};
};
// Installed fonts only. Reject unavailable or non-editably-embeddable faces.
SystemFontSource ResolveSystemFont(std::wstring_view family,bool bold=false,bool italic=false);
std::vector<std::wstring> InstalledFontFamilies();
}
