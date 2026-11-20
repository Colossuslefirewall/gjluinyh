from pathlib import Path
C=Path('/tmp/albion-reference/candidate')
def edit(n,a,b):
 p=C/n;s=p.read_text();assert s.count(a)==1,(n,a[:80],s.count(a));p.write_text(s.replace(a,b))
def put(n,s):
 p=C/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s)
put('output_contract.hpp',r'''#pragma once
#include <algorithm>
#include <atomic>
namespace albion::output {
enum class Status { Rejected, Submitted, Partial, Unverified, Queued };
struct Receipt { int x=0,y=0; Status status=Status::Rejected; };
template<class Send> Receipt SubmitChunks(int dx,int dy,Send send) {
    Receipt r;
    while(dx||dy){
        const int x=std::clamp(dx,-127,127),y=std::clamp(dy,-127,127);
        if(!send(x,y)){r.status=(r.x||r.y)?Status::Partial:Status::Rejected;return r;}
        r.x+=x;r.y+=y;dx-=x;dy-=y;
    }
    r.status=Status::Submitted;return r;
}
struct Counters {
    std::atomic<unsigned long long> attempted{0},submitted{0},partial{0},rejected{0},unverified{0},queued{0};
    void Record(const Receipt&r) noexcept {
        attempted.fetch_add(1,std::memory_order_relaxed);
        auto* count=&rejected;
        switch(r.status){
        case Status::Submitted:count=&submitted;break;
        case Status::Partial:count=&partial;break;
        case Status::Unverified:count=&unverified;break;
        case Status::Queued:count=&queued;break;
        default:break;
        }
        count->fetch_add(1,std::memory_order_relaxed);
    }
};
inline Counters counters;
}
''')
put('modules/core/comparison_support.hpp',r'''#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace albion::comparison {
inline double ObservationMs(double captured,double presented,double consumed) noexcept {
    if(!std::isfinite(captured)||!std::isfinite(consumed)||captured<=0||captured>consumed)return -1;
    return std::isfinite(presented)&&presented>0&&presented<=captured?presented:captured;
}
inline bool Fresh(double observed,double consumed,double maxAge=100) noexcept {
    return std::isfinite(observed)&&std::isfinite(consumed)&&observed>0&&consumed>=observed&&consumed-observed<=maxAge;
}
inline const char* CaptureState(std::uint32_t error,bool failed,double last,double now) noexcept {
    if(error==0x80070005u)return "access denied";
    if(failed)return "failed";
    return Fresh(last,now,250)?"ready":"waiting/stale";
}
inline const char* ModeName(int mode) noexcept {
    constexpr const char* names[]={"Stable legacy","Lab EMA","Lab Kalman","Lab One Euro","Lab SunOne"};
    return mode>=0&&mode<5?names[mode]:"invalid";
}
}
''')
edit('basic_winapi_aim.hpp','#include "modules/tracking/comparison_core.hpp"','#include "modules/tracking/comparison_core.hpp"\n#include "output_contract.hpp"')
edit('basic_winapi_aim.hpp','inline void MoveMouseRelativeWinApi(LONG dx, LONG dy) noexcept {','inline bool MoveMouseRelativeWinApi(LONG dx, LONG dy) noexcept {')
edit('basic_winapi_aim.hpp','    if (dx == 0 && dy == 0) {\n        return;\n    }','    if (dx == 0 && dy == 0) {\n        return true;\n    }')
edit('basic_winapi_aim.hpp','    SendInput(1, &input, sizeof(input));','    return SendInput(1, &input, sizeof(input))==1;')
edit('basic_winapi_aim.hpp','inline std::atomic<MouseMoveSink> gMouseMoveSink{nullptr};','inline std::atomic<MouseMoveSink> gMouseMoveSink{nullptr};\nusing MouseReceiptSink = albion::output::Receipt (*)(LONG,LONG);\ninline std::atomic<MouseReceiptSink> gMouseReceiptSink{nullptr};')
p=C/'basic_winapi_aim.hpp';s=p.read_text();a=s.index('inline void SendMouseMove(');b=s.index('\n\n\nstruct AlbionOutputBatch',a)
s=s[:a]+r'''inline albion::output::Receipt SendMouseMove(LONG dx,LONG dy) noexcept {
    using namespace albion::output;
    if(gBatchOutput){
        gBatchX=LONG(std::clamp<long long>(static_cast<long long>(gBatchX)+dx,-32767,32767));
        gBatchY=LONG(std::clamp<long long>(static_cast<long long>(gBatchY)+dy,-32767,32767));
        return {0,0,Status::Queued};
    }
    Receipt result;
    try {
        if(auto sink=gMouseMoveSink.load(std::memory_order_acquire)){
            sink(dx,dy);result.status=Status::Unverified;
        }else if(auto receipt=gMouseReceiptSink.load(std::memory_order_acquire)){
            result=receipt(dx,dy);
        }else if(MoveMouseRelativeWinApi(dx,dy)){
            result={int(dx),int(dy),Status::Submitted};
        }
    }catch(...){result={};}
    counters.Record(result);
    return result;
}'''+s[b:];p.write_text(s)
edit('arduino_hid.hpp','#pragma once','#pragma once\n#include "output_contract.hpp"')
p=C/'arduino_hid.hpp';s=p.read_text();a=s.index('    bool MoveRelative(int dx, int dy)');b=s.index('    bool Click(',a)
s=s[:a]+r'''    albion::output::Receipt MoveRelativeReceipt(int dx,int dy) {
        if(!Connected()){RequestReconnect();gDevHid.dropped.fetch_add(1);return {};}
        auto r=albion::output::SubmitChunks(dx,dy,[this](int x,int y){return SendCommand(0,x,y);});
        if(r.status!=albion::output::Status::Submitted)gDevHid.dropped.fetch_add(1);
        return r;
    }
    bool MoveRelative(int dx,int dy) {
        return MoveRelativeReceipt(dx,dy).status==albion::output::Status::Submitted;
    }
'''+s[b:];p.write_text(s)
edit('modules/app/app.hpp','static void ArduinoMouseSink(LONG dx, LONG dy);','static albion::output::Receipt ArduinoMouseSink(LONG dx, LONG dy);')
edit('modules/app/commands.cpp','void App::ArduinoMouseSink(LONG dx, LONG dy)','albion::output::Receipt App::ArduinoMouseSink(LONG dx, LONG dy)')
edit('modules/app/commands.cpp','if (auto* sink = sArduinoSink.load(std::memory_order_acquire)) sink->MoveRelative(int(dx), int(dy));','if (auto* sink = sArduinoSink.load(std::memory_order_acquire)) return sink->MoveRelativeReceipt(int(dx), int(dy));\n        return {};')
for f in ('modules/app/commands.cpp','modules/app/lifecycle.cpp'):
 p=C/f;s=p.read_text().replace('gMouseMoveSink.store(', 'gMouseReceiptSink.store(');p.write_text(s)
