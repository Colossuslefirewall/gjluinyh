from pathlib import Path
R=Path('albion-work/current')
import shutil
for f in ['modules/app/app.hpp','modules/app/lifecycle.cpp','modules/config/configuration.cpp','modules/ui/controls.cpp','modules/ui/drawing.cpp','modules/ui/window.cpp','modules/inference/detector.hpp','modules/inference/detector.cpp','modules/pipeline/pipeline.cpp']:
 shutil.copy2(Path('albion-work/pre-followup')/f,R/f)
def edit(path,old,new,count=1):
 p=R/path;s=p.read_text();assert s.count(old)>=count,(path,old[:70]);p.write_text(s.replace(old,new,count))
edit('modules/app/app.hpp','#include "modules/core/platform.hpp"','#include "modules/core/platform.hpp"\n#include "file_catalog.hpp"')
edit('modules/app/app.hpp','struct ConfigEntry { std::wstring name, detail; };','struct ConfigEntry { std::wstring name, detail; bool operator==(const ConfigEntry&) const = default; };')
edit('modules/app/app.hpp','    HWND mConfigStatusLabel = nullptr;','''    HWND mConfigStatusLabel = nullptr;
    HWND mModelRefreshBtn=nullptr,mModelReloadBtn=nullptr,mModelFoldersBtn=nullptr,mModelDiscoveryLabel=nullptr;
    HWND mConfigRefreshBtn=nullptr,mConfigFolderBtn=nullptr,mConfigDiscoveryLabel=nullptr;
    static constexpr int kModelRefresh=1120,kModelReload=1121,kModelFolders=1122,kConfigRefresh=1123,kConfigFolder=1124;
    static constexpr UINT_PTR kFileCatalogTimer=12;
    albion::FileCatalog mFileCatalog;
    std::shared_ptr<const albion::CatalogSnapshot> mFileSnapshot;
    std::uint64_t mModelCatalogSequence=0,mConfigCatalogSequence=0,mProfileRequest=0;
    std::wstring mSelectedModelPath,mRememberedConfigName,mPreferredConfigName;
    std::wstring mStartupModelPath,mModelDiscoveryText,mConfigDiscoveryText;
    bool mDiscoveryStarted=false,mStartupModelAllowed=false,mStartupChoiceKnown=false;
    std::optional<albion::FileVersion> mLoadedProfileVersion;
    std::uint64_t mLoadedProfileChange=0,mModelRequestedChange=0;
    std::wstring mModelRequestedPath;
    void EnsureFileDiscovery();
    void PollFileDiscovery();
    void UpdateFileDiscoveryStatus();
    void ShowFileDirectories(bool models);''')
edit('modules/app/app.hpp','        bool captureReady=false;','''        std::wstring modelPath;
        albion::FileVersion modelVersion;
        bool modelLoading=false;
        bool captureReady=false;''')
# No I/O in path construction: writes already create their parent directories transactionally.
edit('modules/config/configuration.cpp','''        const std::filesystem::path dir = AlbionDataPath(L"configs");
        std::filesystem::create_directories(dir);
        return dir.wstring();''','''        return (std::filesystem::path(AlbionProgramDirectory()) / L"data" / L"configs").wstring();''')
