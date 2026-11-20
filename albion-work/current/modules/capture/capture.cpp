// modules/capture/capture.cpp
#include "modules/app/app.hpp"

void App::CreateDevice()
{
        // FIX-V11-13: Choose an OUTPUT first, then create D3D on its adapter.
        // Default = Windows primary monitor, not arbitrary adapter/output index 0.
        // ALBION_CAPTURE_MONITOR may name a specific output, e.g. \\.\DISPLAY2.
        ComPtr<IDXGIFactory1> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> chosenAdapter; ComPtr<IDXGIOutput> chosenOutput;
        DXGI_OUTPUT_DESC chosen{};
        wchar_t requested[128]{};
        const DWORD n = GetEnvironmentVariableW(L"ALBION_CAPTURE_MONITOR", requested, 128);
        if (n >= 128) throw std::runtime_error("ALBION_CAPTURE_MONITOR is too long");
        const HMONITOR primary = MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY);
        for (UINT ai = 0;; ++ai) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT ah = factory->EnumAdapters1(ai, &adapter);
            if (ah == DXGI_ERROR_NOT_FOUND) break; HR(ah, "EnumAdapters1");
            for (UINT oi = 0;; ++oi) {
                ComPtr<IDXGIOutput> output;
                const HRESULT oh = adapter->EnumOutputs(oi, &output);
                if (oh == DXGI_ERROR_NOT_FOUND) break; HR(oh, "EnumOutputs");
                DXGI_OUTPUT_DESC desc{}; HR(output->GetDesc(&desc), "Output description");
                if (!desc.AttachedToDesktop) continue;
                const bool matches = n ? CompareStringOrdinal(desc.DeviceName, -1, requested, -1, TRUE) == CSTR_EQUAL : desc.Monitor == primary;
                if (matches) { chosenAdapter = adapter; chosenOutput = output; chosen = desc; break; }
            }
            if (chosenOutput) break;
        }
        if (!chosenOutput) throw std::runtime_error("Requested/primary capture monitor is not available");
        // A rotated duplication surface needs shader/copy rotation support. Do
        // not silently produce offset/rotated results on unsupported hardware.
        if (chosen.Rotation != DXGI_MODE_ROTATION_IDENTITY && chosen.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
            throw std::runtime_error("Portrait/rotated capture is not supported yet; select a landscape capture monitor");
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(chosenAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            levels, _countof(levels), D3D11_SDK_VERSION, &mDev, nullptr, &mCtx);
#ifdef _DEBUG
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) hr = D3D11CreateDevice(chosenAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG, levels, _countof(levels), D3D11_SDK_VERSION, &mDev, nullptr, &mCtx);
#endif
        HR(hr, "D3D11CreateDevice on capture adapter");
        ComPtr<ID3D10Multithread> mt; if (SUCCEEDED(mDev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        HR(chosenOutput.As(&mOutput1), "QI IDXGIOutput1");
        chosenOutput.As(&mOutput5);
        mOutputLeft = chosen.DesktopCoordinates.left; mOutputTop = chosen.DesktopCoordinates.top;
        DXGI_ADAPTER_DESC1 adapter{}; HR(chosenAdapter->GetDesc1(&adapter), "Adapter description");
        char identity[256]{};
        std::snprintf(identity, sizeof(identity), "vendor=0x%04X device=0x%04X LUID=%08X:%08X output-origin=%ld,%ld",
            unsigned(adapter.VendorId), unsigned(adapter.DeviceId), unsigned(adapter.AdapterLuid.HighPart),
            unsigned(adapter.AdapterLuid.LowPart), chosen.DesktopCoordinates.left, chosen.DesktopCoordinates.top);
        mCaptureDeviceDetails = WideToUtf8(adapter.Description) + "; monitor=" + WideToUtf8(chosen.DeviceName) + "; " + identity;
    }

void App::EnsureCaptureTexture()
{
        const UINT width = mCaptureRegion.width;
        const UINT height = mCaptureRegion.height;
        if (!width || !height) return;
        if (mFrameTex && mFrameSRV && mFrameW == width && mFrameH == height) return;

        mFrameSRV.Reset();
        mFrameTex.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width; td.Height = height; td.MipLevels = td.ArraySize = 1;
        td.Format = mDuplFormat; td.SampleDesc = { 1,0 };
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HR(mDev->CreateTexture2D(&td, nullptr, &mFrameTex), "Create capture texture");
        HR(mDev->CreateShaderResourceView(mFrameTex.Get(), nullptr, &mFrameSRV),
            "Create capture SRV");
        mFrameW = width;
        mFrameH = height;
        mFrameNeedsFullCopy = true;
    }

HRESULT App::CreateDuplication()
{
        if (mOutput5) {
            // Keep BGRA8 until preprocessing supports native HDR tone mapping.
            const DXGI_FORMAT formats[] = { DXGI_FORMAT_B8G8R8A8_UNORM };
            const HRESULT hr = mOutput5->DuplicateOutput1(mDev.Get(), 0,
                _countof(formats), formats, &mDupl);
            if (SUCCEEDED(hr) || hr == E_ACCESSDENIED) return hr;
        }
        return mOutput1->DuplicateOutput(mDev.Get(), &mDupl);
    }

void App::ReleaseHeldFrame() noexcept
{
        if (mFrameHeld && mDupl) mDupl->ReleaseFrame();
        mFrameHeld = false;
    }

void App::StartDuplication()
{
        ReleaseHeldFrame();
        mDesktopSrvs.Reset();
        mDupl.Reset();
        DXGI_OUTPUT_DESC output{}; HR(mOutput1->GetDesc(&output), "Current output description");
        if (!output.AttachedToDesktop ||
            (output.Rotation != DXGI_MODE_ROTATION_IDENTITY && output.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED))
            throw std::runtime_error("Capture output changed/rotated; select a landscape monitor and use F8 recovery");
        mOutputLeft = output.DesktopCoordinates.left; mOutputTop = output.DesktopCoordinates.top;
        const HRESULT hr = CreateDuplication();
        if (hr == E_ACCESSDENIED) { mCaptureError=std::uint32_t(hr);Sleep(50); return; }
        mCaptureError=SUCCEEDED(hr)?0:std::uint32_t(hr);
        HR(hr, "DuplicateOutput");

        DXGI_OUTDUPL_DESC d{}; mDupl->GetDesc(&d);
        if (d.ModeDesc.Width != mDeskW || d.ModeDesc.Height != mDeskH
            || d.ModeDesc.Format != mDuplFormat)
        {
            mDeskW = d.ModeDesc.Width; mDeskH = d.ModeDesc.Height;
            mDuplFormat = d.ModeDesc.Format;
            mFrameSRV.Reset(); mFrameTex.Reset();
            mFrameW = mFrameH = 0;
        }
        // Staraya tekstura bolshe nedeystvitelna - sbrasyvaem keshirovannyy SRV.
        mDirectCapture = false;
        // A same-resolution duplication restart is still a new image history.
        // Dirty rectangles cannot repair a stale ROI without a fresh base copy.
        mFrameNeedsFullCopy = true;
        mDirectSrvSupported = true;
        RefreshCaptureRegion();
        EnsureCaptureTexture();
    }

UINT App::CaptureTimeoutMs() const
{
        // DXGI waits cannot be cancelled by an inference completion event.
        // Keep active detection event-driven; avoid idle capture polling.
        return mDetectEnabled && mDet.Ready() ? 0u : 8u;
    }

ID3D11ShaderResourceView* App::DesktopSrv(ID3D11Texture2D* src)
{
        return mDesktopSrvs.Get(mDev.Get(), src, mDirectSrvSupported);
    }

#ifdef AIM_ENABLE_WGC
bool App::CaptureWgcFrame()
{
        const double begin=NowMs();
        // Leave the newest frame in the mailbox while all processing slots are
        // occupied. The producer can replace it; no stale active frame is reused.
        if(mDetectEnabled && mDet.Ready() && !HasPreprocessCapacity()) {
            mCaptureMs.store(NowMs()-begin,std::memory_order_relaxed);return false;
        }
        auto frame=mWgc.Take();
        if(!frame){mCaptureMs.store(NowMs()-begin,std::memory_order_relaxed);return false;}
        ++mCapturedFrameId;mCapturedFrameMs=frame->receivedMs;mAcquiredFrameMs=frame->receivedMs;mAccumulatedFrames=1;mPresentedFrameMs=frame->presentedMs;
        auto* src=frame->texture.Get();
        D3D11_TEXTURE2D_DESC sourceDesc{};src->GetDesc(&sourceDesc);
        if(sourceDesc.Width!=mDeskW||sourceDesc.Height!=mDeskH){mDeskW=sourceDesc.Width;mDeskH=sourceDesc.Height;RefreshCaptureRegion();EnsureCaptureTexture();}
        mFrames.Observe(mDev.Get(),mCtx.Get(),src,sourceDesc.Width,sourceDesc.Height,mCaptureRegion.left,mCaptureRegion.top,mCaptureRegion.width,mCaptureRegion.height,mCapturedFrameId,mCapturedFrameMs,mAim.Config().extra,IsKeyHeld(mAim.Config().activationKey),WideToUtf8(FileNameOf(mDet.ModelPath())),mAim.Config().confidence);
        auto* direct=DesktopSrv(src);
        if(direct) {
            mDirectCapture=true;
            if(mDetectEnabled && mDet.Ready() && WantsPreprocess())
                SubmitPreprocess(direct,mDeskW,mDeskH,mCaptureRegion,mCaptureRegion.left,mCaptureRegion.top);
            if(mDetectEnabled&&mLastPreprocessFrameId!=mCapturedFrameId) {
                EnsureCaptureTexture();CopyRegionFull(src);mFrameNeedsFullCopy=false;mDirectCapture=false;
            }
        } else {
            mDirectCapture=false;EnsureCaptureTexture();CopyRegionFull(src);mFrameNeedsFullCopy=false;
        }
        // The owning WGC frame remains alive until all texture operations are issued.
        mCaptureMs.store(NowMs()-begin,std::memory_order_relaxed);return true;
    }
#endif

bool App::CaptureFrame(UINT ms)
{
#ifdef AIM_ENABLE_WGC
        if(mUseWgc)return CaptureWgcFrame();
#endif
        const double captureStart = NowMs();
        auto finishCapture = [this, captureStart]() {
            mCaptureMs.store(NowMs() - captureStart, std::memory_order_relaxed);
            };
        if (!mDupl) { StartDuplication(); finishCapture(); return false; }
        // Keep ownership through GPU submission and release just before acquisition.
        ReleaseHeldFrame();
        ComPtr<IDXGIResource> res; DXGI_OUTDUPL_FRAME_INFO info{};
        HRESULT hr = mDupl->AcquireNextFrame(ms, &info, &res);
        const double acquiredAt=PerfNowMs();
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            mDxgiTimeouts.fetch_add(1, std::memory_order_relaxed);
            finishCapture();
            return false;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL)
        {
            // Aimmy posle 5 podryad sboev peresozdaet duplikaciyu - delaem tak zhe.
            if (++mCapFailures >= kMaxCaptureFailures) mCapFailures = 0;
            StartDuplication();
            finishCapture();
            return false;
        }
        mCapFailures = 0;
        HR(hr, "AcquireNextFrame");
        mFrameHeld = true;
        bool upd = false;
        if (info.LastPresentTime.QuadPart != 0)
        {
            if (info.AccumulatedFrames > 1)
                mDxgiMissedFrames.fetch_add(info.AccumulatedFrames - 1, std::memory_order_relaxed);
            ComPtr<ID3D11Texture2D> src;
            if (SUCCEEDED(res.As(&src)))
            {
                ++mCapturedFrameId;
                mAcquiredFrameMs=acquiredAt; mAccumulatedFrames=info.AccumulatedFrames;
                // Timestamp when this process obtained the desktop frame.
                // Display/scanout latency before acquisition is not included.
                mCapturedFrameMs = NowMs();
                static const double qpcToMs=[] {
                    LARGE_INTEGER f{};QueryPerformanceFrequency(&f);
                    return f.QuadPart>0 ? 1000.0/double(f.QuadPart) : 0.0;
                }();
                mPresentedFrameMs=double(info.LastPresentTime.QuadPart)*qpcToMs;
                mFrames.Observe(mDev.Get(),mCtx.Get(),src.Get(),mDeskW,mDeskH,mCaptureRegion.left,mCaptureRegion.top,mCaptureRegion.width,mCaptureRegion.height,mCapturedFrameId,mCapturedFrameMs,mAim.Config().extra,IsKeyHeld(mAim.Config().activationKey),WideToUtf8(FileNameOf(mDet.ModelPath())),mAim.Config().confidence);
                ID3D11ShaderResourceView* direct = DesktopSrv(src.Get());
                if (direct)
                {
                    // Zero-copy: shader sam vyrezaet oblast FOV iz kadra.
                    // Delaem eto do ReleaseFrame - posle nego tekstura nevalidna.
                    mDirectCapture = true;
                    if (mDetectEnabled && mDet.Ready() && WantsPreprocess())
                    {
                        SubmitPreprocess(direct, mDeskW, mDeskH, mCaptureRegion,
                            mCaptureRegion.left, mCaptureRegion.top);
                    }
                    if(mDetectEnabled && mLastPreprocessFrameId!=mCapturedFrameId) {
                        EnsureCaptureTexture();CopyRegionFull(src.Get());
                        mFrameNeedsFullCopy=false;mDirectCapture=false;
                    }
                }
                else
                {
                    mDirectCapture = false;
                    EnsureCaptureTexture();
                    if (mWorkerCaptureMethod == CaptureMethod::DirectXFull || mFrameNeedsFullCopy)
                    {
                        CopyRegionFull(src.Get());
                        mFrameNeedsFullCopy = false;
                    }
                    else
                    {
                        CopyDirty(info, src.Get());
                    }
                }
                upd = true;
            }
        }
        finishCapture();
        return upd;
    }

