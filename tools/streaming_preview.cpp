// Offline visual evidence for the shared renderer. These values are simulated, never a live measurement.
#include "grid_renderer.h"
#include "stream_metrics.h"
#include <filesystem>
#include <cstdio>
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    std::filesystem::create_directories(std::filesystem::u8path(argv[1]));
    FontSet fonts;fonts.load(FPH_FONT_DIR "/NotoSans-Regular.ttf",FPH_FONT_DIR "/NotoSans-Bold.ttf");
    using namespace fph;using namespace metric;
    for(unsigned scenario=0;scenario<4;++scenario) {
        MetricStore store;
        store.setEnum(kStreamLinkMode,static_cast<uint32_t>(StreamLinkMode::Multi),1);
        store.setI32(kStreamFecRetries,3,1);store.setI32(kStreamFecRetryPackets,21,1);
        setPcStreamNa(store,Reason::PcAppNotRunning,1);
        if(scenario!=0) {
            for(const auto& d:registry()) if(isPcStreamMetric(d.id)) {
                if(d.type==WireType::F32)store.setF32(d.id,0,1);
                else if(d.type==WireType::I32)store.setI32(d.id,0,1);
                else if(d.type==WireType::ShortString)store.setText(d.id,"",1);
            }
            store.setF32(kPcStreamPing,2.4,1);store.setF32(kPcStreamFps,72,1);
            store.setF32(kPcStreamWireMbps,151.2,1);store.setF32(kPcStreamVideoMbps,125.4,1);
            store.setF32(kPcStreamFecMbps,17.8,1);store.setF32(kPcStreamAudioMbps,3.4,1);
            store.setF32(kPcStreamRetryMbps,3.2,1);store.setF32(kPcStreamLogicMbps,1.4,1);
            const double latency[]={1.1,2.6,0.2,7.8,8.3,37.2,57.2};
            for(unsigned j=0;j<7;++j) {store.setF32(static_cast<uint16_t>(1040+2*j),latency[j],1);store.setF32(static_cast<uint16_t>(1041+2*j),latency[j]+0.8,1);}
            store.setF32(kPcStreamNamedEncode,2.7,1);store.setF32(kPcStreamNamedSend,0.2,1);
            store.setF32(kPcStreamEstimatedBandwidth,232,1);
            store.setF32(kPcStreamLink0Ping,45.1,1);store.setF32(kPcStreamLink0Share,33,1);
            store.setF32(kPcStreamLink1Ping,11.9,1);store.setF32(kPcStreamLink1Share,67,1);
            for(uint16_t id=1064;id<=1067;++id)store.setNa(id,Reason::NotConnected,1);
            store.setText(kPcStreamBuild,"1791581198",1);
            if(scenario==2) {
                for(const auto& d:registry()) if(d.id>=1030&&d.id<=1053)store.setNa(d.id,Reason::SteamVrChanged,1);
                store.setText(kPcStreamL3Note,"CSV header/stride mismatch",1);
            }
            if(scenario==3)setPcStreamNa(store,Reason::NoSteamVr,1);
        }
        Layout layout;layout.columns.resize(1);WidgetInstance w;w.type=WidgetType::Streaming;layout.columns[0].widgets={w};
        GridRenderer renderer(fonts);renderer.setLayout(layout);WidgetContext ctx;ctx.store=&store;ctx.now=1;
        renderer.render(ctx);
        const char* names[]={"streaming-no-pc.png","streaming-simulated.png","streaming-mismatch.png","streaming-no-steamvr.png"};
        const auto path=(std::filesystem::u8path(argv[1])/names[scenario]).u8string();
        if(!renderer.writePng(path))return 1;std::printf("Wrote simulated preview %s\n",path.c_str());
    }
    return 0;
}
