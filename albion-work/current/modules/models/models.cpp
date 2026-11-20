// modules/models/models.cpp
#include "modules/app/app.hpp"

void App::StartModelConversion() {} // No compiler is shipped or invoked.
void App::PollModelConversion() {}
bool App::PauseForModelConversion(PipelineActivityWaiter&) { return false; }

namespace {
std::wstring CandidateState(const albion::CatalogFile* file) {
    if(!file) return L"removed / not on disk";
    if(file->error) return L"locked or inaccessible (Windows "+std::to_wstring(file->error)+L")";
    if(file->readiness==albion::FileReadiness::Settling) return L"still changing; wait before loading";
    return L"unvalidated candidate; Load / Apply validates";
}
}

void App::EnsureFileDiscovery()
{
    if(mDiscoveryStarted) return;
    mDiscoveryStarted=mFileCatalog.Start({
        {ConfigDirectory(),albion::FileKind::Profile,true},
        {ModelRelativePath(L""),albion::FileKind::Model},
        {ExeRelativePath(L""),albion::FileKind::Model}});
    if(mDiscoveryStarted && mHwnd) SetTimer(mHwnd,kFileCatalogTimer,250,nullptr);
}

void App::ScanModelFiles() { EnsureFileDiscovery();mFileCatalog.Request(); }
void App::FillModelCombo() { ScanModelFiles(); }

std::wstring App::FileNameOf(const std::wstring& path)
{
    const auto pos=path.find_last_of(L"\\/");return pos==std::wstring::npos?path:path.substr(pos+1);
}

std::wstring App::CurrentModelPath() const
{
    const int selected=mModelCombo?int(SendMessageW(mModelCombo,CB_GETCURSEL,0,0)):CB_ERR;
    if(selected>=0 && selected<int(mModelFiles.size())) return mModelFiles[size_t(selected)];
    return mSelectedModelPath;
}

void App::ApplyModel(bool force)
{
    if(mApplyingConfig) return;
    mStartupModelAllowed=false;
    const auto path=CurrentModelPath();mSelectedModelPath=path;
    const auto* file=mFileSnapshot?albion::FileCatalog::Find(*mFileSnapshot,path):nullptr;
    if(!file || file->readiness!=albion::FileReadiness::Candidate) { UpdateFileDiscoveryStatus();return; }
    if(!force&&!mLoadedConfigName.empty()) mConfigDirty=true;
    mModelRequestedPath=path;mModelRequestedChange=file->changed;
    SettingsCommand next{mAimCfg,int(mAimMethod),int(mCaptureMethod),++mSettingsRevision};
    { std::lock_guard lock(mCommandMx);mPendingSettings=next;mPendingModel=ModelRequest{path,force}; }
    mDet.NotifyActivity();UpdateFileDiscoveryStatus();
}

void App::ShowFileDirectories(bool models)
{
    const auto text=models?L"Model folders (not recursive):\n\n"+ModelRelativePath(L"")+L"\n"+ExeRelativePath(L"")+
        L"\n\nSupported: .onnx, .engine, .plan, .trt. Discovery never builds an engine.\nA listed filename is not proof of validity. Ctrl+C copies this dialog.":
        L"Configuration folder (not recursive):\n\n"+ConfigDirectory()+L"\n\nSupported: .cfg. Refresh never applies a profile. Ctrl+C copies this dialog.";
    MessageBoxW(mHwnd,text.c_str(),L"Albion Experimental — file directories",MB_OK|MB_ICONINFORMATION);
}

