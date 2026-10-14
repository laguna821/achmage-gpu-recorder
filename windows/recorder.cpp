// SPDX-License-Identifier: GPL-3.0-only
// Achmage Recorder: native hotkey broker + disposable hardware-encoding worker.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <codecapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/base.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <cmath>
#include <thread>
#include <cstdio>
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
namespace fs=std::filesystem;
thread_local std::string failedCall;
void checkAt(HRESULT hr,char const*call){if(FAILED(hr)){failedCall=call;winrt::check_hresult(hr);}}
#define check_hresult(hr) checkAt((hr),#hr)
constexpr UINT TOGGLE=WM_APP+1, STOP=WM_APP+2, EXIT=WM_APP+3;
constexpr wchar_t CLASSNAME[]=L"AchmageRecorderBroker01";
constexpr wchar_t STOPNAME[]=L"Local\\AchmageRecorderStop01";
constexpr ULONGLONG MIN_FREE=2ull*1024*1024*1024;
HANDLE stopEvent=nullptr, child=nullptr;
HWND broker=nullptr, badge=nullptr;
std::wstring currentFile;
bool exitWhenDone=false;
HHOOK keyboardHook=nullptr;
bool rHeld=false;
std::string hotkeyMode="unregistered";
NOTIFYICONDATAW tray{};
fs::path dataDir(){
 PWSTR s=nullptr; check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&s));
 fs::path p=fs::path(s)/L"AchmageRecorder"; CoTaskMemFree(s); fs::create_directories(p); return p;
}
fs::path downloads(){
 PWSTR s=nullptr; check_hresult(SHGetKnownFolderPath(FOLDERID_Downloads,0,nullptr,&s));
 fs::path p=s; CoTaskMemFree(s); return p;
}
std::string utf8(std::wstring const&s){return to_string(s);}
std::string escape(std::wstring const&s){
 std::string x=utf8(s), r;
 for(unsigned char c:x){if(c=='"'||c=='\\')r+='\\';if(c>=32)r+=c;else r+=' ';}
 return r;
}
void atomicText(fs::path const&p,std::string const&s){
 auto tmp=p;tmp+=L".tmp";{std::ofstream f(tmp,std::ios::binary);if(!f)throw std::runtime_error("state write failed");f<<s;}
 if(!MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
}
std::string readText(fs::path const&p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void state(std::string const&status,std::wstring const&err=L""){
 atomicText(dataDir()/L"status.json","{\"status\":\""+status+"\",\"brokerPid\":"+std::to_string(GetCurrentProcessId())+
 ",\"workerPid\":"+std::to_string(child?GetProcessId(child):0)+",\"hotkeyMode\":\""+hotkeyMode+"\",\"file\":\""+escape(currentFile)+"\",\"error\":\""+escape(err)+"\"}\n");
}
void notify(wchar_t const*title,std::wstring const&text){
 tray.uFlags=NIF_INFO;wcscpy_s(tray.szInfoTitle,title);wcsncpy_s(tray.szInfo,text.c_str(),_TRUNCATE);tray.dwInfoFlags=NIIF_INFO;
 Shell_NotifyIconW(NIM_MODIFY,&tray);
}
bool freeSpace(fs::path const&p,ULONGLONG floor=MIN_FREE){ULARGE_INTEGER n{};return GetDiskFreeSpaceExW(p.c_str(),&n,nullptr,nullptr)&&n.QuadPart>=floor;}
std::wstring exePath(){std::wstring s(32768,L'\0');DWORD n=GetModuleFileNameW(nullptr,s.data(),DWORD(s.size()));s.resize(n);return s;}
std::wstring quote(std::wstring const&s){
 std::wstring r=L"\"";size_t slash=0;
 for(wchar_t c:s){if(c==L'\\'){++slash;continue;}if(c==L'"')r.append(slash*2+1,L'\\');else r.append(slash,L'\\');slash=0;r+=c;}
 r.append(slash*2,L'\\');return r+L"\"";
}
fs::path newOutput(){
 SYSTEMTIME t{};GetLocalTime(&t);wchar_t n[100];
 swprintf_s(n,L"Video_%04u%02u%02u_%02u%02u%02u_%03u.mp4",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
 fs::path p=downloads()/n;
 for(int i=1;fs::exists(p)||fs::exists(p.wstring()+L".partial.mp4");++i)p=downloads()/(fs::path(n).stem().wstring()+L"_"+std::to_wstring(i)+L".mp4");
 return p;
}
void badgeShow(bool show){
 if(!badge)badge=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"● 녹화 중 · Win+왼쪽 Alt+R 종료",
 WS_POPUP|SS_CENTER,20,20,320,28,broker,nullptr,GetModuleHandleW(nullptr),nullptr);
 SetWindowDisplayAffinity(badge,WDA_EXCLUDEFROMCAPTURE);
 ShowWindow(badge,show?SW_SHOWNOACTIVATE:SW_HIDE);
}
void requestStop(){if(child){SetEvent(stopEvent);state("stopping");}}
void toggle(){
 if(child){requestStop();return;}
 if(!freeSpace(downloads())){state("error",L"저장 공간이 2GiB 미만이거나 확인할 수 없습니다.");notify(L"녹화 시작 실패",L"다운로드 폴더의 여유 공간을 확인해 주세요.");return;}
 POINT pos{};GetCursorPos(&pos);HMONITOR monitor=MonitorFromPoint(pos,MONITOR_DEFAULTTOPRIMARY);
 currentFile=newOutput().wstring();ResetEvent(stopEvent);
 std::error_code readyError;fs::remove(dataDir()/L"worker-ready",readyError);
 HANDLE parentWatch=nullptr;
 check_hresult(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&parentWatch,SYNCHRONIZE,TRUE,0)?S_OK:HRESULT_FROM_WIN32(GetLastError()));
 std::wstring cmd=quote(exePath())+L" --worker --monitor "+std::to_wstring((uintptr_t)monitor)+L" --parent-handle "+
  std::to_wstring((uintptr_t)parentWatch)+L" --output "+quote(currentFile);
 STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);PROCESS_INFORMATION pi{};
 SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
 std::vector<BYTE> attributes(bytes);si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
 if(!InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&bytes)){CloseHandle(parentWatch);check_hresult(HRESULT_FROM_WIN32(GetLastError()));}
 if(!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&parentWatch,sizeof(parentWatch),nullptr,nullptr)){
  DWORD error=GetLastError();DeleteProcThreadAttributeList(si.lpAttributeList);CloseHandle(parentWatch);check_hresult(HRESULT_FROM_WIN32(error));
 }
 BOOL created=CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi);
 DeleteProcThreadAttributeList(si.lpAttributeList);CloseHandle(parentWatch);
 if(!created){
  state("error",L"녹화 프로세스 시작 실패");notify(L"녹화 시작 실패",L"프로세스를 시작하지 못했습니다.");return;
 }
 CloseHandle(pi.hThread);child=pi.hProcess;state("starting");badgeShow(true);
}
// Video processor converts/scales BGRA into NV12 on GPU. Only the encoded input
// crosses to CPU memory; no unbounded frame list or software video encoder.
struct Encoder {
 com_ptr<ID3D11Device> device;com_ptr<ID3D11DeviceContext> ctx;
 com_ptr<ID3D11VideoDevice> vd;com_ptr<ID3D11VideoContext> vc;
 com_ptr<ID3D11VideoProcessorEnumerator> ve;com_ptr<ID3D11VideoProcessor> vp;
 com_ptr<ID3D11Texture2D> nv, staging;
 com_ptr<ID3D11VideoProcessorOutputView> ov;
 com_ptr<IMFSinkWriter> writer;
 DWORD stream=0;UINT w=0,h=0;std::wstring encoderName;LONGLONG frame=0;
 Encoder(com_ptr<ID3D11Device> d,com_ptr<ID3D11DeviceContext> c,UINT iw,UINT ih,fs::path const&file):device(d),ctx(c){
  double scale=std::min(1.0,std::min(1920.0/iw,1080.0/ih));w=std::max(2u,UINT(iw*scale)&~1u);h=std::max(2u,UINT(ih*scale)&~1u);
  vd=device.as<ID3D11VideoDevice>();vc=ctx.as<ID3D11VideoContext>();
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};desc.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;desc.InputWidth=iw;desc.InputHeight=ih;
  desc.OutputWidth=w;desc.OutputHeight=h;desc.InputFrameRate={30,1};desc.OutputFrameRate={30,1};desc.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  check_hresult(vd->CreateVideoProcessorEnumerator(&desc,ve.put()));check_hresult(vd->CreateVideoProcessor(ve.get(),0,vp.put()));
  UINT support=0;check_hresult(ve->CheckVideoProcessorFormat(DXGI_FORMAT_NV12,&support));
  if(!(support&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))throw std::runtime_error("GPU NV12 conversion unavailable");
  D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_NV12;td.SampleDesc.Count=1;
  td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;check_hresult(device->CreateTexture2D(&td,nullptr,nv.put()));
  td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check_hresult(device->CreateTexture2D(&td,nullptr,staging.put()));
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};od.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
  check_hresult(vd->CreateVideoProcessorOutputView(nv.get(),ve.get(),&od,ov.put()));
  RECT src{0,0,LONG(iw),LONG(ih)}, dst{0,0,LONG(w),LONG(h)};
  vc->VideoProcessorSetStreamSourceRect(vp.get(),0,TRUE,&src);vc->VideoProcessorSetStreamDestRect(vp.get(),0,TRUE,&dst);
  vc->VideoProcessorSetOutputTargetRect(vp.get(),TRUE,&dst);
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};cs.RGB_Range=0;cs.YCbCr_Matrix=1;
  vc->VideoProcessorSetStreamColorSpace(vp.get(),0,&cs);cs.Nominal_Range=1;vc->VideoProcessorSetOutputColorSpace(vp.get(),&cs);
  MFT_REGISTER_TYPE_INFO in{MFMediaType_Video,MFVideoFormat_NV12},out{MFMediaType_Video,MFVideoFormat_H264};IMFActivate** acts=nullptr;UINT count=0;
  check_hresult(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,MFT_ENUM_FLAG_HARDWARE|MFT_ENUM_FLAG_SORTANDFILTER,&in,&out,&acts,&count));
  for(UINT i=0;i<count;++i)acts[i]->Release();CoTaskMemFree(acts);
  if(count==0)throw std::runtime_error("No hardware H.264 encoder registered; software fallback disabled");
  com_ptr<IMFAttributes> attr;check_hresult(MFCreateAttributes(attr.put(),4));check_hresult(attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE));
  // Some Windows builds reject DISABLE_CONVERTERS during sink creation.
  // NV12 input is already converted on GPU; selected encoder is still verified.
  check_hresult(attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE,MFTranscodeContainerType_MPEG4));
  com_ptr<IMFByteStream> bytes;
  check_hresult(MFCreateFile(MF_ACCESSMODE_WRITE,MF_OPENMODE_FAIL_IF_EXIST,MF_FILEFLAGS_NONE,file.c_str(),bytes.put()));
  check_hresult(MFCreateSinkWriterFromURL(nullptr,bytes.get(),attr.get(),writer.put()));
  com_ptr<IMFMediaType> output;check_hresult(MFCreateMediaType(output.put()));
  check_hresult(output->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));check_hresult(output->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264));
  check_hresult(output->SetUINT32(MF_MT_AVG_BITRATE,8000000));check_hresult(output->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive));
  check_hresult(MFSetAttributeSize(output.get(),MF_MT_FRAME_SIZE,w,h));check_hresult(MFSetAttributeRatio(output.get(),MF_MT_FRAME_RATE,30,1));
  check_hresult(MFSetAttributeRatio(output.get(),MF_MT_PIXEL_ASPECT_RATIO,1,1));check_hresult(writer->AddStream(output.get(),&stream));
  com_ptr<IMFMediaType> input;check_hresult(MFCreateMediaType(input.put()));check_hresult(input->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));
  check_hresult(input->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12));check_hresult(input->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive));
  check_hresult(MFSetAttributeSize(input.get(),MF_MT_FRAME_SIZE,w,h));check_hresult(MFSetAttributeRatio(input.get(),MF_MT_FRAME_RATE,30,1));
  check_hresult(MFSetAttributeRatio(input.get(),MF_MT_PIXEL_ASPECT_RATIO,1,1));check_hresult(writer->SetInputMediaType(stream,input.get(),nullptr));
  // ENABLE_HARDWARE is a preference, not proof. Inspect the selected encoder.
  com_ptr<IMFTransform> transform;check_hresult(writer->GetServiceForStream(stream,GUID_NULL,__uuidof(IMFTransform),transform.put_void()));
  com_ptr<IMFAttributes> ta;check_hresult(transform->GetAttributes(ta.put()));wchar_t* hw=nullptr;UINT length=0;
  HRESULT hardware=ta->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute,&hw,&length);
  if(FAILED(hardware)||!hw)throw std::runtime_error("Sink writer selected a software encoder; recording rejected");
  CoTaskMemFree(hw);wchar_t* name=nullptr;
  if(SUCCEEDED(ta->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,&name,&length))&&name){encoderName=name;CoTaskMemFree(name);}
  else encoderName=L"verified hardware H.264";
  check_hresult(writer->BeginWriting());
 }
 void write(ID3D11Texture2D* texture){
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};id.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
  com_ptr<ID3D11VideoProcessorInputView> iv;check_hresult(vd->CreateVideoProcessorInputView(texture,ve.get(),&id,iv.put()));
  D3D11_VIDEO_PROCESSOR_STREAM s{};s.Enable=TRUE;s.pInputSurface=iv.get();check_hresult(vc->VideoProcessorBlt(vp.get(),ov.get(),0,1,&s));
  ctx->CopyResource(staging.get(),nv.get());D3D11_MAPPED_SUBRESOURCE mapped{};check_hresult(ctx->Map(staging.get(),0,D3D11_MAP_READ,0,&mapped));
  com_ptr<IMFMediaBuffer> buf;HRESULT hr=MFCreateMemoryBuffer(w*h*3/2,buf.put());if(FAILED(hr)){ctx->Unmap(staging.get(),0);check_hresult(hr);}
  BYTE* data=nullptr;hr=buf->Lock(&data,nullptr,nullptr);if(FAILED(hr)){ctx->Unmap(staging.get(),0);check_hresult(hr);}
  for(UINT y=0;y<h;++y)memcpy(data+y*w,(BYTE*)mapped.pData+y*mapped.RowPitch,w);
  for(UINT y=0;y<h/2;++y)memcpy(data+w*h+y*w,(BYTE*)mapped.pData+(h+y)*mapped.RowPitch,w);
  buf->Unlock();ctx->Unmap(staging.get(),0);check_hresult(buf->SetCurrentLength(w*h*3/2));
  com_ptr<IMFSample> sample;check_hresult(MFCreateSample(sample.put()));check_hresult(sample->AddBuffer(buf.get()));
  LONGLONG t=frame*10000000/30, end=(frame+1)*10000000/30;check_hresult(sample->SetSampleTime(t));check_hresult(sample->SetSampleDuration(end-t));
  check_hresult(writer->WriteSample(stream,sample.get()));++frame;
 }
 void finish(){if(frame==0)throw std::runtime_error("No captured frames");check_hresult(writer->Finalize());writer=nullptr;}
};
int worker(HMONITOR monitor,fs::path output,int seconds,HANDLE parentWatch){
 HANDLE stop=OpenEventW(SYNCHRONIZE,FALSE,STOPNAME);if(!stop||!parentWatch){if(stop)CloseHandle(stop);if(parentWatch)CloseHandle(parentWatch);return 2;}
 fs::path partial=output.wstring()+L".partial.mp4";ULONGLONG frames=0,dropped=0;std::wstring name,error;bool ok=false;
 try{
  if(!freeSpace(output.parent_path()))throw std::runtime_error("Less than 2GiB free");
  init_apartment(apartment_type::multi_threaded);check_hresult(MFStartup(MF_VERSION));
  if(!GraphicsCaptureSession::IsSupported())throw std::runtime_error("Screen capture unsupported");
  MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(monitor,&mi))throw std::runtime_error("Monitor disappeared");
  com_ptr<IDXGIFactory1> factory;check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1),factory.put_void()));
  com_ptr<IDXGIAdapter1> chosen;
  for(UINT ai=0;;++ai){com_ptr<IDXGIAdapter1> a;if(factory->EnumAdapters1(ai,a.put())==DXGI_ERROR_NOT_FOUND)break;
   for(UINT oi=0;;++oi){com_ptr<IDXGIOutput> o;if(a->EnumOutputs(oi,o.put())==DXGI_ERROR_NOT_FOUND)break;DXGI_OUTPUT_DESC d{};o->GetDesc(&d);if(d.Monitor==monitor){chosen=a;break;}}
   if(chosen)break;
  }
  com_ptr<ID3D11Device> d;com_ptr<ID3D11DeviceContext> c;D3D_FEATURE_LEVEL level;
  check_hresult(D3D11CreateDevice(chosen.get(),chosen?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE,nullptr,
   D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,d.put(),&level,c.put()));
  auto interop=get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();GraphicsCaptureItem item{nullptr};
  check_hresult(interop->CreateForMonitor(monitor,guid_of<GraphicsCaptureItem>(),put_abi(item)));
  com_ptr<IDXGIDevice> dxgi=d.as<IDXGIDevice>();com_ptr<IInspectable> insp;check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),insp.put()));
  auto wd=insp.as<IDirect3DDevice>();auto size=item.Size();
  auto pool=Direct3D11CaptureFramePool::CreateFreeThreaded(wd,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
  auto session=pool.CreateCaptureSession(item);session.IsCursorCaptureEnabled(true);
  Encoder encoder(d,c,size.Width,size.Height,partial);name=encoder.encoderName;
  std::mutex m;std::condition_variable ready;Direct3D11CaptureFrame latest{nullptr};std::atomic<bool> closing{false},gone{false};
  auto token=pool.FrameArrived([&](auto const&p,auto const&){try{auto f=p.TryGetNextFrame();if(f){std::lock_guard<std::mutex> lock(m);if(closing){f.Close();return;}if(latest){latest.Close();++dropped;}latest=f;ready.notify_one();}}catch(...){gone=true;ready.notify_one();}});
  auto closed=item.Closed([&](auto const&,auto const&){gone=true;ready.notify_one();});
  session.StartCapture();
  std::ofstream(dataDir()/L"worker-ready")<<GetCurrentProcessId();
  com_ptr<ID3D11Texture2D> cached;
  auto start=std::chrono::steady_clock::now(), next=start;
  try{
   while(WaitForSingleObject(stop,0)==WAIT_TIMEOUT&&WaitForSingleObject(parentWatch,0)==WAIT_TIMEOUT&&!gone){
    if(seconds>0&&std::chrono::steady_clock::now()-start>=std::chrono::seconds(seconds))break;
    if(encoder.frame%30==0&&!freeSpace(output.parent_path()))break;
    Direct3D11CaptureFrame f{nullptr};
    {std::unique_lock<std::mutex> lock(m);if(!latest&&!cached)ready.wait_for(lock,std::chrono::milliseconds(500));f=latest;latest=nullptr;}
    if(f){
     if(f.ContentSize().Width!=size.Width||f.ContentSize().Height!=size.Height){f.Close();break;}
     auto access=f.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
     com_ptr<ID3D11Texture2D> texture;check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),texture.put_void()));
     if(!cached){D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);td.BindFlags=D3D11_BIND_RENDER_TARGET;td.MiscFlags=0;check_hresult(d->CreateTexture2D(&td,nullptr,cached.put()));}
     c->CopyResource(cached.get(),texture.get());f.Close();
    }
    if(cached){encoder.write(cached.get());next+=std::chrono::microseconds(33333);std::this_thread::sleep_until(next);}
   }
  }catch(...){closing=true;session.Close();pool.FrameArrived(token);item.Closed(closed);pool.Close();throw;}
  closing=true;session.Close();pool.FrameArrived(token);item.Closed(closed);
  {std::lock_guard<std::mutex> lock(m);if(latest){latest.Close();latest=nullptr;}}pool.Close();
  encoder.finish();frames=encoder.frame;
  if(!MoveFileExW(partial.c_str(),output.c_str(),MOVEFILE_WRITE_THROUGH))throw hresult_error(HRESULT_FROM_WIN32(GetLastError()));
  ok=true;
 }catch(hresult_error const&e){std::wstringstream s;s<<e.message().c_str()<<L" (0x"<<std::hex<<unsigned(e.code())<<L") "<<to_hstring(failedCall).c_str();error=s.str();}
 catch(std::exception const&e){error=to_hstring(e.what()).c_str();}
 CloseHandle(stop);CloseHandle(parentWatch);MFShutdown();
 atomicText(dataDir()/L"last-recording.json","{\"success\":"+std::string(ok?"true":"false")+",\"frames\":"+std::to_string(frames)+
 ",\"droppedCaptureFrames\":"+std::to_string(dropped)+",\"hardwareEncoder\":\""+escape(name)+"\",\"file\":\""+escape(ok?output.wstring():partial.wstring())+"\",\"error\":\""+escape(error)+"\"}\n");
 return ok?0:1;
}
void poll(){
 if(!child)return;
 if(WaitForSingleObject(child,0)!=WAIT_OBJECT_0){
  state(WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0?"stopping":fs::exists(dataDir()/L"worker-ready")?"recording":"starting");return;}
 DWORD code=1;GetExitCodeProcess(child,&code);CloseHandle(child);child=nullptr;badgeShow(false);
 if(code==0){state("idle");notify(L"녹화 저장 완료",currentFile);}
 else {state("error",L"녹화 실패. last-recording.json에 원인을 기록했습니다.");notify(L"녹화 실패",L"소프트웨어 인코딩으로 전환하지 않았습니다. 오류 기록을 확인해 주세요.");}
 if(exitWhenDone)PostQuitMessage(0);
}
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM w,LPARAM l){
 try{
  if(msg==WM_HOTKEY){if(!(GetAsyncKeyState(VK_LMENU)&0x8000))return 0;toggle();return 0;}
  if(msg==TOGGLE){toggle();return 0;}if(msg==STOP){requestStop();return 0;}
  if(msg==EXIT||msg==WM_CLOSE){exitWhenDone=true;if(child)requestStop();else PostQuitMessage(0);return 0;}
  if(msg==WM_TIMER){poll();return 0;}
  if(msg==WM_WTSSESSION_CHANGE&&w==WTS_SESSION_LOCK){requestStop();return 0;}
  if(msg==WM_POWERBROADCAST&&w==PBT_APMSUSPEND){requestStop();return TRUE;}
  if(msg==WM_QUERYENDSESSION){requestStop();return TRUE;}
  if(msg==WM_DISPLAYCHANGE){if(child){POINT p{};GetCursorPos(&p);requestStop();}return 0;}
  if(msg==WM_APP+10&&l==WM_RBUTTONUP){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,child?L"녹화 종료":L"녹화 시작");AppendMenuW(menu,MF_STRING,2,L"수신기 종료");
   POINT p;GetCursorPos(&p);SetForegroundWindow(h);int n=TrackPopupMenu(menu,TPM_RETURNCMD,p.x,p.y,0,h,nullptr);DestroyMenu(menu);if(n==1)toggle();if(n==2)PostMessageW(h,EXIT,0,0);return 0;}
 }catch(std::exception const&e){try{state("error",to_hstring(e.what()).c_str());}catch(...){}}
 catch(hresult_error const&e){try{state("error",e.message().c_str());}catch(...){}}
 return DefWindowProcW(h,msg,w,l);
}
LRESULT CALLBACK hotkeyHook(int code,WPARAM w,LPARAM l){
 if(code==HC_ACTION){
  auto key=reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
  if(key->vkCode=='R'){
   bool down=w==WM_KEYDOWN||w==WM_SYSKEYDOWN;
   bool up=w==WM_KEYUP||w==WM_SYSKEYUP;
   if(up&&rHeld){rHeld=false;return 1;}
   if(down&&rHeld)return 1;
   bool win=(GetAsyncKeyState(VK_LWIN)|GetAsyncKeyState(VK_RWIN))&0x8000;
   if(down&&win&&(GetAsyncKeyState(VK_LMENU)&0x8000)&&!(GetAsyncKeyState(VK_CONTROL)&0x8000)&&!(GetAsyncKeyState(VK_SHIFT)&0x8000)){
    rHeld=true;PostMessageW(broker,TOGGLE,0,0);return 1;
   }
  }
 }
 return CallNextHookEx(keyboardHook,code,w,l);
}
int runBroker(){
 HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\AchmageRecorderBrokerMutex01");
 if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mutex);return 0;}
 stopEvent=CreateEventW(nullptr,TRUE,FALSE,STOPNAME);if(!stopEvent){CloseHandle(mutex);return 2;}
 WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=CLASSNAME;RegisterClassW(&wc);
 // Invisible ordinary window receives power/display/session notifications.
 broker=CreateWindowExW(WS_EX_TOOLWINDOW,CLASSNAME,L"Achmage Recorder",WS_POPUP,0,0,0,0,nullptr,nullptr,wc.hInstance,nullptr);
 tray.cbSize=sizeof(tray);tray.hWnd=broker;tray.uID=1;tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;tray.uCallbackMessage=WM_APP+10;
 tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wcscpy_s(tray.szTip,L"Achmage Recorder · Win+왼쪽 Alt+R");Shell_NotifyIconW(NIM_ADD,&tray);
 if(RegisterHotKey(broker,1,MOD_WIN|MOD_ALT|MOD_NOREPEAT,'R')){hotkeyMode="registered";state("idle");}
 else{
  // Windows can retain reserved Game Bar hotkeys after package removal.
  // This fallback recognizes only Win+LEFT Alt+R. No key data is retained.
  keyboardHook=SetWindowsHookExW(WH_KEYBOARD_LL,hotkeyHook,GetModuleHandleW(nullptr),0);
  if(keyboardHook){hotkeyMode="specific-chord-hook";state("idle");}
  else{state("error",L"녹화 단축키를 연결하지 못했습니다.");notify(L"단축키 등록 실패",L"toggle 명령으로 녹화할 수 있습니다.");}
 }
 WTSRegisterSessionNotification(broker,NOTIFY_FOR_THIS_SESSION);SetTimer(broker,1,250,nullptr);
 MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
 UnregisterHotKey(broker,1);if(keyboardHook)UnhookWindowsHookEx(keyboardHook);WTSUnRegisterSessionNotification(broker);Shell_NotifyIconW(NIM_DELETE,&tray);
 if(badge)DestroyWindow(badge);DestroyWindow(broker);CloseHandle(stopEvent);ReleaseMutex(mutex);CloseHandle(mutex);state("offline");return 0;
}
HWND findBroker(){return FindWindowW(CLASSNAME,L"Achmage Recorder");}
fs::path startupLink(){
 PWSTR s=nullptr;check_hresult(SHGetKnownFolderPath(FOLDERID_Startup,0,nullptr,&s));
 fs::path p=fs::path(s)/L"Achmage Recorder.lnk";CoTaskMemFree(s);return p;
}
int install(bool remove){
 if(HWND h=findBroker()){PostMessageW(h,EXIT,0,0);for(int i=0;i<60&&findBroker();++i)Sleep(500);if(findBroker())return 4;}
 auto target=dataDir()/L"bin"/L"AchmageRecorder.exe";
 if(remove){
  std::error_code e;fs::remove(startupLink(),e);e.clear();fs::remove(target,e);
  return e?1:0; // recordings, settings and diagnostics remain
 }
 fs::create_directories(target.parent_path());
 if(fs::path(exePath())!=target&&!CopyFileW(exePath().c_str(),target.c_str(),FALSE))check_hresult(HRESULT_FROM_WIN32(GetLastError()));
 init_apartment(apartment_type::single_threaded);
 com_ptr<IShellLinkW> link;check_hresult(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IShellLinkW),link.put_void()));
 check_hresult(link->SetPath(target.c_str()));check_hresult(link->SetArguments(L"--broker"));check_hresult(link->SetDescription(L"Achmage recording hotkey"));
 check_hresult(link->SetWorkingDirectory(target.parent_path().c_str()));link->SetShowCmd(SW_HIDE);
 auto pf=link.as<IPersistFile>();check_hresult(pf->Save(startupLink().c_str(),TRUE));
 std::wstring cmd=quote(target.wstring())+L" --broker";STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
 if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return 2;
 CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return 0;
}
int verify(fs::path const&file){
 init_apartment(apartment_type::multi_threaded);check_hresult(MFStartup(MF_VERSION));
 com_ptr<IMFSourceReader> reader;check_hresult(MFCreateSourceReaderFromURL(file.c_str(),nullptr,reader.put()));
 com_ptr<IMFMediaType> type;check_hresult(MFCreateMediaType(type.put()));type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12);
 check_hresult(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,type.get()));reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE);reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE);
 UINT64 frames=0;LONGLONG first=0,last=0;
 while(true){DWORD flags=0;LONGLONG time=0;com_ptr<IMFSample> sample;
  check_hresult(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&time,sample.put()));
  if(flags&MF_SOURCE_READERF_ERROR)throw std::runtime_error("Decode error");
  if(sample){if(!frames)first=time;last=time;++frames;}
  if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
 }
 std::printf("{\"decodedFrames\":%llu,\"seconds\":%.3f}\n",frames,double(last-first)/10000000.0+1.0/30);
 reader=nullptr;MFShutdown();return frames?0:1;
}
int wmain(int argc,wchar_t**argv){
 try{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  std::wstring command=argc>1?argv[1]:L"--broker";
  if(command==L"install")return install(false);
  if(command==L"uninstall")return install(true);
  if(command==L"verify"&&argc==3)return verify(argv[2]);
  if(command==L"--broker")return runBroker();
  if(command==L"--worker"){
   HMONITOR mon=nullptr;fs::path out;int seconds=0;HANDLE parentWatch=nullptr;
   for(int i=2;i+1<argc;++i){std::wstring a=argv[i];if(a==L"--monitor")mon=(HMONITOR)_wcstoui64(argv[++i],nullptr,10);else if(a==L"--parent-handle")parentWatch=(HANDLE)_wcstoui64(argv[++i],nullptr,10);else if(a==L"--output")out=argv[++i];else if(a==L"--seconds")seconds=_wtoi(argv[++i]);}
   if(!mon||out.empty())return 2;return worker(mon,out,seconds,parentWatch);
  }
  if(command==L"status"){std::string s=readText(dataDir()/L"status.json");if(s.empty())s="{\"status\":\"offline\"}";std::fputs(s.c_str(),stdout);return findBroker()?0:3;}
  if(command==L"inspect"){
   init_apartment(apartment_type::multi_threaded);check_hresult(MFStartup(MF_VERSION));
   MFT_REGISTER_TYPE_INFO in{MFMediaType_Video,MFVideoFormat_NV12},out{MFMediaType_Video,MFVideoFormat_H264};IMFActivate** a=nullptr;UINT n=0;
   check_hresult(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,MFT_ENUM_FLAG_HARDWARE|MFT_ENUM_FLAG_SORTANDFILTER,&in,&out,&a,&n));
   std::printf("{\"hardwareEncoders\":%u,\"captureSupported\":%s}\n",n,GraphicsCaptureSession::IsSupported()?"true":"false");for(UINT i=0;i<n;++i)a[i]->Release();CoTaskMemFree(a);MFShutdown();return n?0:1;
  }
  HWND h=findBroker();
  if(command==L"toggle"&&!h){std::wstring cmd=quote(exePath())+L" --broker";STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
   if(!CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return 2;CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
   for(int i=0;i<40&&!h;++i){Sleep(50);h=findBroker();}
  }
  if(!h)return command==L"stop"||command==L"exit"?0:2;
  if(command==L"toggle")PostMessageW(h,TOGGLE,0,0);else if(command==L"stop")PostMessageW(h,STOP,0,0);else if(command==L"exit")PostMessageW(h,EXIT,0,0);else return 2;
  return 0;
 }catch(hresult_error const&e){std::fprintf(stderr,"HRESULT 0x%08x: %s\n",unsigned(e.code()),to_string(e.message()).c_str());return 1;}
 catch(std::exception const&e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
