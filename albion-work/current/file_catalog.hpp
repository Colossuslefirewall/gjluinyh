#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "config_document.hpp"

namespace albion {
enum class FileKind { Profile, Model };
enum class FileReadiness { Settling, Candidate, Unavailable };
inline std::wstring FileKey(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (!path.empty()) CharLowerBuffW(path.data(), DWORD(path.size()));
    return path;
}
struct FileVersion {
    std::uint64_t bytes=0, written=0, created=0, identity=0;
    bool operator==(const FileVersion&) const = default;
};
struct CatalogFile {
    std::wstring path, name;
    FileKind kind=FileKind::Model;
    FileVersion version;
    FileReadiness readiness=FileReadiness::Settling;
    DWORD error=0;
    std::uint64_t changed=0, quietSince=0;
    bool operator==(const CatalogFile&) const = default;
};
struct CatalogDirectory {
    std::wstring path;
    FileKind kind=FileKind::Model;
    bool createInitially=false;
    DWORD error=0, watcherError=0;
    bool operator==(const CatalogDirectory&) const = default;
};
struct CatalogSnapshot {
    std::uint64_t generation=0, sequence=0, scans=0, notifications=0, overflows=0;
    double scanMs=0;
    bool truncated=false;
    std::vector<CatalogDirectory> directories;
    std::vector<CatalogFile> files;
};
struct ProfileRead {
    std::wstring path;
    std::uint64_t request=0, settingsRevision=0;
    FileVersion version;
    std::string text, error;
};

// One owner for directory I/O. UI reads immutable snapshots; no HWND/callbacks cross threads.
class FileCatalog {
public:
    static constexpr DWORD debounceMs=250, settleMs=750, reconcileMs=30000, retryMs=5000;
    static constexpr size_t maxFiles=4096;
    ~FileCatalog() { Stop(); }
    FileCatalog()=default;
    FileCatalog(const FileCatalog&)=delete;
    FileCatalog& operator=(const FileCatalog&)=delete;
    bool Start(std::vector<CatalogDirectory> directories) {
        if (worker_.joinable()) { Configure(std::move(directories)); return true; }
        stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        request_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!stop_ || !request_) { CloseEvents(); return false; }
        stopping_=false;
        Configure(std::move(directories));
        try { worker_=std::thread([this]{ Run(); }); }
        catch (...) { CloseEvents(); return false; }
        return true;
    }
    void Configure(std::vector<CatalogDirectory> directories) {
        { std::lock_guard lock(mutex_); desired_=std::move(directories); ++generation_; }
        Request();
    }
    void Request() {
        busy_=true; ++requested_;
        if(request_) SetEvent(request_);
    }
    bool Busy() const { return busy_.load(); }
    bool Reading() const { return reading_.load(); }
    std::shared_ptr<const CatalogSnapshot> Latest() {
        std::lock_guard lock(mutex_); return latest_;
    }
    void ReadProfile(std::wstring path,std::uint64_t request,std::uint64_t revision) {
        { std::lock_guard lock(mutex_); read_=ProfileRead{std::move(path),request,revision,{},{},{}}; readResult_.reset(); }
        reading_=true; if(request_) SetEvent(request_);
    }
    std::optional<ProfileRead> TakeProfile() {
        std::lock_guard lock(mutex_); auto result=std::move(readResult_); readResult_.reset(); return result;
    }
    void Stop() {
        stopping_=true;
        if(stop_) SetEvent(stop_);
        if(worker_.joinable()) {
            CancelSynchronousIo(worker_.native_handle());
            worker_.join();
        }
        CloseEvents(); busy_=false; reading_=false;
    }
    static bool Supported(const std::wstring& name,FileKind kind) {
        auto dot=name.find_last_of(L'.');
        const auto ext=dot==std::wstring::npos?L"":FileKey(name.substr(dot));
        return kind==FileKind::Profile?ext==L".cfg":
            (ext==L".onnx"||ext==L".engine"||ext==L".plan"||ext==L".trt");
    }
    static const CatalogFile* Find(const CatalogSnapshot& snapshot,const std::wstring& path) {
        const auto key=FileKey(path);
        for(const auto& file:snapshot.files) if(FileKey(file.path)==key) return &file;
        return nullptr;
    }
    static FileVersion Version(const BY_HANDLE_FILE_INFORMATION& info) {
        return {(std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow,
            (std::uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime,
            (std::uint64_t(info.ftCreationTime.dwHighDateTime)<<32)|info.ftCreationTime.dwLowDateTime,
            ((std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow)^info.dwVolumeSerialNumber};
    }
private:
    struct Watch {
        HANDLE directory=INVALID_HANDLE_VALUE,event=nullptr;
        OVERLAPPED overlap{};
        alignas(DWORD) std::array<BYTE,16384> buffer{};
        bool pending=false;
        CatalogDirectory spec;
        ~Watch() { Close(); }
        void Close() {
            if(directory!=INVALID_HANDLE_VALUE) {
                if(pending) {
                    CancelIoEx(directory,&overlap);
                    DWORD ignored=0; GetOverlappedResult(directory,&overlap,&ignored,TRUE);
                    pending=false;
                }
                CloseHandle(directory); directory=INVALID_HANDLE_VALUE;
            }
            if(event) { CloseHandle(event); event=nullptr; }
        }
        bool Arm() {
            ResetEvent(event);
            overlap={}; overlap.hEvent=event;
            pending=ReadDirectoryChangesW(directory,buffer.data(),DWORD(buffer.size()),FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|
                FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_ATTRIBUTES|FILE_NOTIFY_CHANGE_SECURITY,
                nullptr,&overlap,nullptr)!=FALSE;
            if(!pending) spec.watcherError=GetLastError();
            return pending;
        }
        void Open() {
            if(directory!=INVALID_HANDLE_VALUE) return;
            directory=CreateFileW(spec.path.c_str(),FILE_LIST_DIRECTORY,
                FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr);
            if(directory==INVALID_HANDLE_VALUE) { spec.watcherError=GetLastError(); return; }
            event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            if(!event) { spec.watcherError=GetLastError(); Close(); return; }
            spec.watcherError=0;
            if(!Arm()) Close();
        }
    };
    HANDLE stop_=nullptr,request_=nullptr;
    std::thread worker_;
    std::atomic<bool> stopping_{false},busy_{false},reading_{false};
    std::atomic<std::uint64_t> requested_{0};
    std::mutex mutex_;
    std::uint64_t generation_=0;
    std::vector<CatalogDirectory> desired_;
    std::shared_ptr<const CatalogSnapshot> latest_;
    std::optional<ProfileRead> read_,readResult_;
    void CloseEvents() {
        if(stop_) CloseHandle(stop_);
        if(request_) CloseHandle(request_);
        stop_=request_=nullptr;
    }
    void ReadOne() {
        std::optional<ProfileRead> job;
        { std::lock_guard lock(mutex_); job=std::move(read_); read_.reset(); }
        if(!job) return;
        HANDLE file=CreateFileW(job->path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(file==INVALID_HANDLE_VALUE) job->error="Cannot read profile (missing, locked or inaccessible), Windows error "+std::to_string(GetLastError());
        else {
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(file,&info)) job->error="Cannot inspect profile";
            else {
                job->version=Version(info);
                if(job->version.bytes>1024*1024) job->error="Profile exceeds 1 MiB safety limit";
                else {
                    job->text.resize(size_t(job->version.bytes)); DWORD got=0;
                    if(!ReadFile(file,job->text.data(),DWORD(job->text.size()),&got,nullptr)||got!=job->text.size())
                        job->error="Incomplete profile read";
                    else { ConfigDocument document; if(!document.Parse(job->text)) job->error=document.error; }
                }
            }
            CloseHandle(file);
        }
        { std::lock_guard lock(mutex_); if(!read_) { readResult_=std::move(job); reading_=false; } }
    }
    void Scan(CatalogSnapshot& next,const CatalogSnapshot& previous,
              const std::set<std::wstring>& touched,bool uncertain) {
        const auto now=GetTickCount64();
        for(auto& root:next.directories) {
            root.error=0;
            WIN32_FIND_DATAW data{};
            const auto pattern=(std::filesystem::path(root.path)/L"*").wstring();
            HANDLE find=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&data,FindExSearchNameMatch,nullptr,0);
            if(find==INVALID_HANDLE_VALUE) {
                root.error=GetLastError();
                if(root.error==ERROR_FILE_NOT_FOUND) root.error=0;
                continue;
            }
            do {
                if(stopping_) break;
                if((data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||!Supported(data.cFileName,root.kind)) continue;
                if(next.files.size()>=maxFiles) { next.truncated=true; break; }
                CatalogFile item;
                item.path=(std::filesystem::path(root.path)/data.cFileName).wstring();
                item.name=data.cFileName; item.kind=root.kind;
                item.version={ (std::uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow,
                    (std::uint64_t(data.ftLastWriteTime.dwHighDateTime)<<32)|data.ftLastWriteTime.dwLowDateTime,
                    (std::uint64_t(data.ftCreationTime.dwHighDateTime)<<32)|data.ftCreationTime.dwLowDateTime,0 };
                // Metadata/read-sharing probe only: no model reads, sessions or validation here.
                HANDLE file=CreateFileW(item.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
                    nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(file==INVALID_HANDLE_VALUE) item.error=GetLastError();
                else {
                    BY_HANDLE_FILE_INFORMATION info{};
                    if(GetFileInformationByHandle(file,&info)) item.version=Version(info);
                    else item.error=GetLastError();
                    CloseHandle(file);
                }
                const auto* old=Find(previous,item.path);
                const bool changed=!old || old->version!=item.version || old->error!=item.error ||
                    uncertain || touched.contains(FileKey(item.path));
                item.changed=changed?next.sequence:old->changed;
                item.quietSince=changed?now:old->quietSince;
                item.readiness=item.error?FileReadiness::Unavailable:
                    (now-item.quietSince<settleMs?FileReadiness::Settling:FileReadiness::Candidate);
                next.files.push_back(std::move(item));
            } while(FindNextFileW(find,&data));
            const auto error=GetLastError(); FindClose(find);
            if(!stopping_&&!next.truncated&&error!=ERROR_NO_MORE_FILES) root.error=error;
        }
        std::sort(next.files.begin(),next.files.end(),[](const auto& a,const auto& b){
            const auto ka=FileKey(a.path),kb=FileKey(b.path);
            return ka==kb?a.path<b.path:ka<kb;
        });
        next.files.erase(std::unique(next.files.begin(),next.files.end(),[](const auto&a,const auto&b){
            return a.kind==b.kind&&FileKey(a.path)==FileKey(b.path);
        }),next.files.end());
    }
    void Run() noexcept {
        try { Work(); }
        catch(...) {
            busy_=false; reading_=false;
            auto failure=std::make_shared<CatalogSnapshot>();
            std::lock_guard lock(mutex_); failure->generation=generation_; failure->sequence=latest_?latest_->sequence+1:1;
            failure->directories=desired_;
            for(auto& d:failure->directories) d.error=ERROR_GEN_FAILURE;
            latest_=std::move(failure);
        }
    }
    void Work() {
        std::vector<std::unique_ptr<Watch>> watches;
        CatalogSnapshot previous;
        std::uint64_t generation=0,sequence=0,scans=0,notifications=0,overflows=0,seenRequest=0;
        ULONGLONG deadline=0,firstEvent=0,nextReconcile=0;
        std::set<std::wstring> touched;
        bool uncertain=false;
        while(!stopping_) {
            { std::lock_guard lock(mutex_);
              if(generation!=generation_) {
                // Close/cancellation is done outside this short publication lock below.
                generation=generation_;
              }
            }
            if(previous.generation!=generation) {
                watches.clear(); previous={}; previous.generation=generation;
                std::vector<CatalogDirectory> specs;
                { std::lock_guard lock(mutex_); specs=desired_; }
                for(auto spec:specs) {
                    if(spec.createInitially) { std::error_code ec; std::filesystem::create_directories(spec.path,ec); }
                    auto watch=std::make_unique<Watch>(); watch->spec=std::move(spec); watch->Open(); watches.push_back(std::move(watch));
                }
                deadline=0; nextReconcile=0; touched.clear(); uncertain=false;
            }
            ReadOne();
            const auto now=GetTickCount64();
            if(requested_.load()!=seenRequest) { seenRequest=requested_.load(); deadline=now; busy_=true; }
            if(now>=nextReconcile) { deadline=now; for(auto& w:watches) w->Open(); }
            if(now>=deadline) {
                busy_=true;
                CatalogSnapshot next; next.generation=generation; next.sequence=++sequence;
                next.scans=++scans; next.notifications=notifications; next.overflows=overflows;
                for(const auto& w:watches) next.directories.push_back(w->spec);
                LARGE_INTEGER start{},finish{},freq{}; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
                Scan(next,previous,touched,uncertain);
                QueryPerformanceCounter(&finish); next.scanMs=double(finish.QuadPart-start.QuadPart)*1000.0/double(freq.QuadPart);
                touched.clear(); uncertain=false; firstEvent=0;
                if(stopping_) break;
                bool retry=false,settling=false;
                for(const auto& f:next.files) { settling|=f.readiness==FileReadiness::Settling; retry|=f.error!=0; }
                for(size_t i=0;i<next.directories.size();++i) {
                    const auto& d=next.directories[i]; retry|=d.error!=0||d.watcherError!=0;
                    if(d.error) watches[i]->Close();
                }
                nextReconcile=GetTickCount64()+(retry?retryMs:reconcileMs);
                deadline=settling?GetTickCount64()+settleMs:nextReconcile;
                { std::lock_guard lock(mutex_);
                  if(generation==generation_&&seenRequest==requested_.load()) {
                    previous=next; latest_=std::make_shared<CatalogSnapshot>(std::move(next)); busy_=false;
                  } else deadline=GetTickCount64();
                }
            }
            std::vector<HANDLE> handles{stop_,request_};
            std::vector<Watch*> active;
            for(auto& w:watches) if(w->pending) { handles.push_back(w->event); active.push_back(w.get()); }
            const auto tick=GetTickCount64();
            DWORD timeout=DWORD((std::min<ULONGLONG>)(deadline>tick?deadline-tick:0,reconcileMs));
            const DWORD result=WaitForMultipleObjects(DWORD(handles.size()),handles.data(),FALSE,timeout);
            if(result==WAIT_OBJECT_0) break;
            if(result>=WAIT_OBJECT_0+2 && result<WAIT_OBJECT_0+handles.size()) {
                auto& w=*active[size_t(result-WAIT_OBJECT_0-2)];
                DWORD bytes=0; const bool ok=GetOverlappedResult(w.directory,&w.overlap,&bytes,FALSE)!=FALSE;
                const DWORD error=ok?0:GetLastError(); w.pending=false; ++notifications;
                bool relevant=!ok||bytes==0;
                if(!ok||bytes==0) { uncertain=true; ++overflows; }
                if(ok&&bytes) {
                    size_t offset=0;
                    while(offset+sizeof(FILE_NOTIFY_INFORMATION)<=bytes) {
                        const auto* item=reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(w.buffer.data()+offset);
                        if(offset+offsetof(FILE_NOTIFY_INFORMATION,FileName)+item->FileNameLength>bytes) { relevant=true; uncertain=true; break; }
                        const std::wstring name(item->FileName,item->FileNameLength/sizeof(wchar_t));
                        if(Supported(name,w.spec.kind)) {
                            relevant=true;
                            if(touched.size()<maxFiles) touched.insert(FileKey((std::filesystem::path(w.spec.path)/name).wstring()));
                            else uncertain=true;
                        }
                        if(!item->NextEntryOffset) break;
                        offset+=item->NextEntryOffset;
                    }
                }
                if(error&&error!=ERROR_NOTIFY_ENUM_DIR) { w.spec.watcherError=error; w.Close(); }
                else if(!w.Arm()) w.Close();
                if(relevant) {
                    const auto eventTime=GetTickCount64(); if(!firstEvent) firstEvent=eventTime;
                    deadline=(std::min)(eventTime+debounceMs,firstEvent+1500); busy_=true;
                }
            } else if(result==WAIT_FAILED) throw std::runtime_error("Directory wait failed");
        }
    }
};
}