void App::PollFileDiscovery()
{
    auto snapshot=mFileCatalog.Latest();
    if(snapshot) {
        mFileSnapshot=snapshot;
        if(!mStartupChoiceKnown) {
            mStartupChoiceKnown=true;
            for(const auto* ext:{L".onnx",L".engine",L".plan",L".trt"}) {
                for(const auto& file:snapshot->files) if(file.kind==albion::FileKind::Model &&
                    ConfigNameEqual(std::filesystem::path(file.path).parent_path().wstring(),ModelRelativePath(L"")) &&
                    ConfigNameEqual(std::filesystem::path(file.name).extension().wstring(),ext)) {
                    mStartupModelPath=file.path;break;
                }
                if(!mStartupModelPath.empty()) break;
            }
            if(mStartupModelPath.empty()) for(const auto& file:snapshot->files) if(file.kind==albion::FileKind::Model) {mStartupModelPath=file.path;break;}
            if(mSelectedModelPath.empty()) mSelectedModelPath=mStartupModelPath;
            if(mStartupModelPath.empty()) mStartupModelAllowed=false;
        }
        if(snapshot->sequence!=mModelCatalogSequence && !SendMessageW(mModelCombo,CB_GETDROPPEDSTATE,0,0)) {
            const auto selected=CurrentModelPath();
            std::vector<std::wstring> paths;
            for(const auto& root:snapshot->directories) if(root.kind==albion::FileKind::Model)
                for(const auto& file:snapshot->files) if(file.kind==albion::FileKind::Model &&
                    ConfigNameEqual(std::filesystem::path(file.path).parent_path().wstring(),std::filesystem::path(root.path).lexically_normal().wstring()))
                    paths.push_back(file.path);
            if(paths!=mModelFiles) {
                mSelectedModelPath=selected;
                SendMessageW(mModelCombo,WM_SETREDRAW,FALSE,0);SendMessageW(mModelCombo,CB_RESETCONTENT,0,0);
                mModelFiles=std::move(paths);int selection=-1;
                for(size_t i=0;i<mModelFiles.size();++i) {
                    const auto& path=mModelFiles[i];
                    const auto label=FileNameOf(path)+(ConfigNameEqual(std::filesystem::path(path).parent_path().wstring(),ModelRelativePath(L""))?L"  [models]":L"  [exe]");
                    SendMessageW(mModelCombo,CB_ADDSTRING,0,LPARAM(label.c_str()));
                    if(ConfigNameEqual(path,mSelectedModelPath)) selection=int(i);
                }
                SendMessageW(mModelCombo,CB_SETCURSEL,WPARAM(selection),0);
                SendMessageW(mModelCombo,WM_SETREDRAW,TRUE,0);InvalidateRect(mModelCombo,nullptr,FALSE);
            }
            mModelCatalogSequence=snapshot->sequence;
        }
        if(snapshot->sequence!=mConfigCatalogSequence) {
            const auto selected=SelectedConfigName();if(!selected.empty())mRememberedConfigName=selected;
            const auto preferred=!mPreferredConfigName.empty()?mPreferredConfigName:mRememberedConfigName;
            std::vector<ConfigEntry> entries;
            for(const auto& file:snapshot->files) if(file.kind==albion::FileKind::Profile) {
                const auto name=std::filesystem::path(file.name).stem().wstring();
                entries.push_back({name,L".cfg · "+std::to_wstring(file.version.bytes)+L" bytes · "+CandidateState(&file)});
            }
            if(entries!=mConfigEntries || !mPreferredConfigName.empty()) {
                mConfigEntries=std::move(entries);RebuildConfigList(preferred);
                if(!SelectedConfigName().empty()) {mRememberedConfigName=SelectedConfigName();mPreferredConfigName.clear();}
            }
            mConfigCatalogSequence=snapshot->sequence;
        }
        if(mStartupModelAllowed && !mStartupModelPath.empty()) {
            const auto* initial=albion::FileCatalog::Find(*snapshot,mStartupModelPath);
            if(!initial) mStartupModelAllowed=false;
            else if(initial->readiness==albion::FileReadiness::Candidate) ApplyModel();
        }
    }
    if(auto result=mFileCatalog.TakeProfile()) {
        if(result->request==mProfileRequest) {
            const auto selected=SelectedConfigName();
            if(result->settingsRevision!=mSettingsRevision || mConfigEditing || selected.empty() || !ConfigNameEqual(result->path,ConfigPath(selected)))
                SetConfigStatus(L"Settings or selection changed while reading. Nothing applied; select Apply again.");
            else if(!result->error.empty()) SetConfigStatus(L"Profile invalid, incomplete or inaccessible: "+Utf8ToWide(result->error),true);
            else {
                std::wstring why;
                if(ApplyConfigText(result->text,&why)) {
                    mLoadedConfigName=selected;mConfigDirty=false;mLoadedConfigRevision=mSettingsRevision;
                    mLoadedProfileVersion=result->version;
                    if(mFileSnapshot) if(const auto* file=albion::FileCatalog::Find(*mFileSnapshot,result->path))mLoadedProfileChange=file->changed;
                    mLastProfileStatus.clear();SetConfigStatus(L"Profile validated and applied: "+selected);
                } else SetConfigStatus(L"Profile not applied: "+why,true);
            }
        }
    }
    UpdateFileDiscoveryStatus();
}