t='modules/tracking/tracking.cpp';p=C/t;s=p.read_text()
a=s.index('            if (moveX != 0 || moveY != 0) {');b=s.index('\n        }\n\n        // Keep dispatch',a)
s=s[:a]+'            submittedScale = px;'+s[b:]
# The scale belongs to the computed command, not a concurrently published state.
at=s.index('void AimMover::Tick') if 'void AimMover::Tick' in s else s.index('        LONG moveX')
a=s.index('LONG moveX',at);s=s[:a]+'float submittedScale=1.0f;\n        '+s[a:]
old='            SendMouseMove(moveX, moveY);'
assert s.count(old)==2
s=s.replace(old,r'''            const auto sent=SendMouseMove(moveX,moveY);
            const double submittedAt=NowSeconds();
            std::lock_guard<std::mutex> lock(mMx);
            const float sx=float(sent.x)*submittedScale,sy=float(sent.y)*submittedScale;
            if(sent.x||sent.y){
                mCountsX+=float(sent.x);mCountsY+=float(sent.y);
                mSends.push_back(SendRecord{submittedAt,sx,sy});
                TrimSendsLocked(std::max(mState.captureTime,submittedAt-.5));
            }
            if(sent.status!=albion::output::Status::Submitted)mResidualX=mResidualY=0;''',1)
s=s.replace('        mSentX = float(moveX);\n        mSentY = float(moveY);\n        mHasSent = (moveX != 0 || moveY != 0);','        mSentX = mSentY = 0;\n        mHasSent = false;')
s=s.replace(old,'''            const auto sent=SendMouseMove(moveX,moveY);
            mSentX=float(sent.x);mSentY=float(sent.y);mHasSent=sent.x||sent.y;
            if(sent.status!=albion::output::Status::Submitted)mResidualX=mResidualY=0;''',1)