p=R/'modules/config/configuration.cpp';s=p.read_text();a=s.index('void App::RefreshConfigList(');b=s.index('\nvoid App::UpdateConfigButtons()',a)
s=s[:a]+'''void App::RefreshConfigList(const std::wstring& selectName)
{
        if(!selectName.empty()) mPreferredConfigName=selectName;
        EnsureFileDiscovery();
        mFileCatalog.Request();
        UpdateFileDiscoveryStatus();
    }
'''+s[b:]
a=s.index('void App::LoadSelectedConfig()');b=s.index('\nvoid App::DeleteSelectedConfig()',a)
s=s[:a]+'''void App::LoadSelectedConfig()
{
        if(mConfigEditing) return;
        const auto name=SelectedConfigName(); if(name.empty()) return;
        const auto path=ConfigPath(name);
        const auto* file=mFileSnapshot?albion::FileCatalog::Find(*mFileSnapshot,path):nullptr;
        if(!file || file->readiness!=albion::FileReadiness::Candidate) {
            SetConfigStatus(L"Profile is still changing, unavailable or missing. Refresh and try Apply again.",true);return;
        }
        mFileCatalog.ReadProfile(path,++mProfileRequest,mSettingsRevision);
        SetConfigStatus(L"Reading and validating profile; working settings are unchanged...");
    }
'''+s[b:]
s=s.replace('''                const DWORD attrs = GetFileAttributesW(mModelFiles[i].c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) modelIndex = int(i);
                break;''','''                modelIndex=int(i);
                if(mFileSnapshot) {
                    const auto* file=albion::FileCatalog::Find(*mFileSnapshot,mModelFiles[i]);
                    if(!file || file->readiness!=albion::FileReadiness::Candidate) modelIndex=-1;
                }
                break;''')
s=s.replace('''        SendMessageW(mModelCombo, CB_SETCURSEL, WPARAM(modelIndex), 0);
        mApplyingConfig = false;''','''        SendMessageW(mModelCombo, CB_SETCURSEL, WPARAM(modelIndex), 0);
        mSelectedModelPath=mModelFiles[size_t(modelIndex)]; mStartupModelAllowed=false;
        if(mFileSnapshot) if(const auto* file=albion::FileCatalog::Find(*mFileSnapshot,mSelectedModelPath)) {
            mModelRequestedChange=file->changed; mModelRequestedPath=file->path;
        }
        mApplyingConfig = false;''')
# No neighbor selection following deletion. Refresh only changes the catalog.
a=s.index('''        const int oldIndex =''',s.index('void App::DeleteSelectedConfig()'));b=s.index('\n        const std::wstring question',a);s=s[:a]+s[b:]
a=s.index('''            const int count =''',s.index('void App::DeleteSelectedConfig()'));b=s.index('''            UpdateConfigButtons();''',a);s=s[:a]+s[b:]
# Scroll position is retained by path/name, not only a numeric index.
s=s.replace('''        const int top = int(SendMessageW(mConfigList, LB_GETTOPINDEX, 0, 0));''','''        const int top = int(SendMessageW(mConfigList, LB_GETTOPINDEX, 0, 0));
        wchar_t topName[260]{};
        const auto topLength=SendMessageW(mConfigList,LB_GETTEXTLEN,WPARAM(top),0);
        if(topLength>=0 && topLength<260) SendMessageW(mConfigList,LB_GETTEXT,WPARAM(top),LPARAM(topName));
        int nextTop=-1;''')
s=s.replace('''            if (ConfigNameEqual(entry.name, selected)) nextSelection = int(row);''','''            if (ConfigNameEqual(entry.name, selected)) nextSelection = int(row);
            if(ConfigNameEqual(entry.name,topName)) nextTop=int(row);''')
s=s.replace('''        if (nextSelection < 0 && !mConfigVisible.empty())
            SendMessageW(mConfigList, LB_SETTOPINDEX, WPARAM(std::clamp(top, 0, int(mConfigVisible.size()) - 1)), 0);''','''        if(!mConfigVisible.empty())
            SendMessageW(mConfigList,LB_SETTOPINDEX,WPARAM(nextTop>=0?nextTop:std::clamp(top,0,int(mConfigVisible.size())-1)),0);''')
p.write_text(s)
# The one startup request is chosen from the first catalog, never from later refreshes.
edit('modules/app/lifecycle.cpp','''        CreateSettingsWindow(hInst);''','''        mStartupModelAllowed=true;
        CreateSettingsWindow(hInst);''')
