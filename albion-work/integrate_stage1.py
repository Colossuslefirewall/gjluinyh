from pathlib import Path
import shutil
R=Path('/tmp/albion-reference'); S=R/'stable'; L=R/'lab'; C=R/'candidate'
shutil.copytree(S,C,dirs_exist_ok=True)
def read(p): return p.read_text()
def put(p,s):
 p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s)
def edit(name,old,new,count=1):
 p=C/name;s=read(p);assert s.count(old)==count,(name,old[:90],s.count(old));put(p,s.replace(old,new))
core='modules/tracking/upgrade_core.hpp'
edit(core,' bool tracker=false,',' int comparisonMode=0;\n bool tracker=false,')
lab=read(L/core);a=lab.index('struct Options {');b=lab.index('\nstruct AxisFilter',a)
lab=lab[:a]+'using Options = albion::upgrade::Options;'+lab[b:]
lab=lab.replace('namespace albion::upgrade {','namespace albion::comparison {').replace('#include "motion_estimator.hpp"','#include "upgrade_core.hpp"\n#include "motion_estimator.hpp"')
lab=lab.replace('std::vector<Track> tracks;int next=1;','std::vector<Track> tracks;int next=1;double lastUpdate=-1;')
lab=lab.replace('void Reset(){tracks.clear();next=1;}','void Reset(){tracks.clear();next=1;lastUpdate=-1;}')
lab=lab.replace('if(!std::isfinite(now)){Reset();return;}','if(!std::isfinite(now)){Reset();return;}\n  if(now<=lastUpdate)return;\n  lastUpdate=now;')
lab=lab.replace('const Options&o,int bodyClass){','const Options&o,int bodyClass,float minConfidence=0){').replace('std::isfinite(d.score)&&','std::isfinite(d.score)&&d.score>=minConfidence&&')
# Only Motion2D gets a selected-mode argument.
import re
lab=re.sub(r'(t\.motion\.Observe\([^;]+)(\);)',r'\1,std::clamp(o.comparisonMode-1,0,3)\2',lab)
put(C/'modules/tracking/comparison_core.hpp',lab)
motion=read(L/'modules/tracking/motion_estimator.hpp').replace('if(ix*vx_<0)vx_=0;if(iy*vy_<0)vy_=0;','if(ix*vx_<0){vx_=0;}\n        if(iy*vy_<0){vy_=0;}')
put(C/'modules/tracking/lab_motion_reference.hpp',motion.replace('namespace albion::motion {','namespace albion::motion_reference {'))
motion=motion.replace('unsigned samples_=0; bool ready_=false;','unsigned samples_=0; bool ready_=false; int activeMode_=-1;')
motion=motion.replace('double q=300,double r=9){','double q=300,double r=9,int activeMode=-1){')
motion=motion.replace('        if(ready_ && now<=time_)','        if(activeMode < -1 || activeMode > 3){Reset();return false;}\n        if(activeMode!=activeMode_){Reset();activeMode_=activeMode;}\n        if(ready_ && now<=time_)')
motion=motion.replace('Reset();ready_=true;','Reset();activeMode_=activeMode;ready_=true;')
motion=motion.replace('kx_.Step(x,.016,q,r);ky_.Step(y,.016,q,r);','if(activeMode_<0||activeMode_==1){kx_.Step(x,.016,q,r);ky_.Step(y,.016,q,r);}')
motion=motion.replace('ex_.Step(x,.016);ey_.Step(y,.016);reference_.update(x,y,.016,0);return true;','if(activeMode_<0||activeMode_==2){ex_.Step(x,.016);ey_.Step(y,.016);}\n            if(activeMode_<0||activeMode_==3){reference_.update(x,y,.016,0);}return true;')
motion=motion.replace('kx_.Step(x,dt,q,r);ky_.Step(y,dt,q,r);','if(activeMode_<0||activeMode_==1){kx_.Step(x,dt,q,r);ky_.Step(y,dt,q,r);}')
motion=motion.replace('ex_.Step(x,dt);ey_.Step(y,dt);reference_.update(x,y,dt,0);','if(activeMode_<0||activeMode_==2){ex_.Step(x,dt);ey_.Step(y,dt);}\n        if(activeMode_<0||activeMode_==3){reference_.update(x,y,dt,0);}')
put(C/'modules/tracking/motion_estimator.hpp',motion)
put(C/'third_party/sunone/aim_kalman.h',read(L/'third_party/sunone/aim_kalman.h'))
edit('basic_winapi_aim.hpp','#include "modules/tracking/upgrade_core.hpp"','#include "modules/tracking/upgrade_core.hpp"\n#include "modules/tracking/comparison_core.hpp"')
edit('basic_winapi_aim.hpp','std::optional<AimTarget> SelectAdvanced(','std::optional<AimTarget> SelectComparison(const std::vector<Detection>&,const CaptureRegion&,double);\n    std::optional<AimTarget> SelectAdvanced(')
edit('basic_winapi_aim.hpp','albion::upgrade::Tracker mAdvancedTracker;','albion::upgrade::Tracker mAdvancedTracker;\n    albion::comparison::Tracker mComparisonTracker;\n    const char* mComparisonState="idle";')
edit('basic_winapi_aim.hpp','    void Reset() noexcept;','    const char* ComparisonState() const noexcept { return mComparisonState; }\n    void Reset() noexcept;')
t='modules/tracking/tracking.cpp'
edit(t,'mAdvancedTracker.Reset();mAdvancedTrackId=0;','mAdvancedTracker.Reset();mComparisonTracker.Reset();mAdvancedTrackId=0;mComparisonState="idle";')
edit(t,'if(mConfig.extra.tracker||mConfig.extra.kalman||mConfig.extra.prediction||mConfig.extra.headBody)','if(mConfig.extra.comparisonMode!=0||mConfig.extra.tracker||mConfig.extra.kalman||mConfig.extra.prediction||mConfig.extra.headBody)')
edit(t,'    const auto&o=mConfig.extra;mAdvancedTracker.Update(detections,observed,o,mConfig.targetClass);','    if(mConfig.extra.comparisonMode!=0)return SelectComparison(detections,region,observed);\n    const auto&o=mConfig.extra;mAdvancedTracker.Update(detections,observed,o,mConfig.targetClass);')
advanced=read(L/t);a=advanced.index('        // Advanced estimators already own');b=advanced.index('        // Hard mode',a)
block=advanced[a:b].replace('mConfig.extra.motionMode!=0||mConfig.extra.tracker||\n            mConfig.extra.kalman||mConfig.extra.prediction||mConfig.extra.headBody','mConfig.extra.comparisonMode!=0').replace('std::clamp(mConfig.extra.motionMode==0&&mConfig.extra.kalman?1:mConfig.extra.motionMode,0,3)','std::clamp(mConfig.extra.comparisonMode-1,0,3)').replace('mAdvancedTracker.Tracks()','mComparisonTracker.Tracks()')
edit(t,'        // Hard mode uses',block+'        // Hard mode uses')
edit(t,'            mLastLeadPixels = mMover.LeadPixels();','            if(!advancedMotion)mLastLeadPixels = mMover.LeadPixels();')
fn=advanced[advanced.index('std::optional<AimTarget> BasicWinApiAim::SelectAdvanced('):].replace('::SelectAdvanced(','::SelectComparison(')
a=fn.index('    std::vector<Detection> eligible;');b=fn.index('    const albion::upgrade::Track*',a)
fn=fn[:a]+'    mComparisonTracker.Update(detections,observed,o,mConfig.targetClass,mConfig.confidence);mLastLeadPixels=0;\n'+fn[b:]
fn=fn.replace('albion::upgrade::Track','albion::comparison::Track').replace('mAdvancedTracker.Tracks()','mComparisonTracker.Tracks()')
fn=fn.replace('if(!best)return std::nullopt;','if(!best){mComparisonState=mComparisonTracker.Tracks().empty()?"lost":"temporarily unobserved";return std::nullopt;}')
fn=fn.replace('    if(mAdvancedTrackId!=best->id){','    const bool switched=mAdvancedTrackId!=0&&mAdvancedTrackId!=best->id;\n    mComparisonState=best->motion.ReadyForLead()?"tracking":switched?"reacquiring":"acquiring";\n    if(mAdvancedTrackId!=best->id){')
fn=fn.replace('std::clamp(o.motionMode==0&&o.kalman?1:o.motionMode,0,3)','std::clamp(o.comparisonMode-1,0,3)').replace('o.maxLead,o.prediction,o.kalman||mode!=0,rawDistance','o.maxLead,o.prediction,mode!=0,rawDistance')
put(C/t,read(C/t)+'\n\n'+fn+'\n')
edit('config_document.hpp','{"version", 1, 100, true}','{"version", 1, 2, true}, {"u_comparisonMode",0,4,true}')
edit('config_document.hpp','if (Number("version", 0) != 1) return Fail("Unsupported config version");','if (Number("version", 0) != 1 && Number("version", 0) != 2) return Fail("Unsupported config version");\n        if(Number("version",0)==1 && values.count("u_comparisonMode")) return Fail("Comparison modes require version=2");\n        if(Number("version",0)==2 && !values.count("u_comparisonMode")) return Fail("Missing field: u_comparisonMode");')
edit('modules/config/configuration.cpp','std::string out = "version=1\\r\\n";','std::string out = "version=2\\r\\n";')
edit('modules/config/configuration.cpp','        const auto&o=c.extra;','        const auto&o=c.extra;\n        out+="u_comparisonMode="+std::to_string(o.comparisonMode)+"\\r\\n";')
edit('modules/config/configuration.cpp','        BasicAimConfig& c = mAimCfg;','        BasicAimConfig& c = mAimCfg;\n        c.extra.comparisonMode=int(document.Number("u_comparisonMode",0));')
ui='modules/ui/upgrade_panel.hpp'
edit(ui,'int row=0;','int row=0;HWND modeEdit=nullptr,kalmanEdit=nullptr,qEdit=nullptr,rEdit=nullptr;std::function<std::wstring()> comparisonStatus;')
edit(ui,'page=0;row=0;Bool(', 'page=0;row=0;Combo(L"Estimator / comparison",cfg.extra.comparisonMode,{L"Stable: legacy",L"Lab: guarded EMA",L"Lab: guarded Kalman",L"Lab: One Euro",L"Lab: SunOne (MIT)"});modeEdit=fields.back().edit;Bool(')
edit(ui,'Bool(L"Kalman: оценка положения и скорости",cfg.extra.kalman);','Bool(L"Legacy Kalman (Stable mode only)",cfg.extra.kalman);kalmanEdit=fields.back().edit;')
edit(ui,'Number(L"Шум процесса (ускорение)",cfg.extra.processNoise,.001,1000000);','Number(L"Kalman process noise (px²/s³)",cfg.extra.processNoise,.001,1000000);qEdit=fields.back().edit;')
edit(ui,'Number(L"Шум измерений (px²)",cfg.extra.measurementNoise,.001,100000);','Number(L"Kalman measurement noise (px²)",cfg.extra.measurementNoise,.001,100000);rEdit=fields.back().edit;')
edit(ui,'L"Изменения применяются к текущему профилю Albion"','L"Experimental 20261120 | schema 2 | isolated profiles"')
edit(ui,'  ChangePage(0);SetTimer','  RefreshApplicable();ChangePage(0);SetTimer')
helper=''' void RefreshApplicable(){
  const int mode=int(SendMessageW(modeEdit,CB_GETCURSEL,0,0));bool legacyKalman=false;
  for(const auto& c:checks)if(c.first==kalmanEdit)legacyKalman=c.second;
  for(auto&f:fields){bool enabled=true;if(f.edit==kalmanEdit)enabled=mode==0;if(f.edit==qEdit||f.edit==rEdit)enabled=mode==2||(mode==0&&legacyKalman);EnableWindow(f.edit,enabled);if(f.label)EnableWindow(f.label,enabled);}
 }
'''
edit(ui,' bool Commit(){',helper+' bool Commit(){')
edit(ui,'case WM_CREATE:self->Create();return 0;case WM_COMMAND:','case WM_CREATE:self->Create();return 0;case WM_COMMAND:\n if(reinterpret_cast<HWND>(l)==self->modeEdit&&HIWORD(w)==CBN_SELCHANGE){self->RefreshApplicable();return 0;}')
edit(ui,'check.second=!check.second;InvalidateRect','check.second=!check.second;self->RefreshApplicable();InvalidateRect')
edit(ui,'  if(frames){RECT status','  if(page==0&&comparisonStatus){RECT r{0,302,424,334};auto text=comparisonStatus();DrawTextW(dc,text.c_str(),-1,&r,DT_LEFT|DT_WORDBREAK);}\n  if(frames){RECT status')
edit(ui,'public:\n void Close()','public:\n void SetComparisonStatus(std::function<std::wstring()> f){comparisonStatus=std::move(f);}\n void Close()')
print('Stage 1 files generated')

