from pathlib import Path
import sys
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(r'C:\Users\admin\Desktop\Albion-Experimental-20261120\Source\WindowsProject1-Stable')
def edit(rel,old,new):
 p=root/rel;s=p.read_text(encoding='utf-8-sig')
 if new in s:return
 assert s.count(old)==1,(rel,s.count(old))
 p.write_bytes(s.replace(old,new).encode('utf-8'))
edit('file_catalog.hpp',
 '        { std::lock_guard lock(mutex_); desired_=std::move(directories); ++generation_; }',
 '        { std::lock_guard lock(mutex_); desired_=std::move(directories); ++generation_;\n          latest_.reset(); read_.reset(); readResult_.reset(); reading_=false; }')
edit('file_catalog.hpp',
 '''    void ReadProfile(std::wstring path,std::uint64_t request,std::uint64_t revision,
                     std::optional<FileVersion> expected=std::nullopt) {
        { std::lock_guard lock(mutex_);
          ProfileRead job;''',
 '''    bool ReadProfile(std::wstring path,std::uint64_t request,std::uint64_t revision,
                     std::optional<FileVersion> expected=std::nullopt,
                     std::optional<std::uint64_t> expectedGeneration=std::nullopt) {
        { std::lock_guard lock(mutex_);
          if(expectedGeneration && *expectedGeneration!=generation_) return false;
          ProfileRead job;''')
edit('file_catalog.hpp',
 '''        if(request_) SetEvent(request_);
    }
    std::optional<ProfileRead> TakeProfile() {
        std::lock_guard lock(mutex_); auto result=std::move(readResult_); readResult_.reset(); return result;
    }''',
 '''        if(request_) SetEvent(request_);
        return true;
    }
    std::optional<ProfileRead> TakeProfile() {
        std::lock_guard lock(mutex_); auto result=std::move(readResult_); readResult_.reset();
        if(result && result->generation!=generation_) return std::nullopt;
        return result;
    }''')
edit('modules/config/configuration.cpp',
 '        mFileCatalog.ReadProfile(path,++mProfileRequest,mSettingsRevision,file->version);',
 '''        if(!mFileCatalog.ReadProfile(path,++mProfileRequest,mSettingsRevision,file->version,mFileSnapshot->generation)) {
            SetConfigStatus(L"Discovery changed while selecting the profile. Refresh and Apply again.",true);return;
        }''')
edit('tests/file_catalog_windows.cpp',
 '''        catalog.Configure({{other.wstring(),FileKind::Model}});
        auto switched=Until''',
 '''        const auto prior=catalog.Latest();
        const auto* source=FileCatalog::Find(*prior,profile.wstring());
        Check(source!=nullptr,"profile source before reconfiguration");
        Check(catalog.ReadProfile(profile.wstring(),5,9,source->version,prior->generation),"current snapshot accepted");
        const auto completedEnd=GetTickCount64()+3000;
        while(catalog.Reading()&&GetTickCount64()<completedEnd) Sleep(10);
        Check(!catalog.Reading(),"profile read published before reconfiguration");
        catalog.Configure({{other.wstring(),FileKind::Model}});
        Check(!catalog.TakeProfile(),"reconfiguration discards completed old-generation read");
        Check(!catalog.ReadProfile(profile.wstring(),6,10,source->version,prior->generation),"stale selection cannot submit a new-generation read");
        auto switched=Until''')
print('GENERATION_PATCH_PASS')
