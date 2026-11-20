#pragma once
#include "basic_winapi_aim.hpp"
#include "modules/capture/frame_services.hpp"
#include <functional>
#include <cwchar>
#include "modules/ui/theme.hpp"
#include "ui_fonts.hpp"
#include <uxtheme.h>
#pragma comment(lib,"uxtheme.lib")
namespace albion::upgrade {
class Panel {
 std::function<void(HWND)> styleCombo;HWND hwnd=nullptr;BasicAimConfig cfg;int capture=1,page=0;FrameServices* frames=nullptr;HFONT font=nullptr;HBRUSH brush=nullptr,inputBrush=nullptr;std::unique_ptr<Gdiplus::Image> humanoid;std::vector<std::pair<HWND,bool>> checks;bool binding=false;ULONGLONG bindAfter=0;
 std::function<void(const BasicAimConfig&,int)> apply;
 struct Field{HWND label=nullptr,edit=nullptr;int page=0,kind=0;double low=0,high=0;std::function<void(double)> set;};std::vector<Field> fields;HWND windowEdit=nullptr,dirEdit=nullptr,bindLabel=nullptr;int row=0;HWND modeEdit=nullptr,kalmanEdit=nullptr,qEdit=nullptr,rEdit=nullptr;std::function<std::wstring()> comparisonStatus;
 HWND Make(const wchar_t*cls,const wchar_t*text,DWORD style,int x,int y,int w,int h,int id=0){auto control=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,hwnd,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(control,WM_SETFONT,WPARAM(font),TRUE);
 if(std::wcscmp(cls,L"BUTTON")==0){LONG_PTR flags=GetWindowLongPtrW(control,GWL_STYLE);SetWindowLongPtrW(control,GWL_STYLE,(flags&~BS_TYPEMASK)|BS_OWNERDRAW);}
 SetWindowTheme(control,L"",L"");if(std::wcscmp(cls,L"COMBOBOX")==0&&styleCombo)styleCombo(control);return control;}
 void Bool(const wchar_t*name,bool& value){int y=82+row++*32;auto h=Make(L"BUTTON",name,BS_AUTOCHECKBOX|WS_TABSTOP,22,y,540,26);checks.emplace_back(h,value);bool* ptr=&value;fields.push_back({nullptr,h,page,1,0,1,[ptr](double v){*ptr=v!=0;}});}
 template<class T>void Number(const wchar_t*name,T&value,double low,double high){int y=82+row++*32;auto label=Make(L"STATIC",name,0,22,y+4,360,24);wchar_t text[64];swprintf_s(text,L"%.6g",double(value));auto edit=Make(L"EDIT",text,WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,390,y,174,25);SendMessageW(edit,EM_SETLIMITTEXT,32,0);T*ptr=&value;fields.push_back({label,edit,page,0,low,high,[ptr](double v){*ptr=T(v);}});}
 void Combo(const wchar_t*name,int&value,const std::vector<const wchar_t*>&items){int y=82+row++*32;auto label=Make(L"STATIC",name,0,22,y+4,260,24);auto edit=Make(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,290,y,274,240);for(auto item:items)SendMessageW(edit,CB_ADDSTRING,0,LPARAM(item));SendMessageW(edit,CB_SETCURSEL,value,0);int*ptr=&value;fields.push_back({label,edit,page,2,0,double(items.size()-1),[ptr](double v){*ptr=int(v);}});}
 void Text(const wchar_t*label,HWND&edit,const std::wstring&value){int y=82+row++*32;auto l=Make(L"STATIC",label,0,22,y+4,220,24);edit=Make(L"EDIT",value.c_str(),WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,250,y,314,25);SendMessageW(edit,EM_SETLIMITTEXT,512,0);fields.push_back({l,edit,page,3,0,0,{}});}
 static std::wstring TextOf(HWND edit){wchar_t s[1024]{};GetWindowTextW(edit,s,1024);return s;}
 void ChangePage(int n){page=n;for(auto&f:fields){if(f.label)ShowWindow(f.label,f.page==page?SW_SHOW:SW_HIDE);ShowWindow(f.edit,f.page==page?SW_SHOW:SW_HIDE);}RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);}
 void Create(){font=AlbionFont(13,500);brush=CreateSolidBrush(kThPanel);inputBrush=CreateSolidBrush(kThInput);
 humanoid=std::make_unique<Gdiplus::Image>((AlbionExeDir()+L"extras\\images\\humanoid-front.png").c_str());
  const wchar_t*tabs[]={L"Трекинг",L"Отдача",L"Модель",L"Захват / данные",L"Устройства"};const int pages[]={0,4,1,2,3};const int x[]={0,116,222,320,488},w[]={108,98,90,160,154};for(int i=0;i<5;++i)Make(L"BUTTON",tabs[i],WS_TABSTOP,x[i],0,w[i],32,100+pages[i]);
  page=0;row=0;Combo(L"Estimator",cfg.extra.comparisonMode,{L"Stable: legacy",L"Lab: velocity EMA",L"Lab: guarded Kalman",L"Lab: One Euro",L"Lab: SunOne (MIT)"});modeEdit=fields.back().edit;Bool(L"Многообъектное сопровождение / постоянные ID",cfg.extra.tracker);Bool(L"Legacy Kalman (Stable mode only)",cfg.extra.kalman);kalmanEdit=fields.back().edit;Bool(L"Prediction: прогноз в момент обработки",cfg.extra.prediction);Number(L"Kalman process noise (px²/s³)",cfg.extra.processNoise,.001,1000000);qEdit=fields.back().edit;Number(L"Kalman measurement noise (px²)",cfg.extra.measurementNoise,.001,100000);rEdit=fields.back().edit;Number(L"Дополнительный прогноз, мс",cfg.extra.predictionMs,0,150);Number(L"Максимальное опережение, px",cfg.extra.maxLead,0,500);page=4;row=0;Bool(L"Вертикальная компенсация включена",cfg.extra.recoil);Combo(L"Условие компенсации",cfg.extra.recoilMode,{L"Удерживается наведение",L"Наведение + отдельный бинд"});Number(L"Скорость вниз, output counts/s",cfg.extra.recoilSpeed,0,5000);Number(L"VK бинда компенсации (1 = LMB)",cfg.extra.recoilKey,1,254);bindLabel=Make(L"BUTTON",L"Назначить бинд: нажать и затем нужную кнопку",WS_TABSTOP,22,82+row++*32,542,26,120);fields.push_back({nullptr,bindLabel,4,3,0,0,{}});
  page=1;row=0;Combo(L"Backend / формат",cfg.extra.backend,{L"Auto: engine TRT / ONNX DML→CPU",L"DirectML (без fallback)",L"CPU / ONNX",L"TensorRT: только engine/plan/trt"});Number(L"DirectML DXGI adapter index",cfg.extra.dmlDevice,0,31);Number(L"Confidence",cfg.confidence,.05,.95);Number(L"IoU порог NMS",cfg.extra.iou,0,1);Number(L"Максимум обнаружений",cfg.extra.maxDetections,1,500);Number(L"Класс корпуса / объекта (-1 = все)",cfg.targetClass,-1,79);Bool(L"Модель имеет отдельную голову и корпус",cfg.extra.headBody);Number(L"Индекс класса головы",cfg.extra.headClass,0,79);Combo(L"Постобработка",cfg.extra.postprocess,{L"Albion: class-aware NMS",L"Sunone-compatible: class-agnostic"});
  page=2;row=0;Combo(L"Захват / область обработки",capture,{L"DXGI ROI",L"DXGI полный кадр",L"WGC полный кадр",L"WGC ROI"});Text(L"WGC: часть имени окна",windowEdit,cfg.extra.windowTitle);Bool(L"Предпросмотр (повышает нагрузку)",cfg.extra.preview);Number(L"Частота preview, FPS",cfg.extra.previewFps,1,30);Bool(L"Сбор кадров для обучения",cfg.extra.record);Bool(L"Сохранять лишь при удержании наведения",cfg.extra.recordActive);Number(L"Интервал записи, мс",cfg.extra.recordInterval,100,60000);Text(L"Папка (пусто = datasets/albion)",dirEdit,cfg.extra.recordDir);Bool(L"Черновые YOLO labels: требуют проверки",cfg.extra.autoLabel);
  page=3;row=0;auto l=Make(L"STATIC",L"Рабочие исходные адаптеры: WinAPI, Arduino HID.\r\n\r\nRP2040/RP2350, Teensy Serial/RawHID, MAKCU, KMBox:\r\nпрофили расширения находятся в devices/adapters.json.\r\nЭто заготовки, не подключённые и не проверенные платы.\r\nВыбор работающего вывода остаётся в основном окне.\r\n\r\nВ коде: capabilities и регистрация будущих transport adapters.\r\nНеподдерживаемый адаптер должен возвращать Unsupported.\r\nДрайверные обходы и подмена USB-дескрипторов не добавлены.",0,22,90,550,340);fields.push_back({nullptr,l,3,3,0,0,{}});
  Make(L"BUTTON",L"Применить",WS_TABSTOP,0,362,180,32,1);Make(L"STATIC",L"Experimental 20261120 | schema 2 | isolated profiles",0,194,368,448,22);
  int rows[5]{};for(auto&f:fields){int y=46+32*rows[f.page]++;if(f.label){MoveWindow(f.label,0,y+5,f.kind==0?288:f.kind==2?154:170,22,FALSE);MoveWindow(f.edit,f.kind==0?300:f.kind==2?158:174,y,f.kind==0?124:f.kind==2?266:250,f.kind==2?220:27,FALSE);}else MoveWindow(f.edit,0,y,424,f.page==3?292:28,FALSE);}
  RefreshApplicable();ChangePage(0);SetTimer(hwnd,1,100,nullptr);
 }
 void RefreshApplicable(){
  const int mode=int(SendMessageW(modeEdit,CB_GETCURSEL,0,0));bool legacyKalman=false;
  for(const auto& c:checks)if(c.first==kalmanEdit)legacyKalman=c.second;
  for(auto&f:fields){bool enabled=true;if(f.edit==kalmanEdit)enabled=mode==0;if(f.edit==qEdit||f.edit==rEdit)enabled=mode==2||(mode==0&&legacyKalman);EnableWindow(f.edit,enabled);}
 }
 bool Commit(){std::vector<double> values;for(auto&f:fields){double v=0;if(f.kind==1){for(auto&check:checks)if(check.first==f.edit)v=check.second?1:0;}else if(f.kind==2)v=double(SendMessageW(f.edit,CB_GETCURSEL,0,0));else if(f.kind==0){auto s=TextOf(f.edit);wchar_t*end=nullptr;v=std::wcstod(s.c_str(),&end);if(s.empty()||!end||*end||!std::isfinite(v)||v<f.low||v>f.high){MessageBoxW(hwnd,L"Проверьте числовое поле и допустимый диапазон.",L"Albion",MB_ICONWARNING);SetFocus(f.edit);return false;}}values.push_back(v);}
  for(size_t i=0;i<fields.size();++i)if(fields[i].set)fields[i].set(values[i]);cfg.extra.windowTitle=TextOf(windowEdit);cfg.extra.recordDir=TextOf(dirEdit);
  if(cfg.extra.headBody&&cfg.extra.headClass==cfg.targetClass){MessageBoxW(hwnd,L"Голова и корпус должны иметь разные индексы классов.",L"Albion",MB_ICONWARNING);return false;}
  // Advanced output is composed on the pipeline owner, never on two senders.
  if(cfg.extra.recoil)cfg.asyncMover=false;apply(cfg,capture);SetWindowTextW(hwnd,L"Albion · расширенные функции — применено");return true;
 }
 void DrawControl(DRAWITEMSTRUCT* item){
 auto dc=item->hDC;RECT r=item->rcItem;SetBkMode(dc,TRANSPARENT);SelectObject(dc,font);
 bool checked=false,isCheck=false;for(auto& c:checks)if(c.first==item->hwndItem){isCheck=true;checked=c.second;}
 const int id=GetDlgCtrlID(item->hwndItem);bool selected=(item->itemState&ODS_SELECTED)!=0;
 if(item->CtlType==ODT_COMBOBOX){ThemeRoundRect(dc,r,4,selected?kThSelect:kThInput,kThBorder,1);if(item->itemID!=UINT(-1)){wchar_t text[256]{};SendMessageW(item->hwndItem,CB_GETLBTEXT,item->itemID,LPARAM(text));r.left+=10;SetTextColor(dc,kThText);DrawTextW(dc,text,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);}return;}
 if(isCheck){auto fill=CreateSolidBrush(kThPanel);FillRect(dc,&r,fill);DeleteObject(fill);RECT box{r.left+2,r.top+4,r.left+20,r.top+22};ThemeRoundRect(dc,box,5,checked?kThAccent:kThInput,checked?kThAccent:kThBorder,1);if(checked){SetTextColor(dc,kThTextBright);DrawTextW(dc,L"✓",-1,&box,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}r.left+=30;}
 else{auto background=CreateSolidBrush(kThPanel);FillRect(dc,&r,background);DeleteObject(background);bool active=id>=100&&id<=104&&id-100==page;
 if(id>=100&&id<=104){SetTextColor(dc,active?kThTextBright:kThTextMuted);wchar_t label[128]{};GetWindowTextW(item->hwndItem,label,128);RECT text=r;text.bottom-=5;DrawTextW(dc,label,-1,&text,DT_LEFT|DT_VCENTER|DT_SINGLELINE);if(active){RECT line{r.left,r.bottom-3,r.left+60,r.bottom};auto accent=CreateSolidBrush(kThAccent);FillRect(dc,&line,accent);DeleteObject(accent);}return;}
 ThemeRoundRect(dc,r,14,selected?kThPressed:kThInput,kThBorder,1);r.left+=8;r.right-=8;}
 SetTextColor(dc,kThText);wchar_t text[512]{};GetWindowTextW(item->hwndItem,text,512);DrawTextW(dc,text,-1,&r,(isCheck?DT_LEFT:DT_CENTER)|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
 if(item->itemState&ODS_FOCUS){InflateRect(&r,-3,-3);DrawFocusRect(dc,&r);}
 }
 void Paint(){PAINTSTRUCT ps{};auto dc=BeginPaint(hwnd,&ps);RECT client{};GetClientRect(hwnd,&client);FillRect(dc,&client,brush);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,kThText);SelectObject(dc,font);
  RECT title{442,43,642,64};DrawTextW(dc,L"Точка наведения",-1,&title,DT_CENTER|DT_SINGLELINE);
  if(humanoid&&humanoid->GetLastStatus()==Gdiplus::Ok){Gdiplus::Graphics g(dc);g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);g.DrawImage(humanoid.get(),449,68,186,169);}
  float fraction=cfg.targetBodyPart==AimBodyPart::Head?.075f:cfg.targetBodyPart==AimBodyPart::Neck?.3f:cfg.targetBodyPart==AimBodyPart::Body?.45f:cfg.aimHeight;
  auto point=CreateSolidBrush(kThAccent);auto old=SelectObject(dc,point);int yy=68+int(fraction*169);Ellipse(dc,538,yy-5,548,yy+5);SelectObject(dc,old);DeleteObject(point);
  RECT hint{440,239,644,271};DrawTextW(dc,cfg.extra.headBody?L"Head/body: классы модели":L"Геометрия bbox — не head detector",-1,&hint,DT_CENTER|DT_WORDBREAK);
  RECT area{442,274,642,352};auto black=CreateSolidBrush(kThBg);FillRect(dc,&area,black);DeleteObject(black);
  auto image=frames?frames->Preview():nullptr;
  if(image&&!image->pixels.empty()){float scale=std::min(200.f/image->w,78.f/image->h);int w=int(image->w*scale),h=int(image->h*scale),left=442+(200-w)/2,top=274+(78-h)/2;Gdiplus::Graphics g(dc);Gdiplus::Bitmap b(image->w,image->h,INT(image->w*4),PixelFormat32bppARGB,const_cast<BYTE*>(image->pixels.data()));g.DrawImage(&b,left,top,w,h);Gdiplus::Pen box(Gdiplus::Color(255,90,220,140),1.2f);for(const auto&d:frames->LabelsFor(image->id))g.DrawRectangle(&box,float(left)+(d.x-image->left)*scale,float(top)+(d.y-image->top)*scale,d.w*scale,d.h*scale);}
  else{auto empty=area;DrawTextW(dc,L"Preview выключен",-1,&empty,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
  if(page==0&&comparisonStatus){RECT r{0,302,424,334};auto text=comparisonStatus();DrawTextW(dc,text.c_str(),-1,&r,DT_LEFT|DT_WORDBREAK);}
  if(frames){RECT status{0,336,424,358};SetTextColor(dc,kThTextMuted);auto text=frames->Status();DrawTextW(dc,text.c_str(),-1,&status,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);}
  EndPaint(hwnd,&ps);
 }
 static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l){auto*self=reinterpret_cast<Panel*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);self->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,LONG_PTR(self));}if(!self)return DefWindowProcW(h,m,w,l);
 switch(m){case WM_MEASUREITEM:reinterpret_cast<MEASUREITEMSTRUCT*>(l)->itemHeight=25;return TRUE;
 case WM_DRAWITEM:self->DrawControl(reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;
 case WM_CREATE:self->Create();return 0;case WM_COMMAND:
 if(reinterpret_cast<HWND>(l)==self->modeEdit&&HIWORD(w)==CBN_SELCHANGE){self->RefreshApplicable();return 0;}
 for(auto&check:self->checks)if(check.first==reinterpret_cast<HWND>(l)&&HIWORD(w)==BN_CLICKED){check.second=!check.second;self->RefreshApplicable();InvalidateRect(check.first,nullptr,TRUE);return 0;}if(LOWORD(w)>=100&&LOWORD(w)<=104){self->ChangePage(int(LOWORD(w))-100);return 0;}if(LOWORD(w)==1){self->Commit();return 0;}if(LOWORD(w)==2){PostMessageW(GetParent(h),WM_APP+78,0,0);return 0;}if(LOWORD(w)==120){self->binding=true;self->bindAfter=GetTickCount64()+400;SetWindowTextW(self->bindLabel,L"Нажмите нужную кнопку...");return 0;}break;case WM_TIMER:if(self->binding&&GetTickCount64()>self->bindAfter){for(int key=1;key<255;++key)if(GetAsyncKeyState(key)&0x8000){self->cfg.extra.recoilKey=key;for(auto&f:self->fields)if(f.page==4&&f.kind==0&&f.low==1&&f.high==254)SetWindowTextW(f.edit,std::to_wstring(key).c_str());self->binding=false;SetWindowTextW(self->bindLabel,L"Назначить бинд: нажать и затем нужную кнопку");break;}}InvalidateRect(h,nullptr,FALSE);return 0;case WM_LBUTTONDOWN:if(LOWORD(l)>=449&&LOWORD(l)<=635&&HIWORD(l)>=68&&HIWORD(l)<=237){float fraction=std::clamp((float(HIWORD(l))-68)/169,0.f,1.f);self->cfg.targetBodyPart=fraction<.22f?AimBodyPart::Head:fraction<.36f?AimBodyPart::Neck:fraction<.62f?AimBodyPart::Body:AimBodyPart::Custom;self->cfg.aimHeight=fraction;InvalidateRect(h,nullptr,FALSE);}return 0;case WM_PAINT:self->Paint();return 0;case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(w),kThText);SetBkColor(reinterpret_cast<HDC>(w),m==WM_CTLCOLOREDIT?kThInput:kThPanel);return LRESULT(m==WM_CTLCOLOREDIT?self->inputBrush:self->brush);case WM_CTLCOLORLISTBOX:SetTextColor(reinterpret_cast<HDC>(w),kThText);SetBkColor(reinterpret_cast<HDC>(w),kThInput);return LRESULT(self->inputBrush);case WM_ERASEBKGND:return 1;case WM_CLOSE:PostMessageW(GetParent(h),WM_APP+78,0,0);return 0;case WM_NCDESTROY:KillTimer(h,1);self->hwnd=nullptr;if(self->font){DeleteObject(self->font);self->font=nullptr;}if(self->brush){DeleteObject(self->brush);self->brush=nullptr;}if(self->inputBrush){DeleteObject(self->inputBrush);self->inputBrush=nullptr;}self->humanoid.reset();break;}
 return DefWindowProcW(h,m,w,l);
 }
public:
 void SetComparisonStatus(std::function<std::wstring()> f){comparisonStatus=std::move(f);}
 void Close(){if(hwnd)DestroyWindow(hwnd);}
 ~Panel(){Close();}
 void Open(HWND owner,const BasicAimConfig&config,int mode,FrameServices*service,std::function<void(const BasicAimConfig&,int)>callback,std::function<void(HWND)>comboDecorator={}){if(hwnd){ShowWindow(hwnd,SW_SHOW);SetForegroundWindow(hwnd);return;}styleCombo=std::move(comboDecorator);cfg=config;capture=mode;frames=service;apply=std::move(callback);fields.clear();checks.clear();WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Proc;wc.lpszClassName=L"AlbionUpgradePanel";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);if(!owner)throw std::invalid_argument("Advanced settings require the original Albion main window");CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,124,82,664,400,owner,nullptr,wc.hInstance,this);}
};
}
