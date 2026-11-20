#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <gdiplus.h>
#include "wgc_capture.hpp"
#include "modules/capture/frame_services.hpp"
#include <iostream>
#include <fstream>
#include <numeric>
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"gdiplus.lib")
using Microsoft::WRL::ComPtr;
static double Now(){static double k=[](){LARGE_INTEGER f;QueryPerformanceFrequency(&f);return 1000./f.QuadPart;}();LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart*k;}
static void Messages(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
int wmain(){
 winrt::init_apartment(winrt::apartment_type::multi_threaded);ULONG_PTR token=0;Gdiplus::GdiplusStartupInput si;Gdiplus::GdiplusStartup(&token,&si,nullptr);int rc=0;HWND window=nullptr;
 try{
 ComPtr<IDXGIFactory1> factory;winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;ComPtr<IDXGIOutput1> output;auto primary=MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
 for(UINT a=0;!output;++a){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(a,&candidate)==DXGI_ERROR_NOT_FOUND)break;for(UINT i=0;;++i){ComPtr<IDXGIOutput> out;if(candidate->EnumOutputs(i,&out)==DXGI_ERROR_NOT_FOUND)break;DXGI_OUTPUT_DESC d{};out->GetDesc(&d);if(d.Monitor==primary){adapter=candidate;out.As(&output);break;}}}
 if(!output)throw std::runtime_error("No primary output");ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;winrt::check_hresult(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx));
 window=CreateWindowExW(0,L"STATIC",L"Albion synthetic capture benchmark - no input",WS_OVERLAPPEDWINDOW|WS_VISIBLE|SS_CENTER,80,80,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);UpdateWindow(window);
 std::ofstream csv("review-results/capture-bench.csv");csv<<"case,iteration,acquire_and_submit_ms,source_width,source_height,sample_width,sample_height\n";
 for(bool wgc:{false,true}){
 ComPtr<IDXGIOutputDuplication> dupl;WgcCapture capture;if(wgc)capture.Start(device.Get(),primary,nullptr,window);else winrt::check_hresult(output->DuplicateOutput(device.Get(),&dupl));
 for(int options=0;options<4;++options){bool roi=(options&1)!=0,preview=(options&2)!=0;albion::upgrade::FrameServices frames;albion::upgrade::Options o;o.preview=preview;o.previewFps=30;std::vector<double> samples;double until=Now()+3000;int tick=0,previews=0,timeouts=0;std::uint64_t previewId=0;
 std::string name=std::string(wgc?"wgc_window_":"dxgi_monitor_")+(roi?"roi":"full")+(preview?"_preview_on":"_preview_off");
 while(Now()<until){Messages();SetWindowTextW(window,(L"Albion generated frame "+std::to_wstring(++tick)).c_str());double begin=Now();ComPtr<ID3D11Texture2D> tex;bool held=false;std::shared_ptr<WgcCapture::Frame> f;
 if(wgc){f=capture.Take();if(f)tex=f->texture;}
 else{DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> r;HRESULT hr=dupl->AcquireNextFrame(10,&info,&r);if(hr==DXGI_ERROR_WAIT_TIMEOUT){++timeouts;continue;}winrt::check_hresult(hr);held=true;r.As(&tex);}
 if(!tex){++timeouts;Sleep(1);continue;}D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);UINT w=roi?160:td.Width,h=roi?160:td.Height;int x=roi?int((td.Width-w)/2):0,y=roi?int((td.Height-h)/2):0;
 frames.Observe(device.Get(),ctx.Get(),tex.Get(),td.Width,td.Height,x,y,w,h,tick,Now(),o,false,"generated-window",.5f);if(held)winrt::check_hresult(dupl->ReleaseFrame());
 auto image=frames.Preview();if(image&&image->id!=previewId){previewId=image->id;++previews;if(image->w!=w||image->h!=h)throw std::runtime_error("preview dimension mismatch");}
 double elapsed=Now()-begin;samples.push_back(elapsed);csv<<name<<','<<samples.size()<<','<<elapsed<<','<<td.Width<<','<<td.Height<<','<<w<<','<<h<<'\n';Sleep(2);
 }
 frames.Stop();if(samples.empty()||(preview&&!previews))throw std::runtime_error("No capture/preview samples");std::sort(samples.begin(),samples.end());auto q=[&](double f){return samples[std::min(samples.size()-1,size_t(f*samples.size()))];};
 std::cout<<name<<" count="<<samples.size()<<" p50_ms="<<q(.5)<<" p95_ms="<<q(.95)<<" p99_ms="<<q(.99)<<" previews="<<previews<<" timeouts="<<timeouts<<'\n';
 }
 capture.Stop();dupl.Reset();
 }
 std::cout<<"CAPTURE_BENCH_PASS: generated window / desktop samples held in RAM, no desktop images persisted, no inference/input. Boundary acquisition plus preview submission, NOT inference latency or game FPS.\n";
 }catch(const winrt::hresult_error&e){std::cerr<<winrt::to_string(e.message())<<'\n';rc=2;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';rc=3;}
 if(window)DestroyWindow(window);Gdiplus::GdiplusShutdown(token);winrt::uninit_apartment();return rc;
}