void App::CopyDirty(const DXGI_OUTDUPL_FRAME_INFO& info, ID3D11Texture2D* src)
{
        UINT metaSize = info.TotalMetadataBufferSize;
        bool full = (metaSize == 0);
        static std::vector<uint8_t> meta;
        if (!full)
        {
            if (meta.size() < metaSize) meta.resize(metaSize);
            UINT mb = 0, db = 0;
            if (FAILED(mDupl->GetFrameMoveRects(metaSize,
                reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta.data()), &mb))) full = true;
            if (!full && mb > 0) full = true;
            if (!full)
            {
                if (FAILED(mDupl->GetFrameDirtyRects(metaSize,
                    reinterpret_cast<RECT*>(meta.data()), &db))) full = true;
                else
                {
                    const RECT* rects = reinterpret_cast<const RECT*>(meta.data());
                    for (UINT i = 0; i < db / sizeof(RECT); ++i)
                    {
                        // Kopiruem tolko peresechenie s oblastyu zakhvata.
                        const LONG cl = mCaptureRegion.left;
                        const LONG ct = mCaptureRegion.top;
                        const LONG cr = cl + LONG(mCaptureRegion.width);
                        const LONG cb = ct + LONG(mCaptureRegion.height);
                        const LONG l = std::max<LONG>(rects[i].left, cl);
                        const LONG t = std::max<LONG>(rects[i].top, ct);
                        const LONG r = std::min<LONG>(rects[i].right, cr);
                        const LONG b = std::min<LONG>(rects[i].bottom, cb);
                        if (r <= l || b <= t) continue;
                        D3D11_BOX box{ UINT(l),UINT(t),0,UINT(r),UINT(b),1 };
                        mCtx->CopySubresourceRegion(mFrameTex.Get(), 0,
                            UINT(l - cl), UINT(t - ct), 0, src, 0, &box);
                    }
                }
            }
        }
        if (full) CopyRegionFull(src);
    }

