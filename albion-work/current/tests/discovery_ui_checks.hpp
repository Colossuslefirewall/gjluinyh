    static void Discovery(App& a) {
        namespace fs=std::filesystem;
        const auto root=fs::temp_directory_path()/(L"Albion-discovery-ui-"+std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(root/L"models");fs::create_directories(root/L"configs");
        struct Cleanup { App& app;fs::path root;~Cleanup(){app.mFileCatalog.Stop();std::error_code ec;fs::remove_all(root,ec);} } cleanup{a,root};
        auto write=[](const fs::path& path,const std::string& text){std::ofstream out(path,std::ios::binary);out<<text;out.close();Check(bool(out),"fixture write");};
        auto wait=[&](auto predicate,const char* name){const auto end=GetTickCount64()+9000;do{Pump(30);a.PollFileDiscovery();if(predicate())return;}while(GetTickCount64()<end);Check(false,name);};
        const auto model=root/L"models"/L"Model Ω with spaces.onnx";
        const auto alpha=root/L"configs"/L"alpha.cfg",beta=root/L"configs"/L"beta.cfg";
        write(model,"discovery fixture only; never an inference model");
        a.mStartupModelAllowed=false;
        a.mFileCatalog.Configure({{(root/L"configs").wstring(),albion::FileKind::Profile},{(root/L"models").wstring(),albion::FileKind::Model}});
        auto ready=[&](const fs::path& path){const auto* f=a.mFileSnapshot?albion::FileCatalog::Find(*a.mFileSnapshot,path.wstring()):nullptr;return f&&f->readiness==albion::FileReadiness::Candidate;};
        wait([&]{return ready(model)&&a.mModelFiles.size()==1;},"model appeared asynchronously");
        {std::lock_guard lock(a.mCommandMx);a.mPendingModel.reset();a.mPendingSettings.reset();}
        SendMessageW(a.mModelCombo,CB_SETCURSEL,0,0);
        SendMessageW(a.mHwnd,WM_COMMAND,MAKEWPARAM(IDC_MODEL,CBN_SELCHANGE),LPARAM(a.mModelCombo));
        Check(!a.mPendingModel,"selecting model does not activate it");
        a.mAimCfg.extra.comparisonMode=1;write(alpha,a.BuildConfigText());write(beta,a.BuildConfigText());
        wait([&]{return ready(alpha)&&ready(beta);},"profile candidates appeared");
        auto select=[&](const wchar_t* name){auto index=SendMessageW(a.mConfigList,LB_FINDSTRINGEXACT,WPARAM(-1),LPARAM(name));Check(index!=LB_ERR,"profile found by name");SendMessageW(a.mConfigList,LB_SETCURSEL,WPARAM(index),0);a.OnConfigSelectionChanged();};
        select(L"alpha");a.mAimCfg.extra.comparisonMode=4;a.QueueSettings();a.mConfigDirty=true;
        const auto revision=a.mSettingsRevision;
        a.RefreshConfigList();wait([&]{return !a.mFileCatalog.Busy();},"manual refresh finished");
        Check(a.mSettingsRevision==revision&&a.mAimCfg.extra.comparisonMode==4&&a.mConfigDirty&&!a.mPendingModel,"refresh preserves working settings and does not activate");
        Check(a.SelectedConfigName()==L"alpha"&&a.CurrentModelPath()==model.wstring(),"refresh preserves both selections");
        // ConfigPath normally uses the executable-relative directory; the harness uses that real location for Apply.
        const auto realProfile=fs::path(a.ConfigPath(L"discovery-fixture"));
        Check(!fs::exists(realProfile),"test never overwrites an existing profile");
        struct RemoveProfile {fs::path path;~RemoveProfile(){std::error_code ec;fs::remove(path,ec);}} removeProfile{realProfile};
        fs::create_directories(realProfile.parent_path());write(realProfile,a.BuildConfigText());
        a.mFileCatalog.Configure({{realProfile.parent_path().wstring(),albion::FileKind::Profile},{(root/L"models").wstring(),albion::FileKind::Model}});
        wait([&]{return ready(realProfile);},"executable-relative profile discovered");select(L"discovery-fixture");
        a.LoadSelectedConfig();a.mAimCfg.extra.comparisonMode=2;a.QueueSettings();const auto changed=a.mSettingsRevision;
        wait([&]{return !a.mFileCatalog.Reading();},"pending profile read completed");a.PollFileDiscovery();
        Check(a.mSettingsRevision==changed&&a.mAimCfg.extra.comparisonMode==2,"late profile cannot overwrite edits");
        a.LoadSelectedConfig();wait([&]{return a.mLoadedConfigName==L"discovery-fixture";},"explicit Apply completes");
        Check(a.mAimCfg.extra.comparisonMode==4&&a.mPendingModel&&a.mPendingModel->path==model.wstring(),"validated profile queues explicit model activation");
        {std::lock_guard lock(a.mCommandMx);a.mPendingModel.reset();}
        a.BeginConfigEdit(false);SetWindowTextW(a.mConfigNameEdit,L"Unsaved draft name");a.RefreshConfigList();
        wait([&]{return !a.mFileCatalog.Busy();},"refresh while editing");
        wchar_t draft[128]{};GetWindowTextW(a.mConfigNameEdit,draft,128);Check(std::wstring(draft)==L"Unsaved draft name"&&a.mConfigEditing,"refresh preserves unfinished name editing");a.FinishConfigEdit();
        a.mConfigDirty=true;const auto preserved=a.mAimCfg.extra.comparisonMode;fs::remove(realProfile);
        wait([&]{return !albion::FileCatalog::Find(*a.mFileSnapshot,realProfile.wstring());},"active profile removal discovered");
        Check(a.SelectedConfigName().empty()&&a.mAimCfg.extra.comparisonMode==preserved&&a.mConfigDirty&&!a.mPendingModel,"removed selection does not select neighbor or change working state");
        Check(a.mConfigDiscoveryText.find(L"active source removed")!=std::wstring::npos,"active profile removal is visible");
        a.ToggleFileDiscovery();Check(!a.mFileCatalog.Automatic(),"auto discovery can be disabled");
        wait([&]{return !a.mFileCatalog.Busy();},"auto off settled");
        const auto manual=root/L"models"/L"Manual refresh.plan";write(manual,"candidate");Pump(1100);a.PollFileDiscovery();
        Check(!albion::FileCatalog::Find(*a.mFileSnapshot,manual.wstring()),"auto off ignores new files");
        a.mFileCatalog.Request();wait([&]{return ready(manual);},"manual refresh works with auto off");
        Check(a.CurrentModelPath()==model.wstring()&&!a.mPendingModel,"new model is not automatically selected or loaded");
        a.ToggleFileDiscovery();Check(a.mFileCatalog.Automatic(),"automatic discovery can be restored");
        a.ApplyTab(2);Save(a.mHwnd,L"review-results/discovery-models.png");
        a.ApplyTab(3);Save(a.mHwnd,L"review-results/discovery-profiles.png");
        std::cout<<"DISCOVERY_UI_PASS refresh selection dirty_edits async_apply stale_apply removal auto_toggle no_implicit_activation\n";
    }
