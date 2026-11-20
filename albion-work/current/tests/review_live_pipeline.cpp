// Real capture, preprocessing, inference and publication; all physical output is disabled.
#define ALBION_UPGRADE_TESTS 1
#include "modules/app/app.hpp"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <stdexcept>
static void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static double CpuSeconds(){FILETIME c{},e{},k{},u{};GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);ULARGE_INTEGER a{},b{};a.LowPart=k.dwLowDateTime;a.HighPart=k.dwHighDateTime;b.LowPart=u.dwLowDateTime;b.HighPart=u.dwHighDateTime;return double(a.QuadPart+b.QuadPart)/1e7;}
static albion::output::Receipt NoPhysicalOutput(LONG,LONG){return {0,0,albion::output::Status::Rejected};}
struct AlbionUiComponentCheck {
 static void Run(const std::wstring& model,const std::string& name) {
  App a;Gdiplus::GdiplusStartupInput startup;
  Check(Gdiplus::GdiplusStartup(&a.mGdiplusToken,&startup,nullptr)==Gdiplus::Ok,"GDI startup");
  a.mAimEnabled=false;a.mDetectEnabled=true;
  a.mAimCfg.extra.record=false;a.mAimCfg.extra.autoLabel=false;a.mAimCfg.extra.preview=false;a.mAimCfg.extra.recoil=false;
  a.mAimCfg.radius=320;a.mAimCfg.confidence=.5f;a.mAimCfg.extra.comparisonMode=0;
  gMouseReceiptSink.store(&NoPhysicalOutput);
  a.CreateDevice();a.CreateSettingsWindow(GetModuleHandleW(nullptr));
  a.mPre.Init(a.mDev.Get(),AimPerfEnvFlag(L"ALBION_GPU_TIMING"));
  a.mDet.EnableSharedInput(a.mDev.Get(),a.mPre.TensorBuffer(0),a.mPre.TensorBuffer(1));
  a.StartDuplication();a.CreateControls();a.LayoutControls();
#ifdef ALBION_DISCOVERY_PROFILE
  a.mStartupModelAllowed=false;
#endif
  a.mAim.Config()=a.mAimCfg;a.QueueSettings();a.mDet.Start(model);a.StartPipeline();
  const int x=(GetSystemMetrics(SM_CXSCREEN)-720)/2,y=(GetSystemMetrics(SM_CYSCREEN)-720)/2;
  HWND generated=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"STATIC",L"Albion generated frames — output disabled",WS_POPUP|WS_VISIBLE, x,y,720,720,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  Check(generated!=nullptr,"generated frame window");
  struct WindowLease {HWND h;~WindowLease(){DestroyWindow(h);}} windowLease{generated};
  unsigned tick=0;
  auto pump=[&] {
    MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message!=WM_QUIT){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    HDC dc=GetDC(generated);RECT r{0,0,720,720};HBRUSH brush=CreateSolidBrush(RGB(20+(tick%80),35,65));FillRect(dc,&r,brush);DeleteObject(brush);
    r={int((tick*7)%560),200,int((tick*7)%560)+100,400};brush=CreateSolidBrush(RGB(100,130,160));FillRect(dc,&r,brush);DeleteObject(brush);ReleaseDC(generated,dc);++tick;Sleep(8);
  };
  auto wait=[&](auto predicate,DWORD timeout,const char* label) {const auto end=GetTickCount64()+timeout;do{pump();if(predicate())return;const auto error=a.mDet.Error();if(!error.empty())throw std::runtime_error(error);}while(GetTickCount64()<end);throw std::runtime_error(label);};
  wait([&]{return a.mDet.Ready()&&a.mDet.CompletedCount()>10;},120000,"pipeline did not produce results");
  const auto warm=GetTickCount64()+2000;while(GetTickCount64()<warm)pump();
  std::cout<<"CASE="<<name<<" PID="<<GetCurrentProcessId()<<" backend="<<a.mDet.BackendDetails()<<" shared="<<a.mDet.SharedInput()<<" generated_desktop=1 physical_output=disabled gpu_timing="<<AimPerfEnvFlag(L"ALBION_GPU_TIMING")<<'\n'<<std::flush;
  Check(albion::performance::telemetry.StartBenchmark(),"start benchmark");
  const double cpuStart=CpuSeconds();double tracking=0;std::uint64_t samples=0;
  const auto begin=GetTickCount64();
  while(GetTickCount64()-begin<60500) {
    pump();std::lock_guard lock(a.mUiMx);tracking+=a.mUiSnap.trackingOutputMs;++samples;
  }
  const auto stats=albion::performance::telemetry.Read();
  Check(stats.finished&&stats.benchmark[albion::performance::InferenceFps].samples>100,"complete 60-second pipeline window");
  albion::performance::telemetry.ExportCompleted();
  std::cout<<"LIVE_PIPELINE_PASS case="<<name<<" cpu_seconds="<<CpuSeconds()-cpuStart<<" ui_sampled_tracking_output_ms="<<tracking/double(samples)<<" input_disabled=1\n";
  const char* metrics[]={"capture_fps","inference_fps","capture_wall_ms","backend_wall_ms","gpu_compute_ms","capture_to_result_ms","delivery_ms","decode_ms","queue_ms","objects"};
  for(size_t i=0;i<albion::performance::Count;++i){const auto& v=stats.benchmark[i];std::cout<<metrics[i]<<" mean="<<v.average<<" min="<<v.minimum<<" max="<<v.maximum<<" samples="<<v.samples<<'\n';}
#ifdef ALBION_DISCOVERY_PROFILE
  const auto original=std::filesystem::path(model),backup=std::filesystem::path(model+L".source-test-backup");
  Check(!std::filesystem::exists(backup),"source test does not overwrite backup");
  std::filesystem::rename(original,backup);
  struct Restore {std::filesystem::path original,backup;~Restore(){std::error_code ec;if(std::filesystem::exists(backup,ec)){std::filesystem::remove(original,ec);std::filesystem::rename(backup,original,ec);}}} restore{original,backup};
  const auto before=a.mDet.CompletedCount();
  wait([&]{a.PollFileDiscovery();return a.mModelDiscoveryText.find(L"source removed; loaded session retained")!=std::wstring::npos&&a.mDet.CompletedCount()>before+20;},10000,"loaded source removal visibility/continuity");
  std::filesystem::copy_file(backup,original);
  wait([&]{a.PollFileDiscovery();return a.mModelDiscoveryText.find(L"disk change detected; Reload")!=std::wstring::npos&&a.mDet.CompletedCount()>before+40;},10000,"replacement does not silently reload session");
  Check(a.mDet.Ready(),"loaded inference session retained after replacement");
  std::cout<<"LIVE_SOURCE_PASS removed_and_replaced_source_did_not_reload_or_stop_inference\n";
#endif
  a.StopPipeline();a.mDet.Stop();
 }
};
int wmain(int argc,wchar_t** argv){try{Check(argc==3,"usage: live-pipeline MODEL.engine CASE");winrt::init_apartment(winrt::apartment_type::multi_threaded);AlbionUiComponentCheck::Run(argv[1],Utf8FromWide(argv[2]));DevShutdownLog();winrt::uninit_apartment();return 0;}catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';DevShutdownLog();return 1;}}
