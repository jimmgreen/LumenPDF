#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
namespace lpdf {
enum class ProgressStage {
    Reading, TextLayout, ImageDecode, OfficeConversion, ConversionReady,
    Inspecting, Flattening, MergingPages, Bookmarks,
    Writing, Validating, Optimizing, Publishing, Complete
};
inline constexpr size_t NoProgressFile=std::numeric_limits<size_t>::max();
// Reports completed work, never an estimate of elapsed duration. total == 0
// means the backend cannot yet provide a measurable denominator.
struct OperationProgress {
    ProgressStage stage{ProgressStage::Reading};
    size_t fileIndex{NoProgressFile},fileCount{};
    uint64_t completed{},total{},fileCompleted{},fileTotal{};
    std::wstring backend;
};
using ProgressSink=std::function<void(const OperationProgress&)>;
inline void ReportProgress(const ProgressSink& sink,ProgressStage stage,uint64_t done=0,uint64_t total=0){
    if(sink){OperationProgress event;event.stage=stage;event.completed=done;event.total=total;sink(event);}
}
}
