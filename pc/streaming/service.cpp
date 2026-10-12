#include "service.h"
#include "json.h"
#include "win_text.h"
#include "openvr.h"
#include "log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
namespace fph { namespace pc { namespace streaming {
namespace {
double clockSec() {return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
class ProcessMemory : public Memory {
public:
    HANDLE handle=nullptr;
    ~ProcessMemory() override {close();}
    void close() {if(handle) CloseHandle(handle);handle=nullptr;}
    bool read(uint64_t a,void* dest,size_t n) override {
        if(!handle||a<0x10000||a>=0x800000000000ull||n>0x200000||n>0x800000000000ull-a) return false;
        SIZE_T got=0; return ReadProcessMemory(handle,reinterpret_cast<const void*>(a),dest,n,&got)&&got==n;
    }
};
DWORD serverPid() {
    const HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snap==INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e{};e.dwSize=sizeof(e);DWORD pid=0;
    if(Process32FirstW(snap,&e)) do {if(_wcsicmp(e.szExeFile,L"vrserver.exe")==0) {pid=e.th32ProcessID;break;}}while(Process32NextW(snap,&e));
    CloseHandle(snap);return pid;
}
bool modulePath(ProcessMemory& m,uint64_t& base,std::wstring& path) {
    struct Pbi {void* exitStatus;void* peb;void* affinity;void* priority;void* pid;void* inherited;};
    using Query=LONG(NTAPI*)(HANDLE,ULONG,void*,ULONG,ULONG*);
    const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess"));
    Pbi info{};ULONG length=0;uint64_t ldr=0,cur=0;
    if(!query||query(m.handle,0,&info,sizeof(info),&length)!=0||!m.get(reinterpret_cast<uint64_t>(info.peb)+0x18,ldr)||!m.get(ldr+0x20,cur)) return false;
    const uint64_t head=ldr+0x20;std::set<uint64_t> seen;
    while(cur!=head&&seen.size()<256) {
        if(cur<0x10010||!seen.insert(cur).second) return false;
        uint8_t entry[0x70];if(!m.read(cur-0x10,entry,sizeof(entry))) return false;
        uint64_t name=0,addr=0;uint16_t len=0;
        std::memcpy(&addr,entry+0x30,8); std::memcpy(&len,entry+0x48,2);std::memcpy(&name,entry+0x50,8);
        if(len>0&&len<=32766&&len%2==0) {
            std::wstring full(len/2,L'\0');
            if(!m.read(name,full.data(),len)) return false;
            if(_wcsicmp(std::filesystem::path(full).filename().c_str(),L"driver_vrlink.dll")==0) {base=addr;path=full;return true;}
        }
        std::memcpy(&cur,entry+0x10,8);
    }
    return false;
}
std::wstring runtimePath() {
    wchar_t local[32768];const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);if(!n||n>=32768) return {};
    std::ifstream in(std::filesystem::path(local)/L"openvr"/L"openvrpaths.vrpath",std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)),{}),error;JsonValue json;
    if(!parseJson(data,json,error)) return {};
    const auto* paths=json.get("runtime");if(!paths||paths->items.empty()||!paths->items[0].isString()) return {};
    return widen(paths->items[0].text);
}
std::wstring logPath() {
    wchar_t steam[32768]{};DWORD bytes=sizeof(steam);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,steam,&bytes)!=ERROR_SUCCESS) return {};
    return (std::filesystem::path(steam)/L"logs"/L"driver_vrlink.txt").wstring();
}
class Tail {
public:
    LogParser parser;
    std::wstring path=logPath();
    void tick(MetricStore& s,double now) {
        using namespace metric;
        const uint16_t ids[]={kPcStreamStreamResets,kPcStreamBadLinks,kPcStreamEncoderReinits,kPcStreamThrottleEvents};
        const HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE) {for(auto id:ids)s.setNa(id,Reason::ReadFailed,now);return;}
        BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        if(!GetFileInformationByHandle(h,&info)||!GetFileSizeEx(h,&size)) {CloseHandle(h);return;}
        const uint64_t id=static_cast<uint64_t>(info.nFileIndexHigh)<<32|info.nFileIndexLow;
        if(!initialized_||id!=id_||volume_!=info.dwVolumeSerialNumber||static_cast<uint64_t>(size.QuadPart)<offset_) {offset_=0;parser.rewritten();initialized_=true;}
        id_=id;volume_=info.dwVolumeSerialNumber;
        LARGE_INTEGER pos{};pos.QuadPart=offset_;
        if(SetFilePointerEx(h,pos,nullptr,FILE_BEGIN)) {
            char data[16384];size_t budget=256*1024;DWORD n=0;
            while(budget&&ReadFile(h,data,static_cast<DWORD>(std::min(budget,sizeof(data))),&n,nullptr)&&n) {parser.feed(data,n);offset_+=n;budget-=n;}
        }
        CloseHandle(h);const auto& e=parser.events();
        const uint32_t vals[]={e.resets,e.badLinks,e.encoderReinits,e.throttles};for(unsigned j=0;j<4;++j)s.setI32(ids[j],static_cast<int32_t>(vals[j]),now);
    }
