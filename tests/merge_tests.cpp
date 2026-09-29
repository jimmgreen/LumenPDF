#include "core/conversion.h"
#include "app/merge_progress.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <windows.h>
using namespace lpdf;
namespace {
int checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
bool Has(const std::vector<OperationProgress>& events,ProgressStage stage){return std::any_of(events.begin(),events.end(),[&](const auto& e){return e.stage==stage;});}
void State(){
    MergeProgressState state(3);OperationProgress event;event.stage=ProgressStage::OfficeConversion;event.backend=L"Microsoft Word";
    state.Conversion(1,event);auto s=state.Read();Check(s.phase==1&&s.current.total==0&&s.current.fileIndex==1,"unknown Office duration became fake percentage");
    Check(s.rows[1].state==MergeRowState::Converting&&s.rows[1].backend==L"Microsoft Word","conversion row state lost");
    state.Converted(1,L"Microsoft Word");Check(state.Read().rows[1].state==MergeRowState::Prepared,"converted file marked completed before merge");
    event.stage=ProgressStage::MergingPages;event.fileIndex=0;event.fileCount=3;event.completed=2;event.total=8;event.fileCompleted=2;event.fileTotal=2;
    state.Merge(event);s=state.Read();Check(s.phase==2&&s.rows[0].state==MergeRowState::Merged&&s.current.completed==2,"page progress mismatch");
    event.fileIndex=1;event.fileCompleted=1;event.fileTotal=4;event.completed=3;state.Merge(event);
    state.Finish(MergeOutcome::Cancelled,L"cancelled");s=state.Read();
    Check(s.rows[0].state==MergeRowState::Merged&&s.rows[1].state==MergeRowState::Cancelled&&s.rows[2].state==MergeRowState::NotProcessed,"cancellation corrupted completed/pending row states");
    auto before=s.revision;state.Merge(event);Check(state.Read().revision==before,"late progress changed a terminal outcome");
    MergeProgressState saving(1);event.stage=ProgressStage::Publishing;event.fileIndex=NoProgressFile;saving.Merge(event);
    Check(saving.Read().outcome==MergeOutcome::Running&&saving.Read().phase==3,"publishing reported success before atomic replacement");
    saving.Finish(MergeOutcome::Complete);Check(saving.Read().outcome==MergeOutcome::Complete,"completion was not recorded");
    MergeProgressState concurrent(1);std::atomic_bool done=false;
    std::jthread writer([&]{for(int i=0;i<2000;++i){OperationProgress e;e.stage=ProgressStage::MergingPages;e.fileIndex=0;e.fileCount=1;e.completed=i;e.total=2000;e.fileCompleted=i;e.fileTotal=2000;concurrent.Merge(e);}done.store(true);});
    uint64_t revision=0;bool consistent=true;while(!done.load()){auto read=concurrent.Read();consistent=consistent&&read.rows.size()==1&&read.revision>=revision;revision=read.revision;}
    Check(consistent,"progress snapshot was torn");
}
void VerifyMerged(const fs::path& file){
    Document doc;doc.Open(file);Check(doc.Info().pages.size()==4,"mixed merge page count or selected range mismatch");
    Check(doc.Info().outline.size()==3,"merge bookmarks missing");
    Check(doc.Text(0).find(L"Text input")!=std::wstring::npos,"TXT conversion not first after merge");
}
void Run(const fs::path& out,bool office){
    fs::create_directories(out);
    Check(FileKind(L"A.PDF")==InputFileKind::Pdf,"PDF extension classification");
    Check(FileKind(L"A.DoCx")==InputFileKind::Word&&FileKind(L"A.DOC")==InputFileKind::Word,"Word extension classification");
    Check(FileKind(L"A.TXT")==InputFileKind::Text,"TXT extension classification");
    Check(FileKind(L"A.JPEG")==InputFileKind::Image&&FileKind(L"A.png")==InputFileKind::Image,"image extension classification");
    Check(FileKind(L"A.xyz")==InputFileKind::Other,"unknown extension not distinguished");
    const auto text=out/L"notes.TXT";const auto utf=Utf8(L"Text input\n中文转换与合并进度。");WriteBytes(text,{utf.begin(),utf.end()});const auto originalText=ReadBytes(text);
    Document source;source.New();source.InsertBlank(0);source.InsertBlank(1);source.Save(out/L"three-pages.pdf");
    const auto originalPdf=ReadBytes(out/L"three-pages.pdf");
    const auto fixtures=fs::path(__FILE__).parent_path()/L"fixtures";
    Converter converter;std::vector<OperationProgress> textEvents;
    const auto textPdf=converter.Convert(text,{}, {},[&](const auto& event){textEvents.push_back(event);});
    Check(!textEvents.empty()&&textEvents.front().stage==ProgressStage::Reading&&textEvents.back().stage==ProgressStage::ConversionReady,"text conversion event boundaries");
    Check(Has(textEvents,ProgressStage::TextLayout)&&Has(textEvents,ProgressStage::Writing)&&Has(textEvents,ProgressStage::Validating)&&Has(textEvents,ProgressStage::Publishing),"conversion skipped real layout/write/verify stages");
    Document convertedText;convertedText.Open(textPdf.pdf);Check(convertedText.Info().pages.size()==1,"converted text output invalid");
    std::vector<OperationProgress> imageEvents;
    auto imagePdf=converter.Convert(fixtures/L"transparent.png",{}, {},[&](const auto& e){imageEvents.push_back(e);});
    Check(Has(imageEvents,ProgressStage::ImageDecode)&&imageEvents.back().stage==ProgressStage::ConversionReady,"image conversion stages missing");
    std::vector<OperationProgress> events;std::vector<int> legacy;
    const auto merged=out/L"mixed-progress.pdf";
    std::vector<MergeInput> inputs{{textPdf.pdf,L"",L""},{out/L"three-pages.pdf",L"3,1",L""},{imagePdf.pdf,L"",L""}};
    Document::Merge(inputs,merged,true,{},[&](int n,int total){Check(total==3,"legacy total changed");legacy.push_back(n);},[&](const auto& e){
        events.push_back(e);
        if(e.stage==ProgressStage::Complete){Document proof;proof.Open(merged);Check(proof.Info().pages.size()==4,"completion preceded durable output");}
    });
    Check(legacy==std::vector<int>({1,2,3}),"legacy per-file callback contract changed");
    Check(Has(events,ProgressStage::Inspecting)&&Has(events,ProgressStage::Flattening)&&Has(events,ProgressStage::MergingPages)&&Has(events,ProgressStage::Bookmarks),"merge phases missing");
    Check(events.back().stage==ProgressStage::Complete&&events.back().completed==4&&events.back().total==4,"final page progress wrong");
    uint64_t previous=0;bool allPages=false,written=false;
    for(const auto& e:events){
        Check(e.fileCount==3,"progress lost file count");
        if(e.stage==ProgressStage::MergingPages){Check(e.total==4&&e.completed>=previous&&e.completed<=4,"page progress is not real and monotonic");previous=e.completed;allPages|=previous==4;Check(e.fileCompleted<=e.fileTotal,"per-file page counter overflow");}
        if(e.stage==ProgressStage::Writing){Check(allPages,"write started before all pages merged");written=true;}
        if(e.stage==ProgressStage::Complete)Check(written,"completion skipped file writing");
    }
    VerifyMerged(merged);
    const auto sentinel=ReadBytes(merged);auto cancel=std::make_shared<std::atomic_bool>(false);bool stopped=false;bool complete=false;
    try{Document::Merge(inputs,merged,false,cancel,{},[&](const auto& e){if(e.stage==ProgressStage::MergingPages&&e.completed>=1)cancel->store(true);complete|=e.stage==ProgressStage::Complete;});}catch(const Cancelled&){stopped=true;}
    Check(stopped&&!complete&&ReadBytes(merged)==sentinel,"cancelled page merge changed target or reported success");
    cancel->store(false);stopped=false;
    try{Document::Merge(inputs,merged,false,cancel,{},[&](const auto& e){if(e.stage==ProgressStage::Publishing)cancel->store(true);});}catch(const Cancelled&){stopped=true;}
    Check(stopped&&ReadBytes(merged)==sentinel,"cancellation at atomic publication changed target");
    bool failed=false;size_t failing=NoProgressFile;complete=false;
    try{Document::Merge({{textPdf.pdf,L"",L""},{out/L"three-pages.pdf",L"99",L""}},merged,false,{}, {},[&](const auto& e){failing=e.fileIndex;complete|=e.stage==ProgressStage::Complete;});}catch(const std::exception&){failed=true;}
    Check(failed&&failing==1&&!complete&&ReadBytes(merged)==sentinel,"invalid range did not identify file or preserve target");
    bool password=false;
    try{Document::Merge({{fixtures/L"password.pdf",L"",L"wrong"}},merged,false,{}, {},[](const auto&){});}catch(const PasswordRequired&){password=true;}
    Check(password&&ReadBytes(merged)==sentinel,"password failure damaged output");
    cancel->store(false);stopped=false;
    try{converter.Convert(text,{},cancel,[&](const auto& e){if(e.stage==ProgressStage::TextLayout)cancel->store(true);});}catch(const Cancelled&){stopped=true;}
    Check(stopped&&ReadBytes(text)==originalText,"cancelled text conversion changed its input");
    failed=false;try{converter.Convert(out/L"missing.xyz",{}, {},[](const auto&){});}catch(const std::exception&){failed=true;}
    Check(failed,"unsupported type did not fail");
    Check(ReadBytes(text)==originalText&&ReadBytes(out/L"three-pages.pdf")==originalPdf,"merge changed source files");
    // UI fixtures include a long filename and enough real text to observe layout.
    fs::copy_file(fixtures/L"transparent.png",out/L"site-photo.png",fs::copy_options::overwrite_existing);
    fs::copy_file(fixtures/L"word-sample.docx",out/L"work-report.docx",fs::copy_options::overwrite_existing);
    std::wstring large;for(int i=0;i<12000;++i)large+=L"Progress fixture — 中文分页验证，保留原始文件。\n";
    auto bytes=Utf8(large);WriteBytes(out/L"long-conversion.txt",{bytes.begin(),bytes.end()});
    WriteBytes(out/L"invalid-encoding.txt",{0xff,0x81,0x00,0x80,0x7f});
    if(office){
        std::vector<OperationProgress> officeEvents;
        auto word=converter.Convert(fixtures/L"word-sample.docx",{}, {},[&](const auto& e){officeEvents.push_back(e);});
        auto start=std::find_if(officeEvents.begin(),officeEvents.end(),[](const auto& e){return e.stage==ProgressStage::OfficeConversion;});
        Check(start!=officeEvents.end()&&start->total==0&&!start->backend.empty(),"Office conversion reported a fabricated denominator");
        Document proof;proof.Open(word.pdf);Check(proof.Text(0).find(L"Word conversion")!=std::wstring::npos,"real Office conversion output invalid");
        Check(officeEvents.back().stage==ProgressStage::ConversionReady,"Office conversion did not validate completion");
        proof.Save(out/L"word-progress.pdf");
    }
}
}
// 基准：merge_tests --bench-merge 输出.pdf 输入1.pdf 输入2.pdf …，逐阶段打印耗时（秒）。
int BenchMerge(int argc,wchar_t** argv){
    std::vector<MergeInput> inputs;for(int i=3;i<argc;++i)inputs.push_back({argv[i],L"",L""});
    const auto start=std::chrono::steady_clock::now();auto last=start;
    auto secs=[](auto a,auto b){return std::chrono::duration<double>(b-a).count();};
    Document::Merge(inputs,argv[2],false,{},{},[&](const OperationProgress& e){
        if(e.stage==ProgressStage::MergingPages)return;
        const auto now=std::chrono::steady_clock::now();
        std::wcout<<L"stage "<<static_cast<int>(e.stage)<<L" at "<<secs(start,now)<<L"s (+"<<secs(last,now)<<L")\n";last=now;
    });
    std::wcout<<L"total "<<secs(start,std::chrono::steady_clock::now())<<L"s size "<<fs::file_size(argv[2])<<L"\n";
    return 0;
}
int wmain(int argc,wchar_t** argv){
    if(argc>3&&std::wstring_view(argv[1])==L"--bench-merge")return BenchMerge(argc,argv);
    if(argc==4&&std::wstring_view(argv[1])==L"--word-worker")return WordWorker(argv[2],argv[3]);
    try{
        if(argc>2&&std::wstring_view(argv[1])==L"--verify-merged")VerifyMerged(argv[2]);
        else{State();Run(argc>1?fs::path(argv[1]):fs::path(L"merge-output"),argc>2&&std::wstring_view(argv[2])==L"--office");}
        std::cout<<"PASS merge progress: "<<checks<<" assertions\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL merge progress: "<<e.what()<<" after "<<checks<<" assertions\n";return 1;}
}