// Links the real App implementation; exercises only its original settings UI.
// Does not call App::Run, CreateDevice, StartPipeline, authentication, or HID.
#define ALBION_UPGRADE_TESTS 1
#include "modules/app/app.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <stdexcept>
static void Check(bool ok,const char*what){if(!ok)throw std::runtime_error(what);}
static int fakeCalls=0;
static albion::output::Receipt FakeSubmitted(LONG x,LONG y){++fakeCalls;return {int(x),int(y),albion::output::Status::Submitted};}
static albion::output::Receipt FakePartial(LONG x,LONG y){int chunks=0;++fakeCalls;return albion::output::SubmitChunks(int(x),int(y),[&](int,int){return ++chunks<2;});}
static void Pump(int ms){auto start=GetTickCount64();do{MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message!=WM_QUIT){TranslateMessage(&msg);DispatchMessageW(&msg);}}Sleep(5);}while(GetTickCount64()-start<ULONGLONG(ms));}
static void Save(HWND h,const std::wstring&file){SetForegroundWindow(h);RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);Pump(250);RECT r{};GetClientRect(h,&r);auto dc=GetDC(h),mem=CreateCompatibleDC(dc);auto bitmap=CreateCompatibleBitmap(dc,r.right,r.bottom);auto old=SelectObject(mem,bitmap);Check(BitBlt(mem,0,0,r.right,r.bottom,dc,0,0,SRCCOPY)!=FALSE,"BitBlt");CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};Gdiplus::Status status;{Gdiplus::Bitmap image(bitmap,nullptr);status=image.Save(file.c_str(),&png,nullptr);}SelectObject(mem,old);DeleteObject(bitmap);DeleteDC(mem);ReleaseDC(h,dc);Check(status==Gdiplus::Ok,"PNG save");}
struct AlbionUiComponentCheck{
 static void Run(){
  SetEnvironmentVariableW(L"ALBION_REDUCED_MOTION",L"1");
  Check(!gMouseMoveSink.load()&&!gMouseReceiptSink.load(),"test begins without physical sink");
  BasicWinApiAim aim;aim.mResidualX=.75f;aim.mResidualY=.25f;
  gMouseReceiptSink.store(&FakeSubmitted);
  {AlbionOutputBatch batch(3);auto pending=SendMouseMove(10,2);Check(pending.status==albion::output::Status::Queued,"batch is queued");aim.CompleteOutput(pending);Check(aim.mResidualX==.75f&&!aim.mHasSent,"queued preserves residual without claiming submission");auto sent=batch.Flush();aim.CompleteOutput(sent);Check(aim.mSentX==10&&aim.mSentY==5&&aim.mHasSent&&aim.mResidualX==.75f,"flush accounts submitted aim and recoil");batch.Flush();}Check(fakeCalls==1,"flush sends exactly once");
  gMouseReceiptSink.store(&FakePartial);
  {AlbionOutputBatch batch(0);aim.CompleteOutput(SendMouseMove(300,10));auto sent=batch.Flush();aim.CompleteOutput(sent);Check(sent.status==albion::output::Status::Partial&&aim.mSentX==127&&aim.mSentY==10&&aim.mResidualX==0&&aim.mResidualY==0,"partial flush accounts only accepted chunks");}Check(fakeCalls==2,"partial batch not retried by destructor");
  aim.mResidualX=.5f;aim.CompleteOutput({0,0,albion::output::Status::Rejected});Check(!aim.mHasSent&&aim.mResidualX==0,"rejection clears unsent state");
  aim.mResidualY=.5f;aim.CompleteOutput({0,0,albion::output::Status::Unverified});Check(!aim.mHasSent&&aim.mResidualY==0,"observer does not claim submitted counts");
  gMouseReceiptSink.store(nullptr);

  App a;Gdiplus::GdiplusStartupInput si;Check(Gdiplus::GdiplusStartup(&a.mGdiplusToken,&si,nullptr)==Gdiplus::Ok,"GDI startup");
  a.CreateSettingsWindow(GetModuleHandleW(nullptr));a.CreateControls();a.LayoutControls();
  RECT rect{};GetClientRect(a.mHwnd,&rect);Check(rect.right==820&&rect.bottom==540,"original 820x540 shell");

  const auto expected=std::filesystem::path(AlbionProgramDirectory())/L"data"/L"configs";
  Check(std::filesystem::path(a.ConfigDirectory())==expected,"candidate profile directory isolated");
  Check(std::filesystem::path(AlbionUserDirectory()).filename()==L"AlbionAI-Experimental-20261120","license cache isolated");
  wchar_t caption[128]{};GetWindowTextW(a.mHwnd,caption,128);Check(std::wstring(caption)==L"Albion Experimental 20261120","candidate title");
  const auto model=std::filesystem::absolute(L"review-results/fixture.onnx");std::ofstream(model).put('\0');
  a.mModelFiles={model.wstring()};SendMessageW(a.mModelCombo,CB_RESETCONTENT,0,0);SendMessageW(a.mModelCombo,CB_ADDSTRING,0,LPARAM(L"fixture.onnx"));SendMessageW(a.mModelCombo,CB_SETCURSEL,0,0);
  for(int mode=0;mode<5;++mode)for(int prediction=0;prediction<2;++prediction){
    a.mAimCfg.extra.comparisonMode=mode;a.mAimCfg.extra.prediction=prediction!=0;
    const auto text=a.BuildConfigText();albion::ConfigDocument doc;Check(doc.Parse(text),"serialized schema2 validates");
    Check(doc.Number("version",0)==2&&doc.Number("u_comparisonMode",-1)==mode,"schema2 mode serialized");
    const auto file=expected/(L"ui-mode-"+std::to_wstring(mode)+L"-prediction-"+std::to_wstring(prediction)+L".cfg");
    Check(albion::SaveConfigWithHistory(file,text),"isolated profile save");std::string loaded;Check(ReadTextFileUtf8(file.wstring(),loaded)&&loaded==text,"profile disk roundtrip");
    a.mAimCfg.extra.comparisonMode=(mode+1)%5;a.mAimCfg.extra.prediction=prediction==0;
    std::wstring why;Check(a.ApplyConfigText(loaded,&why),"schema2 apply");Check(a.mAimCfg.extra.comparisonMode==mode&&a.mAimCfg.extra.prediction==(prediction!=0),"mode/prediction roundtrip");
    const auto invalid=text+"u_motionMode=1\n";Check(!a.ApplyConfigText(invalid,&why),"Lab-v1 field rejected");Check(a.mAimCfg.extra.comparisonMode==mode,"invalid profile leaves state unchanged");
  }
  std::string legacy=a.BuildConfigText();legacy.replace(legacy.find("version=2"),9,"version=1");auto pos=legacy.find("u_comparisonMode=");Check(pos!=std::string::npos,"mode key exists");legacy.erase(pos,legacy.find('\n',pos)-pos+1);
  std::wstring why;Check(a.ApplyConfigText(legacy,&why)&&a.mAimCfg.extra.comparisonMode==0,"schema1 restores legacy mode");
  Check(a.ApplyConfigText(a.BuildConfigText(),&why),"legacy resave schema2 applies");
  for(int tab=0;tab<4;++tab){a.ApplyTab(tab);Save(a.mHwnd,L"review-results/original-tab-"+std::to_wstring(tab)+L".png");}
  a.ShowUpgrade();HWND embedded=FindWindowExW(a.mHwnd,nullptr,L"AlbionUpgradePanel",nullptr);Check(embedded&&IsChild(a.mHwnd,embedded),"advanced settings must be a child of original main window");Check((GetWindowLongPtrW(embedded,GWL_STYLE)&WS_CHILD)!=0,"no standalone window");Check((GetWindowLongPtrW(embedded,GWL_STYLE)&WS_CAPTION)==0,"no second titlebar");

  HWND selector=FindWindowExW(embedded,nullptr,L"COMBOBOX",nullptr);Check(selector&&SendMessageW(selector,CB_GETCOUNT,0,0)==5,"five candidate modes");
  HWND kalman=FindWindowExW(embedded,nullptr,L"BUTTON",L"Legacy Kalman (Stable mode only)");Check(kalman!=nullptr,"legacy Kalman control");
  for(int mode=0;mode<5;++mode){SendMessageW(selector,CB_SETCURSEL,mode,0);SendMessageW(embedded,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(selector),CBN_SELCHANGE),LPARAM(selector));Check(bool(IsWindowEnabled(kalman))==(mode==0),"mode-specific Kalman enablement");SendMessageW(embedded,WM_COMMAND,1,0);Check(a.mAimCfg.extra.comparisonMode==mode,"native selector applies mode");Save(a.mHwnd,L"review-results/candidate-mode-"+std::to_wstring(mode)+L".png");}
  SendMessageW(selector,CB_SETCURSEL,0,0);SendMessageW(embedded,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(selector),CBN_SELCHANGE),LPARAM(selector));SendMessageW(embedded,WM_COMMAND,1,0);
  // Real native controls, same apply callback as the running program.
  HWND toggle=FindWindowExW(embedded,nullptr,L"BUTTON",L"Многообъектное сопровождение / постоянные ID");Check(toggle!=nullptr,"tracker control");bool before=a.mAimCfg.extra.tracker;SendMessageW(embedded,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(toggle),BN_CLICKED),LPARAM(toggle));SendMessageW(embedded,WM_COMMAND,1,0);Check(a.mAimCfg.extra.tracker!=before,"apply reaches original App config");
  std::ofstream paint("review-results/original-ui-paint.csv");paint<<"subtab,iteration,microseconds\n";
  for(int page=0;page<5;++page){SendMessageW(embedded,WM_COMMAND,100+page,0);Pump(60);for(int i=0;i<100;++i){auto t=std::chrono::steady_clock::now();RedrawWindow(embedded,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);paint<<page<<','<<i<<','<<std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t).count()<<'\n';}Save(a.mHwnd,L"review-results/integrated-section-"+std::to_wstring(page)+L".png");}
  a.ApplyTab(0);Check(!FindWindowExW(a.mHwnd,nullptr,L"AlbionUpgradePanel",nullptr),"normal tabs reclaim content area");DWORD baseline=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
  for(int i=0;i<20;++i){a.ShowUpgrade();a.ApplyTab(i%4);Pump(2);}DWORD after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);Check(after<=baseline+2,"GDI handles leaked");
  std::cout<<"CANDIDATE_UI_PASS schema1/schema2=PASS profiles_isolated=PASS five_modes=PASS batch_receipts=PASS prediction_roundtrip=PASS shell=820x540 original_tabs=4 integrated_sections=5 apply_callback=PASS gdi_before="<<baseline<<" gdi_after="<<after<<" no_capture_no_auth_no_mouse_input\n";
 }
};
int wmain(){try{std::filesystem::create_directories(L"review-results");AlbionUiComponentCheck::Run();return 0;}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