private:
    bool initialized_=false;uint64_t id_=0,offset_=0;DWORD volume_=0;
};
// No link to openvr_api.lib: resolve SteamVR's installed DLL's public exports at runtime.
class PublicVr {
public:
    double hz=0;
    bool connected() const {return connected_;}
    ~PublicVr(){shutdown();if(dll_)FreeLibrary(dll_);}
    void shutdown() {if(connected_&&shutdown_)shutdown_();connected_=false;system_=nullptr;apps_=nullptr;timings_=nullptr;hz=0;}
    void poll(double now,bool wanted,bool haveServer,MetricStore& out) {
        using namespace metric;
        if(connected_) {
            vr::VREvent_t e{};
            while(system_->PollNextEvent(&e,sizeof(e))) {
                if(e.eventType==vr::VREvent_Quit) {shutdown();logLine("[stream] VREvent_Quit: VR_ShutdownInternal completed");nextTry_=now+5;out.setNa(kPcStreamPing,Reason::NoSteamVr,now);return;}
                if(e.eventType==vr::VREvent_SceneApplicationChanged) scene(out,now);
            }
        }
        if(!wanted) {shutdown();return;}
        if(!connected_&&haveServer&&now>=nextTry_) {nextTry_=now+5;connect(out,now);}
    }
    void sample(MetricStore& out,double now) {
        using namespace metric;
        if(!connected_||!timings_) {out.setNa(kPcStreamPing,Reason::NoSteamVr,now);return;}
        vr::Compositor_FrameTiming t{}; t.m_nSize=sizeof(t);
        const auto count=timings_(&t,1);
        if(count&&std::isfinite(t.m_flTransferLatencyMs)&&t.m_flTransferLatencyMs>=0&&t.m_flTransferLatencyMs<=500)
            out.setF32(kPcStreamPing,t.m_flTransferLatencyMs,now);
        else out.setNa(kPcStreamPing,Reason::NoFrames,now);
        if(hz>0)out.setF32(kPcStreamDisplayHz,hz,now);
    }
private:
    using Init=uint32_t(__cdecl*)(vr::EVRInitError*,vr::EVRApplicationType,const char*);
    using Shutdown=void(__cdecl*)();using Interface=void*(__cdecl*)(const char*,vr::EVRInitError*);
    using Timings=uint32_t(__stdcall*)(vr::Compositor_FrameTiming*,uint32_t);
    HMODULE dll_=nullptr; Init init_=nullptr;Shutdown shutdown_=nullptr;Interface interface_=nullptr;Timings timings_=nullptr;
    vr::IVRSystem* system_=nullptr;vr::IVRApplications* apps_=nullptr;bool connected_=false;double nextTry_=0;
    void connect(MetricStore& out,double now) {
        if(!dll_) {
            const auto path=runtimePath();if(path.empty())return;
            const auto dllPath=std::filesystem::path(path)/L"bin"/L"win64"/L"openvr_api.dll";
            dll_=LoadLibraryExW(dllPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if(!dll_)return;
            init_=reinterpret_cast<Init>(GetProcAddress(dll_,"VR_InitInternal2"));shutdown_=reinterpret_cast<Shutdown>(GetProcAddress(dll_,"VR_ShutdownInternal"));interface_=reinterpret_cast<Interface>(GetProcAddress(dll_,"VR_GetGenericInterface"));
        }
        if(!init_||!shutdown_||!interface_)return;
        vr::EVRInitError error=vr::VRInitError_None;init_(&error,vr::VRApplication_Background,nullptr);if(error!=vr::VRInitError_None)return;
        connected_=true;
        system_=static_cast<vr::IVRSystem*>(interface_(vr::IVRSystem_Version,&error));
        if(!system_||error!=vr::VRInitError_None){shutdown();return;}
        auto** table=static_cast<void**>(interface_("FnTable:IVRCompositor_029",&error));
        if(table&&error==vr::VRInitError_None) timings_=reinterpret_cast<Timings>(table[11]);
        apps_=static_cast<vr::IVRApplications*>(interface_(vr::IVRApplications_Version,&error));
        if(error!=vr::VRInitError_None)apps_=nullptr;
        hz=system_->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,vr::Prop_DisplayFrequency_Float);
        logLine("[stream] OpenVR background connected, display %.1f Hz",hz);
        scene(out,now);
    }
    void scene(MetricStore& out,double now) {
        using namespace metric;
        if(!apps_)return;const uint32_t pid=apps_->GetCurrentSceneProcessId();char key[512]{},name[512]{};
        out.setI32(kPcStreamScenePid,static_cast<int32_t>(pid),now);
        if(pid&&apps_->GetApplicationKeyByProcessId(pid,key,sizeof(key))==vr::VRApplicationError_None) {
            apps_->GetApplicationPropertyString(key,vr::VRApplicationProperty_Name_String,name,sizeof(name));
            out.setText(kPcStreamSceneKey,key,now);out.setText(kPcStreamSceneName,name,now);
        } else {out.setText(kPcStreamSceneKey,"",now);out.setText(kPcStreamSceneName,"SteamVR",now);}
    }
};
static_assert(sizeof(vr::Compositor_FrameTiming)==192,"OpenVR Windows timing ABI changed");
}
struct Service::Runtime {
    ProcessMemory memory;PublicVr vr;Tail log;
    std::unique_ptr<Reader> reader;
    MetricStore store;
    DWORD pid=0;double nextFind=0,nextModule=0;
    uint64_t lastResets=0;std::string lastNotes;
    bool mismatch=false; uint64_t cachedSize=0;uint32_t cachedStamp=0;Anchors cachedAnchors;
    explicit Runtime(bool force):mismatch(force){}
    void closeReader(){if(pid)logLine("[stream] vrserver reader detached (pid %lu)",pid);reader.reset();memory.close();pid=0;nextModule=0;lastResets=0;lastNotes.clear();}
    void tick(double now) {
        using namespace metric;
        if(memory.handle) {DWORD code=0;if(!GetExitCodeProcess(memory.handle,&code)||code!=STILL_ACTIVE)closeReader();}
        if(!memory.handle&&now>=nextFind) {
            nextFind=now+5;pid=serverPid();
            if(pid) memory.handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,FALSE,pid);
            if(memory.handle)logLine("[stream] read-only vrserver handle opened (pid %lu)",pid);
        }
        if(!memory.handle) {
            // Public OpenVR remains independent of access to vrserver's memory.
            if(!vr.connected())setPcStreamNa(store,Reason::NoSteamVr,now);
            else for(const auto& d:registry()) if(isPcStreamMetric(d.id)&&d.id>=1021&&d.id<1080)store.setNa(d.id,Reason::ReadFailed,now);
            vr.sample(store,now);log.tick(store,now);return;
        }
        if(!reader&&now>=nextModule) {
            nextModule=now+10;
            uint64_t base=0;std::wstring path;
            if(modulePath(memory,base,path)) {
                std::ifstream in(std::filesystem::path(path),std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});
                if(!bytes.empty()&&bytes.size()<64*1024*1024) {
                    uint32_t stamp=0;
                    if(bytes.size()>=64) {uint32_t pe=0;std::memcpy(&pe,bytes.data()+0x3c,4);if(pe+12ull<=bytes.size())std::memcpy(&stamp,bytes.data()+pe+8,4);}
                    if(cachedSize!=bytes.size()||cachedStamp!=stamp) {
                        cachedAnchors=findAnchors(bytes,mismatch?std::string(kCsvHeader)+"forced mismatch":kCsvHeader);cachedSize=bytes.size();cachedStamp=stamp;
                    }
                    if(sameLoadedBuild(memory,base,cachedAnchors)) {reader=std::make_unique<Reader>(memory,base,cachedAnchors);logLine("[stream] driver build %u anchors L1=%d L2=%d L3=%d",cachedAnchors.timestamp,cachedAnchors.l1,cachedAnchors.l2,cachedAnchors.l3);}
                }
            }
        }
        vr.sample(store,now);
        if(reader) {
            const double before=clockSec();reader->tick(now,vr.hz,store);store.setF32(kPcStreamReadMs,(clockSec()-before)*1000,now);
            if(lastResets!=reader->resets()) {lastResets=reader->resets();logLine("[stream] owner/index changed: caches reset, generation %llu",static_cast<unsigned long long>(lastResets));}
            std::string notes;
            for(const auto id:{kPcStreamL1Note,kPcStreamL2Note,kPcStreamL3Note}) {const auto* v=store.get(id);notes+=(v&&v->ok?v->text:"unavailable")+";";}
            if(notes!=lastNotes) {lastNotes=notes;logLine("[stream] layer status %s (build %u)",notes.c_str(),cachedAnchors.timestamp);}
        } else {
            for(const auto& d:registry()) if(isPcStreamMetric(d.id)&&d.id>=1021&&d.id<=1078)store.setNa(d.id,Reason::SteamVrChanged,now);
            store.setText(kPcStreamL1Note,"module/build unavailable",now);store.setText(kPcStreamL2Note,"module/build unavailable",now);store.setText(kPcStreamL3Note,"module/build unavailable",now);
        }
        log.tick(store,now);
    }
};
Service::Service(bool force):runtime_(std::make_unique<Runtime>(force)) {
    // Every member (including the atomics, mutex and CV) must exist before run() can access it.
    worker_=std::thread([this]{run();});
}
Service::~Service(){stop();}
void Service::setDemand(bool value){if(demand_.exchange(value)!=value)wake_.notify_all();}
void Service::stop(){stop_=true;wake_.notify_all();if(worker_.joinable())worker_.join();}
bool Service::take(MetricStore& store,double now) {
    std::lock_guard<std::mutex> lock(mutex_);if(taken_==generation_)return false;
    for(const auto& v:snapshot_)link::storeWire(store,v,now);taken_=generation_;return true;
}
void Service::run() {
    double nextTick=0;bool wasWanted=false;
    while(!stop_) {
        const double now=clockSec();const bool wanted=demand_;
        runtime_->vr.poll(now,wanted,runtime_->pid!=0,runtime_->store);
        if(wanted&&now>=nextTick) {
            nextTick=now+0.5;runtime_->tick(now);
            runtime_->vr.poll(clockSec(),true,runtime_->pid!=0,runtime_->store);
            std::lock_guard<std::mutex> lock(mutex_);snapshot_=pcStreamValues(runtime_->store);++generation_;
        } else if(!wanted&&wasWanted) {
            runtime_->closeReader();setPcStreamNa(runtime_->store,Reason::Paused,now);
            std::lock_guard<std::mutex> lock(mutex_);snapshot_=pcStreamValues(runtime_->store);++generation_;
        }
        wasWanted=wanted;
        std::unique_lock<std::mutex> lock(mutex_);
        if(wanted)wake_.wait_for(lock,std::chrono::milliseconds(50));
        else wake_.wait(lock,[this]{return stop_||demand_;});
    }
    runtime_->vr.shutdown();runtime_->closeReader();
}
}}}