edit('modules/app/lifecycle.cpp','''        mDet.Start(CurrentModelPath());''','''        QueueSettings();''')
edit('modules/app/lifecycle.cpp','''App::~App()
{
        mConverter.Cancel();mConverter.Join();''','''App::~App()
{
        if(mHwnd) KillTimer(mHwnd,kFileCatalogTimer);
        mFileCatalog.Stop();
        mConverter.Cancel();mConverter.Join();''')
# Controls use the existing owner-drawn action style and existing native tabs.
edit('modules/ui/controls.cpp','''                           mDiagnosticsLabel, mDiagnosticsButton };''','''                           mDiagnosticsLabel, mDiagnosticsButton,
                           mModelRefreshBtn,mModelReloadBtn,mModelFoldersBtn,mModelDiscoveryLabel };''')
edit('modules/ui/controls.cpp','''                               mConfigNewBtn, mConfigConfirmBtn, mConfigCancelBtn };''','''                               mConfigNewBtn, mConfigConfirmBtn, mConfigCancelBtn,
                               mConfigRefreshBtn,mConfigFolderBtn,mConfigDiscoveryLabel };''')
edit('modules/ui/controls.cpp','''        SendMessageW(mConfigNameEdit, EM_LIMITTEXT, 120, 0);''','''        mModelRefreshBtn=configControl(L"BUTTON",L"Refresh",actionStyle,kModelRefresh);
        mModelReloadBtn=configControl(L"BUTTON",L"Load / reload",actionStyle,kModelReload);
        mModelFoldersBtn=configControl(L"BUTTON",L"Model folders...",actionStyle,kModelFolders);
        mModelDiscoveryLabel=configControl(L"STATIC",L"Auto discovery on · refreshing...",SS_LEFT,0);
        mConfigRefreshBtn=configControl(L"BUTTON",L"Refresh",actionStyle,kConfigRefresh);
        mConfigFolderBtn=configControl(L"BUTTON",L"Folder...",actionStyle,kConfigFolder);
        mConfigDiscoveryLabel=configControl(L"STATIC",L"Auto discovery on · refreshing...",SS_LEFT|SS_ENDELLIPSIS,0);
        SendMessageW(mConfigNameEdit, EM_LIMITTEXT, 120, 0);''')
edit('modules/ui/controls.cpp','''                      mConfigStatusLabel, mConfigNewBtn, mConfigConfirmBtn, mConfigCancelBtn };''','''                      mConfigStatusLabel, mConfigNewBtn, mConfigConfirmBtn, mConfigCancelBtn,
                      mModelRefreshBtn,mModelReloadBtn,mModelFoldersBtn,mModelDiscoveryLabel,
                      mConfigRefreshBtn,mConfigFolderBtn,mConfigDiscoveryLabel };''')
edit('modules/ui/controls.cpp','''        SendMessageW(mConfigStatusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(mFontSmall), TRUE);''','''        SendMessageW(mConfigStatusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(mFontSmall), TRUE);
        for(HWND h:{mModelDiscoveryLabel,mConfigDiscoveryLabel}) SendMessageW(h,WM_SETFONT,WPARAM(mFontSmall),TRUE);''')
edit('modules/ui/controls.cpp','''            mConfigNewBtn, mConfigConfirmBtn, mConfigCancelBtn,''','''            mConfigNewBtn, mConfigConfirmBtn, mConfigCancelBtn,
            mModelRefreshBtn,mModelReloadBtn,mModelFoldersBtn,mConfigRefreshBtn,mConfigFolderBtn,''')
edit('modules/ui/drawing.cpp','''        put(mConfigTitle, 148, 104, 592, 24);''','''        put(mModelRefreshBtn,L,328,160,32);
        put(mModelReloadBtn,L+172,328,184,32);
        put(mModelFoldersBtn,L+368,328,208,32);
        put(mModelDiscoveryLabel,L,370,R-L,94);
        put(mConfigTitle,148,104,340,24);
        put(mConfigRefreshBtn,524,100,112,30);
        put(mConfigFolderBtn,648,100,116,30);
        put(mConfigDiscoveryLabel,156,132,600,18);''')