void App::CopyRegionFull(ID3D11Texture2D* src)
{
        const LONG left = std::max<LONG>(mCaptureRegion.left, 0);
        const LONG top = std::max<LONG>(mCaptureRegion.top, 0);
        const LONG right = std::min<LONG>(mCaptureRegion.left + LONG(mCaptureRegion.width), LONG(mDeskW));
        const LONG bottom = std::min<LONG>(mCaptureRegion.top + LONG(mCaptureRegion.height), LONG(mDeskH));
        if (right <= left || bottom <= top) return;
        const D3D11_BOX box{ UINT(left), UINT(top), 0, UINT(right), UINT(bottom), 1 };
        mCtx->CopySubresourceRegion(mFrameTex.Get(), 0,
            UINT(left - mCaptureRegion.left), UINT(top - mCaptureRegion.top), 0, src, 0, &box);
    }

void App::RefreshCaptureRegion(){
    const int mode=int(mWorkerCaptureMethod);
    if(mode==0||mode==3)mCaptureRegion=MakeCenteredRegion(mDeskW,mDeskH,mAim.Config().radius);
    else mCaptureRegion=CaptureRegion{0,0,mDeskW.load(),mDeskH.load()};
    mFrameNeedsFullCopy=true;
}
void App::StartCaptureMode(){
    mCaptureError=0;
    ReleaseHeldFrame();mDesktopSrvs.Reset();mDupl.Reset();mDirectCapture=false;mFrames.ResetCapture();
#ifdef AIM_ENABLE_WGC
    mWgc.Stop();mUseWgc=int(mWorkerCaptureMethod)>=2;
    if(mUseWgc){DXGI_OUTPUT_DESC output{};HR(mOutput1->GetDesc(&output),"WGC monitor");HWND window=nullptr;
        auto title=mAim.Config().extra.windowTitle;
        if(!title.empty()){struct Search{const std::wstring*title;HWND found;};Search search{&title,nullptr};EnumWindows([](HWND h,LPARAM data)->BOOL{auto*s=reinterpret_cast<Search*>(data);wchar_t text[1024]{};GetWindowTextW(h,text,1024);DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(pid!=GetCurrentProcessId()&&IsWindowVisible(h)&&std::wstring(text).find(*s->title)!=std::wstring::npos){s->found=h;return FALSE;}return TRUE;},LPARAM(&search));window=search.found;if(!window)throw std::runtime_error("Selected WGC window not found");RECT rect{};GetWindowRect(window,&rect);mOutputLeft=rect.left;mOutputTop=rect.top;}
        else{mOutputLeft=output.DesktopCoordinates.left;mOutputTop=output.DesktopCoordinates.top;}
        mWgc.Start(mDev.Get(),output.Monitor,mDet.ActivityEvent(),window);mDeskW=UINT(mWgc.Width());mDeskH=UINT(mWgc.Height());RefreshCaptureRegion();EnsureCaptureTexture();return;
    }
#else
    if(int(mWorkerCaptureMethod)>=2)throw std::runtime_error("WGC is unavailable in this build");
#endif
    StartDuplication();
}
