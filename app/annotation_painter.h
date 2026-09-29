#pragma once
#include "core/document.h"
#include <lumen/Painter.h>
#include <functional>
namespace lpdf {
void DrawAnnotationPreview(lumen::Painter&,const Annotation&,float scale,
                           const std::function<lumen::Point(Point)>& toScreen);
}