p.write_text(s)
edit('modules/app/app.hpp','    bool mCaptureFailed = false;','    std::uint32_t mCaptureError=0;\n    double mTrackingOutputMs=0;\n    bool mCaptureFailed = false;')
edit('modules/app/app.hpp','        std::string captureDevice;','        std::string captureDevice;\n        std::string comparisonText;\n        bool captureReady=false;\n        double observationAgeMs=-1,trackingOutputMs=0;')
edit('modules/capture/capture.cpp','if (hr == E_ACCESSDENIED) { Sleep(50); return; }','if (hr == E_ACCESSDENIED) { mCaptureError=std::uint32_t(hr);Sleep(50); return; }\n        mCaptureError=SUCCEEDED(hr)?0:std::uint32_t(hr);')
edit('modules/pipeline/pipeline.cpp','#include "modules/app/app.hpp"','#include "modules/app/app.hpp"\n#include "modules/core/comparison_support.hpp"')
edit('modules/pipeline/pipeline.cpp','        const double ageMs = mLatestTiming.capturedMs > 0.0\n            ? std::max(0.0, now - mLatestTiming.capturedMs) : 0.0;','        const double observedMs=albion::comparison::ObservationMs(mLatestTiming.capturedMs,mLatestTiming.presentedMs,now);\n        const double controllerMs=mAim.Config().extra.comparisonMode?observedMs:mLatestTiming.capturedMs;\n        const double ageMs=controllerMs>0?std::max(0.0,now-controllerMs):0.0;')
edit('modules/pipeline/pipeline.cpp','        mAimTarget = mAim.Update(mDets, mCaptureRegion, allowed, nd,','        const double trackingBegin=NowMs();\n        const bool fresh=!mAim.Config().extra.comparisonMode||albion::comparison::Fresh(controllerMs,now);\n        mAimTarget = mAim.Update(mDets, mCaptureRegion, allowed&&fresh, nd,')
edit('modules/pipeline/pipeline.cpp','            mLatestTiming.capturedMs > 0.0 ? mLatestTiming.capturedMs * 0.001 : -1.0,','            controllerMs>0 ? controllerMs*.001 : -1.0,')
edit('modules/pipeline/pipeline.cpp','            traceNow ? &trace : nullptr);','            traceNow ? &trace : nullptr);\n        mTrackingOutputMs=NowMs()-trackingBegin;')
edit('modules/pipeline/pipeline.cpp','        snapshot.captureDevice = mCaptureDeviceDetails;','''        snapshot.captureDevice = mCaptureDeviceDetails;
        const double observation=albion::comparison::ObservationMs(mLatestTiming.capturedMs,mLatestTiming.presentedMs,snapshotNow);
        snapshot.observationAgeMs=observation>0?snapshotNow-observation:-1;
        snapshot.trackingOutputMs=mTrackingOutputMs;
        const auto captureState=albion::comparison::CaptureState(mCaptureError,mCaptureFailed,mCapturedFrameMs,snapshotNow);
        snapshot.captureReady=mCaptureError==0&&!mCaptureFailed&&albion::comparison::Fresh(mCapturedFrameMs,snapshotNow,250);
        const auto& extra=mAim.Config().extra;
        char detail[360]{};
        const double horizon=extra.prediction&&snapshot.observationAgeMs>=0?
            std::clamp(snapshot.observationAgeMs+extra.predictionMs,0.0,extra.comparisonMode?100.0:300.0):0;
        std::snprintf(detail,sizeof(detail),\"Applied: %s | %s | age %.1f ms | horizon <= %.1f ms\\\\nCapture: %s | model: %s | track+output %.3f ms\",
            albion::comparison::ModeName(extra.comparisonMode),extra.comparisonMode?mAim.ComparisonState():\"legacy state\",
            snapshot.observationAgeMs,horizon,captureState,snapshot.ready?\"ready\":\"not ready\",mTrackingOutputMs);
        snapshot.comparisonText=detail;''')
edit('modules/ui/controls.cpp','void App::ShowUpgrade() {\n    ApplyTab(4);','void App::ShowUpgrade() {\n    ApplyTab(4);\n    mUpgradePanel.SetComparisonStatus([this]{std::lock_guard<std::mutex> lock(mUiMx);return mUiSnap.comparisonText.empty()?std::wstring(L\"Experimental 20261120 | capture/model: not running\"):Utf8ToWide(mUiSnap.comparisonText);});')
edit('config_document.hpp',' (expected 1)',' (expected 1 or 2)')
print('Stage 2 generated')