edit('modules/ui/drawing.cpp','''            di->hwndItem == mConfigNewBtn || di->hwndItem == mConfigConfirmBtn || di->hwndItem == mConfigCancelBtn)''','''            di->hwndItem == mConfigNewBtn || di->hwndItem == mConfigConfirmBtn || di->hwndItem == mConfigCancelBtn ||
            di->hwndItem==mModelRefreshBtn || di->hwndItem==mModelReloadBtn || di->hwndItem==mModelFoldersBtn ||
            di->hwndItem==mConfigRefreshBtn || di->hwndItem==mConfigFolderBtn)''')
edit('modules/ui/window.cpp','''        case WM_COMMAND:
''','''        case WM_COMMAND:
            if(HIWORD(w)==BN_CLICKED) {
                switch(LOWORD(w)) {
                    case kModelRefresh: mFileCatalog.Request();UpdateFileDiscoveryStatus();return 0;
                    case kConfigRefresh: RefreshConfigList();return 0;
                    case kModelReload: ApplyModel(true);return 0;
                    case kModelFolders: ShowFileDirectories(true);return 0;
                    case kConfigFolder: ShowFileDirectories(false);return 0;
                }
            }
''')
edit('modules/ui/window.cpp','''        case WM_TIMER:
''','''        case WM_TIMER:
            if(w==kFileCatalogTimer) { PollFileDiscovery();return 0; }
''')
# A thread-safe loaded-source snapshot; metadata is acquired only by the loader.
edit('modules/inference/detector.hpp','#include "model_contract.hpp"','#include "model_contract.hpp"\n#include "file_identity.hpp"')
edit('modules/inference/detector.hpp','    bool Loading() const;','    bool Loading() const;\n    albion::FileVersion SourceVersion();')
edit('modules/inference/detector.hpp','    std::string             mError;','    std::string             mError;\n    albion::FileVersion mSourceVersion;')
p=R/'modules/inference/detector.cpp';s=p.read_text();a=s.index('        const std::wstring sidecar =');b=s.index('        mExited.store(false',a);sidecar=s[a:b];s=s[:a]+s[b:];s=s.replace('const std::wstring sidecar = modelPath','const std::wstring sidecar = mModelPath')
a=s.index('        std::string err;',s.index('void Detector::Loop()'));s=s[:a]+'''        albion::ModelReadLease source(mModelPath);
'''+sidecar.replace('modelPath +','mModelPath +')+s[a:]
s=s.replace('''        auto backend = CreateInferenceBackend(mModelPath, netSize, err);
        if (backend)''','''        auto backend = CreateInferenceBackend(mModelPath, netSize, err);
        { std::lock_guard lock(mResMx);mSourceVersion=source.version; }
        source.Close();
        if (backend)''')
s=s.replace('''bool Detector::Loading() const''','''albion::FileVersion Detector::SourceVersion()
{ std::lock_guard lock(mResMx);return mSourceVersion; }

bool Detector::Loading() const''')
s=s.replace('''mBackendDetails.clear(); mStageTiming={};''','''mBackendDetails.clear(); mStageTiming={}; mSourceVersion={};''')
p.write_text(s)
edit('modules/pipeline/pipeline.cpp','''        snapshot.ready = mDet.Ready(); snapshot.revision = mAppliedRevision;''','''        snapshot.ready = mDet.Ready(); snapshot.revision = mAppliedRevision;
        snapshot.modelPath=mDet.ModelPath();snapshot.modelVersion=mDet.SourceVersion();snapshot.modelLoading=mDet.Loading();''')
edit('modules/pipeline/pipeline.cpp','''        mUiSnap.ready = snapshot.ready; mUiSnap.revision = snapshot.revision;''','''        mUiSnap.ready = snapshot.ready; mUiSnap.revision = snapshot.revision;
        mUiSnap.modelPath=std::move(snapshot.modelPath);mUiSnap.modelVersion=snapshot.modelVersion;mUiSnap.modelLoading=snapshot.modelLoading;''')