void App::UpdateFileDiscoveryStatus()
{
    std::wstring models=L"Auto discovery on",profiles=L"Auto discovery on";
    if(!mDiscoveryStarted) models=profiles=L"Discovery unavailable — Refresh to retry";
    else if(mFileCatalog.Busy()) {models+=L" · refreshing...";profiles+=L" · refreshing...";}
    if(mFileSnapshot) {
        size_t modelCount=0,profileCount=0;bool modelError=false,profileError=false,watchError=false;
        for(const auto& file:mFileSnapshot->files) (file.kind==albion::FileKind::Model?modelCount:profileCount)++;
        for(const auto& root:mFileSnapshot->directories) {if(root.error)(root.kind==albion::FileKind::Model?modelError:profileError)=true;watchError|=root.watcherError!=0;}
        models+=L" · "+std::to_wstring(modelCount)+L" candidates";profiles+=L" · "+std::to_wstring(profileCount)+L" profiles";
        if(!modelCount)models+=L" · no supported models";
        if(!profileCount)profiles+=L" · no supported files";
        if(modelError)models+=L" · directory unavailable (retrying)";
        if(profileError)profiles+=L" · directory unavailable (retrying)";
        if(watchError){models+=L" · watcher degraded; 5s reconcile / Refresh";profiles+=L" · watcher degraded; Refresh available";}
        if(mFileSnapshot->truncated){models+=L" · 4096-file limit; narrow directories";profiles+=L" · file limit reached";}
        const auto selected=CurrentModelPath();
        if(!selected.empty()) models+=L"\nSelected: "+FileNameOf(selected)+L" — "+CandidateState(albion::FileCatalog::Find(*mFileSnapshot,selected));
        else models+=L"\nChoose a model to load; discovery does not activate files.";
        UiSnapshot state;{std::lock_guard lock(mUiMx);state.modelPath=mUiSnap.modelPath;state.modelVersion=mUiSnap.modelVersion;state.modelLoading=mUiSnap.modelLoading;state.ready=mUiSnap.ready;state.error=mUiSnap.error;}
        if(state.modelLoading)models+=L"\nLoading: "+FileNameOf(state.modelPath);
        else if(state.ready) {
            models+=L"\nLoaded: "+FileNameOf(state.modelPath);
            const auto* file=albion::FileCatalog::Find(*mFileSnapshot,state.modelPath);
            if(!file) models+=L" — source removed; loaded session retained";
            else if(file->version!=state.modelVersion || (ConfigNameEqual(state.modelPath,mModelRequestedPath)&&file->changed>mModelRequestedChange))
                models+=L" — disk change detected; Reload to use new version";
        } else models+=L"\nNo loaded model"+(state.error.empty()?L"":L": "+Utf8ToWide(state.error));
        const auto* chosen=albion::FileCatalog::Find(*mFileSnapshot,selected);
        EnableWindow(mModelReloadBtn,chosen&&chosen->readiness==albion::FileReadiness::Candidate&&!state.modelLoading);
        if(!mLoadedConfigName.empty()) {
            const auto* file=albion::FileCatalog::Find(*mFileSnapshot,ConfigPath(mLoadedConfigName));
            if(!file)profiles+=L" · active source removed; settings retained";
            else if(mLoadedProfileVersion && (file->version!=*mLoadedProfileVersion || file->changed>mLoadedProfileChange))
                profiles+=L" · active file changed; Apply to reload";
        }
        if(!mRememberedConfigName.empty()&&!albion::FileCatalog::Find(*mFileSnapshot,ConfigPath(mRememberedConfigName)))profiles+=L" · selected file removed";
        if(mConfigDirty)profiles+=L" · unsaved edits retained";
    }
    if(models!=mModelDiscoveryText) {mModelDiscoveryText=models;if(mModelDiscoveryLabel)SetWindowTextW(mModelDiscoveryLabel,models.c_str());}
    if(profiles!=mConfigDiscoveryText) {mConfigDiscoveryText=profiles;if(mConfigDiscoveryLabel)SetWindowTextW(mConfigDiscoveryLabel,profiles.c_str());}
}
