// modules/app/commands.cpp
#include "modules/app/app.hpp"

albion::output::Receipt App::ArduinoMouseSink(LONG dx, LONG dy)
{
        if (auto* sink = sArduinoSink.load(std::memory_order_acquire)) return sink->MoveRelativeReceipt(int(dx), int(dy));
        return {};
    }

void App::QueueSettings(bool includeModel)
{
        if (mApplyingConfig) return;
        if (!mLoadedConfigName.empty()) mConfigDirty = true;
        SettingsCommand next{mAimCfg, int(mAimMethod), int(mCaptureMethod), ++mSettingsRevision};
        const auto path = includeModel ? CurrentModelPath() : std::wstring{};
        { std::lock_guard<std::mutex> lock(mCommandMx); mPendingSettings = next;
          if (includeModel) mPendingModel = ModelRequest{path, false}; }
        mDet.NotifyActivity();
    }

void App::ProcessCommands()
{
        std::optional<SettingsCommand> settings; std::optional<ModelRequest> model; bool retry = false;
        { std::lock_guard<std::mutex> lock(mCommandMx);
          settings.swap(mPendingSettings); model.swap(mPendingModel);
          retry = mPendingCaptureRetry; mPendingCaptureRetry = false; }
        if (settings) {
            const auto old=mAim.Config().extra;
            const bool sourceChanged=int(mWorkerCaptureMethod)!=settings->capture||old.windowTitle!=settings->config.extra.windowTitle;
            const bool backendChanged=old.backend!=settings->config.extra.backend||old.dmlDevice!=settings->config.extra.dmlDevice;
            mAim.Reset();mRecoil.Reset();mAimTarget.reset();mAim.Config()=settings->config;
            if(mAim.Config().extra.recoil)mAim.Config().asyncMover=false;
            if(old.record&&!mAim.Config().extra.record)mFrames.CancelRecording();
            const auto&o=mAim.Config().extra;gDecodeClass.store(o.headBody?-1:settings->config.targetClass);
            gNmsIou.store(o.iou);gMaxDetections.store(unsigned(o.maxDetections));gPostprocess.store(o.postprocess);gBackendChoice.store(o.backend);gDmlDevice.store(o.dmlDevice);
            if((backendChanged||sourceChanged)&&!model)model=ModelRequest{mDet.ModelPath(),true};
            if(sourceChanged)mDet.Stop(); // old-source results cannot reach the new capture epoch
            gDecodeConf.store(settings->config.confidence);
            mLastPreprocessFrameId=mCapturedFrameId; // discard cached ROI on settings revision
            mCaptureRegion = MakeCenteredRegion(mDeskW, mDeskH, settings->config.radius);
            mWorkerCaptureMethod = static_cast<CaptureMethod>(settings->capture); mCapFailures = 0;
            RefreshCaptureRegion();if(sourceChanged){mSharedFrames.Reset();mHasTensor=false;mPre.DiscardCpuFrames();StartCaptureMode();mCaptureFailed=false;}
            const auto method = static_cast<AimMethod>(settings->method);
            if (method != mWorkerAimMethod) {
                mAim.Shutdown(); // no old sender can outlive a device/sink switch
                if (method == AimMethod::ArduinoHid) {
                    sArduinoSink.store(&mArduino); gMouseReceiptSink.store(&App::ArduinoMouseSink);
                    mArduino.Connect(); // queues enumeration/handshake, never falls back silently
                } else {
                    gMouseReceiptSink.store(nullptr); sArduinoSink.store(nullptr); mArduino.Disconnect();
                }
                mWorkerAimMethod = method;
            }
            mAppliedRevision = settings->revision;
        }
        if (retry) {
            // Explicit recovery owns the full teardown: detector releases its
            // registrations BEFORE a replacement device/preprocessor is created.
            mDet.Stop(); mSharedFrames.Reset(); mHasTensor = false; mPre.DiscardCpuFrames();
            mFrameSRV.Reset(); mFrameTex.Reset(); mDupl.Reset(); mDesktopSrvs.Reset();
#ifdef AIM_ENABLE_WGC
            if (mUseWgc) mWgc.Stop();
#endif
            CreateDevice(); mPre = GpuPreprocessor{}; mPre.Init(mDev.Get(), AimPerfEnvFlag(L"ALBION_GPU_TIMING"));
            mDet.EnableSharedInput(mDev.Get(), mPre.TensorBuffer(0), mPre.TensorBuffer(1));
            StartCaptureMode();
            mFrameW = mFrameH = 0; mLastPreprocessFrameId = 0; mCapturedFrameId = 0;
            if (!model) model = ModelRequest{mDet.ModelPath(), true};
            mCaptureFailed = false;
        }
        if (model && (model->force || model->path != mDet.ModelPath() || (!mDet.Ready() && !mDet.Loading()))) {
            mDet.Stop(); // FIX-V11-04/08: owner join, never under UI/global lock
            mDets.clear(); mAimTarget.reset(); mAim.Reset(); mLastDetVer = 0;
            mSharedFrames.Reset(); mHasTensor = false; mPre.DiscardCpuFrames();
            mLatestTiming = {}; mLatestStages = {}; mPipelineMs = 0; mLastPreprocessFrameId = 0;
            mDet.Start(model->path);
        }
    }
