#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "file_catalog.hpp"
#include <fstream>
#include <iostream>
#include <functional>
#include <chrono>

using namespace albion;
namespace fs=std::filesystem;
static void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static void Write(const fs::path& path,const std::string& text="fixture") {
    std::ofstream file(path,std::ios::binary); file<<text; Check(bool(file),"fixture write");
}
static std::shared_ptr<const CatalogSnapshot> Until(FileCatalog& catalog,
    const std::function<bool(const CatalogSnapshot&)>& predicate,DWORD ms=9000) {
    const auto end=GetTickCount64()+ms;
    while(GetTickCount64()<end) { auto s=catalog.Latest(); if(s&&predicate(*s)) return s; Sleep(10); }
    throw std::runtime_error("catalog condition timed out");
}
static bool Ready(const CatalogSnapshot& s,const fs::path& p) {
    const auto* f=FileCatalog::Find(s,p.wstring()); return f&&f->readiness==FileReadiness::Candidate;
}
int wmain() {
    const auto root=fs::temp_directory_path()/(L"Albion-catalog-test-"+std::to_wstring(GetCurrentProcessId()));
    FileCatalog catalog;
    try {
        fs::create_directories(root/L"configs");fs::create_directories(root/L"models");
        const auto cfg=root/L"configs",models=root/L"models";
        Check(catalog.Start({{cfg.wstring(),FileKind::Profile},{models.wstring(),FileKind::Model}}),"start worker");
        auto initial=Until(catalog,[](const auto& s){return s.scans>0;});
        Check(initial->files.empty(),"empty supported list");
        const auto model=models/L"Model Ω with spaces.ONNX",profile=cfg/L"профиль one.cfg";
        Write(model);Write(profile,"version=2\nmodel=Model Ω with spaces.ONNX\nu_comparisonMode=0\n");
        Write(models/L"ignore.onnx.tmp");Write(cfg/L"ignore.json");
        auto found=Until(catalog,[&](const auto&s){return Ready(s,model)&&Ready(s,profile);});
        Check(found->files.size()==2,"supported formats only; Unicode and spaces");
        auto original=*FileCatalog::Find(*found,model.wstring());
        const auto renamed=models/L"renamed.engine";
        fs::rename(model,renamed);
        auto renamedSnapshot=Until(catalog,[&](const auto&s){return !FileCatalog::Find(s,model.wstring())&&Ready(s,renamed);});
        Check(FileCatalog::Find(*renamedSnapshot,renamed.wstring())->version.identity==original.version.identity,"rename retains file identity");
        const auto old=*FileCatalog::Find(*renamedSnapshot,renamed.wstring());
        Write(models/L"atomic.tmp","replacement");fs::remove(renamed);fs::rename(models/L"atomic.tmp",renamed);
        auto replacement=Until(catalog,[&](const auto&s){const auto*f=FileCatalog::Find(s,renamed.wstring());return f&&f->version!=old.version&&Ready(s,renamed);});
        Check(FileCatalog::Find(*replacement,renamed.wstring())->changed!=old.changed,"same-name replacement revision");
        const auto gradual=models/L"gradual.plan";
        HANDLE writer=CreateFileW(gradual.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
        Check(writer!=INVALID_HANDLE_VALUE,"gradual writer");DWORD count=0;
        WriteFile(writer,"part",4,&count,nullptr);FlushFileBuffers(writer);
        auto copying=Until(catalog,[&](const auto&s){return FileCatalog::Find(s,gradual.wstring())!=nullptr;});
        Check(!Ready(*copying,gradual),"open writer is not ready");
        Sleep(800);WriteFile(writer,"two",3,&count,nullptr);FlushFileBuffers(writer);CloseHandle(writer);
        Until(catalog,[&](const auto&s){const auto*f=FileCatalog::Find(s,gradual.wstring());return f&&f->version.bytes==7&&Ready(s,gradual);});
        const auto beforeBurst=catalog.Latest()->scans;
        for(int i=0;i<80;++i) Write(models/(L"burst "+std::to_wstring(i)+L".trt"));
        std::vector<std::thread> requesters;
        for(int n=0;n<4;++n) requesters.emplace_back([&]{for(int i=0;i<200;++i)catalog.Request();});
        for(auto& thread:requesters) thread.join();
        auto burst=Until(catalog,[&](const auto&s){return s.files.size()==83&&Ready(s,models/L"burst 79.trt");});
        Check(burst->scans-beforeBurst<20,"coalesced concurrent refresh and burst");
        std::vector<std::wstring> order;for(const auto& f:burst->files)order.push_back(FileKey(f.path));
        Check(std::is_sorted(order.begin(),order.end()),"deterministic ordering");
        const auto stableScans=burst->scans;Sleep(1200);Check(catalog.Latest()->scans==stableScans,"no continuous idle scan");
        HANDLE locked=CreateFileW(profile.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
        Check(locked!=INVALID_HANDLE_VALUE,"exclusive profile lock");catalog.Request();
        Until(catalog,[&](const auto&s){const auto*f=FileCatalog::Find(s,profile.wstring());return f&&f->readiness==FileReadiness::Unavailable;});
        catalog.ReadProfile(profile.wstring(),1,5);
        std::optional<ProfileRead> read;const auto readEnd=GetTickCount64()+3000;
        while(!(read=catalog.TakeProfile())&&GetTickCount64()<readEnd)Sleep(10);
        Check(read&&!read->error.empty(),"locked read is reported, not applied");CloseHandle(locked);
        Write(profile,"invalid/incomplete profile");catalog.ReadProfile(profile.wstring(),2,6);read.reset();
        const auto invalidEnd=GetTickCount64()+3000;while(!(read=catalog.TakeProfile())&&GetTickCount64()<invalidEnd)Sleep(10);
        Check(read&&read->request==2&&!read->error.empty(),"parser validates on explicit read only");
        Write(profile,"version=2\nmodel=Model Ω with spaces.ONNX\nu_comparisonMode=0\n");
        auto valid=Until(catalog,[&](const auto&s){return Ready(s,profile);});
        const auto expected=FileCatalog::Find(*valid,profile.wstring())->version;
        Write(profile,"version=2\nmodel=Model Ω with spaces.ONNX\nu_comparisonMode=0\n\n");
        catalog.ReadProfile(profile.wstring(),3,7,expected);read.reset();
        const auto staleEnd=GetTickCount64()+3000;while(!(read=catalog.TakeProfile())&&GetTickCount64()<staleEnd)Sleep(10);
        Check(read&&!read->error.empty(),"changed source rejected against requested version");
        valid=Until(catalog,[&](const auto&s){return Ready(s,profile);});
        catalog.ReadProfile(profile.wstring(),4,8,FileCatalog::Find(*valid,profile.wstring())->version);read.reset();
        const auto parsedEnd=GetTickCount64()+3000;while(!(read=catalog.TakeProfile())&&GetTickCount64()<parsedEnd)Sleep(10);
        Check(read&&read->error.empty()&&read->document.values.at("model")=="Model Ω with spaces.ONNX","parsed document delivered from worker");
        fs::remove_all(models);
        Until(catalog,[](const auto&s){return std::any_of(s.directories.begin(),s.directories.end(),[](const auto&d){return d.kind==FileKind::Model&&d.error!=0;});});
        fs::create_directories(models);Write(models/L"recreated.onnx");
        Until(catalog,[&](const auto&s){return Ready(s,models/L"recreated.onnx");});
        const auto other=root/L"other";fs::create_directories(other);Write(other/L"new-root.onnx");
        catalog.Configure({{other.wstring(),FileKind::Model}});
        auto switched=Until(catalog,[&](const auto&s){return s.generation==2&&s.files.size()==1&&Ready(s,other/L"new-root.onnx");});
        Check(switched->directories.size()==1,"directory generation supersedes old scan");
        const auto autoScans=switched->scans;catalog.SetAutomatic(false);
        auto manual=Until(catalog,[&](const auto&s){return s.scans>autoScans&&!catalog.Busy();});
        const auto manualScans=manual->scans;Write(other/L"manual-only.plan");Sleep(1200);
        Check(catalog.Latest()->scans==manualScans&&!FileCatalog::Find(*catalog.Latest(),(other/L"manual-only.plan").wstring()),"automatic off performs no unsolicited scan");
        catalog.Request();Until(catalog,[&](const auto&s){return Ready(s,other/L"manual-only.plan");});
        catalog.SetAutomatic(true);const auto enabled=catalog.Latest()->scans;
        Until(catalog,[&](const auto&s){return s.scans>enabled&&!catalog.Busy();});
        Write(other/L"automatic-restored.trt");Until(catalog,[&](const auto&s){return Ready(s,other/L"automatic-restored.trt");});
        std::set<std::wstring> touched;bool uncertain=false;
        Check(FileCatalog::DecodeChanges(nullptr,0,{other.wstring(),FileKind::Model},touched,uncertain)&&uncertain,
              "notification overflow requests full reconciliation");
        alignas(DWORD) std::array<BYTE,64> malformed{};
        auto* notification=reinterpret_cast<FILE_NOTIFY_INFORMATION*>(malformed.data());notification->FileNameLength=1000;
        uncertain=false;Check(FileCatalog::DecodeChanges(malformed.data(),DWORD(malformed.size()),{other.wstring(),FileKind::Model},touched,uncertain)&&uncertain,
              "malformed notification requests reconciliation");
        catalog.Stop();DWORD handlesBefore=0,handlesAfter=0;GetProcessHandleCount(GetCurrentProcess(),&handlesBefore);
        for(int i=0;i<20;++i){FileCatalog shortLived;Check(shortLived.Start({{other.wstring(),FileKind::Model}}),"repeat start");shortLived.Request();shortLived.Stop();}
        GetProcessHandleCount(GetCurrentProcess(),&handlesAfter);Check(handlesAfter<=handlesBefore+1,"watch handles released after cancellation");
        fs::remove_all(root);
        std::cout<<"CATALOG_PASS adds renames removes replacements gradual Unicode bursts locks invalid directories generations concurrent_requests overflow cancellation idle_no_scans auto_toggle stale_profile parsed_document\n";
        std::cout<<"last_scan_ms="<<switched->scanMs<<" scans="<<switched->scans<<" notifications="<<switched->notifications<<" handles_before="<<handlesBefore<<" handles_after="<<handlesAfter<<'\n';
        return 0;
    } catch(const std::exception& e) { catalog.Stop();std::cerr<<"FAIL "<<e.what()<<'\n';std::error_code ec;fs::remove_all(root,ec);return 1; }
}
