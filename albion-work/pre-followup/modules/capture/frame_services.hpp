#pragma once
#include "modules/tracking/upgrade_core.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <gdiplus.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstring>
namespace albion::upgrade {
struct CpuFrame {unsigned w=0,h=0;int left=0,top=0;std::uint64_t id=0;double timestamp=0;std::vector<unsigned char> pixels;Options options;std::string model;float confidence=0;bool recordEligible=false;};
class FrameServices {
 using Clock=std::chrono::steady_clock;
 struct Slot {Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;Microsoft::WRL::ComPtr<ID3D11Query> query;std::shared_ptr<CpuFrame> frame;};
 Slot slots[3];unsigned width=0,height=0;double nextSample=0;std::mutex mx;std::condition_variable cv;std::deque<std::shared_ptr<CpuFrame>> jobs;std::map<std::uint64_t,std::vector<Detection>> labels;std::shared_ptr<const CpuFrame> preview;std::thread writer;bool stop=false;std::atomic<std::uint64_t> saved{0},dropped{0};std::string error;std::uint64_t bytesWritten=0;double lastRecord=0;
 void Fail(const std::string&s){std::lock_guard<std::mutex> lock(mx);error=s;}
 void Writer(){
  for(;;){std::shared_ptr<CpuFrame> frame;std::vector<Detection> ds;bool matched=false;
   {std::unique_lock<std::mutex> lock(mx);cv.wait(lock,[&]{return stop||!jobs.empty();});if(stop&&jobs.empty())break;frame=std::move(jobs.front());jobs.pop_front();
    if(frame->options.autoLabel)cv.wait_for(lock,std::chrono::milliseconds(200),[&]{return stop||labels.count(frame->id);});auto it=labels.find(frame->id);if(it!=labels.end()){matched=true;ds=it->second;}}
   try{
    namespace fs=std::filesystem;fs::path dir=frame->options.recordDir.empty()?fs::current_path()/L"datasets"/L"albion":fs::path(frame->options.recordDir);fs::create_directories(dir);
    auto stem=std::to_wstring(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count())+L"_"+std::to_wstring(frame->id);auto image=dir/(stem+L".jpg");
    if(bytesWritten>=1024ull*1024*1024){Fail("Dataset session limit 1 GiB reached; disable/restart recording session");++dropped;continue;}
    CLSID jpeg{0x557cf401,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    Gdiplus::Bitmap bitmap(frame->w,frame->h,INT(frame->w*4),PixelFormat32bppARGB,frame->pixels.data());ULONG quality=90;Gdiplus::EncoderParameters params{};params.Count=1;params.Parameter[0].Guid=Gdiplus::EncoderQuality;params.Parameter[0].Type=Gdiplus::EncoderParameterValueTypeLong;params.Parameter[0].NumberOfValues=1;params.Parameter[0].Value=&quality;
    if(bitmap.Save(image.c_str(),&jpeg,&params)!=Gdiplus::Ok)throw std::runtime_error("JPEG encoding/save failed");
    std::ofstream meta(dir/(stem+L".json"));if(!meta)throw std::runtime_error("Cannot write frame metadata");meta<<"{\"frame_id\":"<<frame->id<<",\"capture_ms\":"<<frame->timestamp<<",\"model\":"<<std::quoted(frame->model)<<",\"width\":"<<frame->w<<",\"height\":"<<frame->h<<",\"crop_x\":"<<frame->left<<",\"crop_y\":"<<frame->top<<",\"confidence\":"<<frame->confidence<<",\"nms_iou\":"<<frame->options.iou<<",\"postprocess\":"<<frame->options.postprocess<<",\"head_class\":"<<frame->options.headClass<<",\"confidence_labels_require_review\":true,\"labels_matched\":"<<(matched?"true":"false")<<"}\n";
    if(frame->options.autoLabel&&matched){std::ofstream text(dir/(stem+L".txt"));if(!text)throw std::runtime_error("Cannot write labels");for(const auto&d:ds){if(!std::isfinite(d.score)||d.score<.5f)continue;float x1=std::clamp(d.x-frame->left,0.f,float(frame->w)),y1=std::clamp(d.y-frame->top,0.f,float(frame->h)),x2=std::clamp(d.x+d.w-frame->left,0.f,float(frame->w)),y2=std::clamp(d.y+d.h-frame->top,0.f,float(frame->h));if(x2>x1&&y2>y1)text<<d.cls<<' '<<(x1+x2)*.5f/frame->w<<' '<<(y1+y2)*.5f/frame->h<<' '<<(x2-x1)/frame->w<<' '<<(y2-y1)/frame->h<<'\n';}}
    bytesWritten+=fs::file_size(image);++saved;
   }catch(const std::exception&e){Fail(e.what());++dropped;}
  }
 }
public:
#ifdef ALBION_UPGRADE_TESTS
 void InjectTestFrame(const std::shared_ptr<CpuFrame>&frame,bool recording){std::lock_guard<std::mutex> lock(mx);preview=frame;if(recording){jobs.push_back(frame);if(!writer.joinable()){stop=false;writer=std::thread([this]{Writer();});}cv.notify_all();}}
 std::uint64_t SavedForTest()const{return saved.load();}
#endif
 ~FrameServices(){Stop();}
 void Stop(){ {std::lock_guard<std::mutex> lock(mx);stop=true;jobs.clear();}cv.notify_all();if(writer.joinable())writer.join();for(auto&s:slots)s=Slot{};}
 std::shared_ptr<const CpuFrame> Preview(){std::lock_guard<std::mutex> lock(mx);return preview;}
 std::vector<Detection> LabelsFor(std::uint64_t id){std::lock_guard<std::mutex> lock(mx);auto it=labels.find(id);return it==labels.end()?std::vector<Detection>{}:it->second;}
 void Labels(std::uint64_t id,const std::vector<Detection>&ds){std::lock_guard<std::mutex> lock(mx);labels[id]=ds;while(labels.size()>64)labels.erase(labels.begin());cv.notify_all();}
 std::wstring Status(){std::lock_guard<std::mutex> lock(mx);return L"Saved="+std::to_wstring(saved.load())+L" dropped="+std::to_wstring(dropped.load())+L" queue="+std::to_wstring(jobs.size())+L" "+std::wstring(error.begin(),error.end());}
 void CancelRecording(){std::lock_guard<std::mutex> lock(mx);jobs.clear();}
 void ResetCapture(){for(auto&s:slots)s=Slot{};width=height=0;nextSample=0;lastRecord=0;std::lock_guard<std::mutex> lock(mx);labels.clear();preview.reset();}
 void Observe(ID3D11Device*dev,ID3D11DeviceContext*ctx,ID3D11Texture2D*src,unsigned sourceW,unsigned sourceH,int left,int top,unsigned w,unsigned h,std::uint64_t id,double time,const Options&o,bool active,const std::string&model,float confidence){
  bool recording=o.record&&(!o.recordActive||active);if(!o.preview&&!o.record){if(width||height)ResetCapture();return;}if(!o.preview&&!recording)return;
  if(!src||!w||!h||std::uint64_t(w)*h>4096ull*2160||left<0||top<0||unsigned(left)+w>sourceW||unsigned(top)+h>sourceH)return;
  if(width!=w||height!=h){ResetCapture();width=w;height=h;}
  for(auto&s:slots)if(s.frame){if(ctx->GetData(s.query.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;D3D11_MAPPED_SUBRESOURCE mapped{};HRESULT hr=ctx->Map(s.texture.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);if(hr==DXGI_ERROR_WAS_STILL_DRAWING)continue;if(FAILED(hr)){s.frame.reset();++dropped;continue;}auto f=std::move(s.frame);f->pixels.resize(size_t(w)*h*4);for(unsigned y=0;y<h;++y)std::memcpy(f->pixels.data()+size_t(y)*w*4,static_cast<unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(w)*4);ctx->Unmap(s.texture.Get(),0);
   std::lock_guard<std::mutex> lock(mx);if(o.preview)preview=f;
   if(o.record&&f->recordEligible&&f->timestamp-lastRecord>=f->options.recordInterval){lastRecord=f->timestamp;if(jobs.size()>=8){++dropped;}else{jobs.push_back(f);if(!writer.joinable()){stop=false;writer=std::thread([this]{Writer();});}cv.notify_all();}}
  }
  double interval=o.preview?1000./std::clamp(o.previewFps,1,30):double(std::max(100,o.recordInterval));if(time<nextSample)return;nextSample=time+interval;
  Slot*available=nullptr;for(auto&s:slots)if(!s.frame){available=&s;break;}if(!available){++dropped;return;}auto&s=*available;
  if(!s.texture){D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT,0};if(FAILED(dev->CreateTexture2D(&td,nullptr,&s.texture))||FAILED(dev->CreateQuery(&qd,&s.query))){++dropped;return;}}
  auto f=std::make_shared<CpuFrame>();f->w=w;f->h=h;f->left=left;f->top=top;f->id=id;f->timestamp=time;f->options=o;f->model=model;f->confidence=confidence;f->recordEligible=recording;s.frame=f;D3D11_BOX box{UINT(left),UINT(top),0,UINT(left)+w,UINT(top)+h,1};ctx->CopySubresourceRegion(s.texture.Get(),0,0,0,0,src,0,&box);ctx->End(s.query.Get());ctx->Flush();
 }
};
}
