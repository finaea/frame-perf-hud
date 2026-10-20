// PC-owned Streaming values. IDs and names are shared by the registry, packets and both renderers.
#pragma once
#include "link_wire.h"

#define FPH_PC_STREAM_METRICS(X)                                                                                       \
    X(Ping, 1020, "ping", "ms", F32)                                                                                   \
    X(NamedPing, 1021, "server_ping", "ms", F32)                                                                       \
    X(NamedEncode, 1022, "encode_named", "ms", F32)                                                                    \
    X(NamedSend, 1023, "send_named", "ms", F32)                                                                        \
    X(EstimatedBandwidth, 1024, "estimated_bandwidth", "Mbit/s", F32)                                                  \
    X(RetransmitRequests, 1025, "retransmit_requests", "", F32)                                                        \
    X(RetransmitSent, 1026, "retransmit_sent", "", F32)                                                                \
    X(Skipped, 1027, "skipped_per_second", "", F32)                                                                    \
    X(Late, 1028, "late", "", F32)                                                                                     \
    X(EncoderResets, 1029, "encoder_resets", "", F32)                                                                  \
    X(Fps, 1030, "fps", "Hz", F32)                                                                                     \
    X(WireMbps, 1031, "wire", "Mibit/s", F32)                                                                          \
    X(VideoMbps, 1032, "video", "Mibit/s", F32)                                                                        \
    X(FecMbps, 1033, "fec", "Mibit/s", F32)                                                                            \
    X(AudioMbps, 1034, "audio", "Mibit/s", F32)                                                                        \
    X(RetryMbps, 1035, "retry", "Mibit/s", F32)                                                                        \
    X(LogicMbps, 1036, "logic", "Mibit/s", F32)                                                                        \
    X(GameAvg, 1040, "game_avg", "ms", F32)                                                                            \
    X(GameP95, 1041, "game_p95", "ms", F32)                                                                            \
    X(EncodeAvg, 1042, "encode_avg", "ms", F32)                                                                        \
    X(EncodeP95, 1043, "encode_p95", "ms", F32)                                                                        \
    X(SendAvg, 1044, "send_avg", "ms", F32)                                                                            \
    X(SendP95, 1045, "send_p95", "ms", F32)                                                                            \
    X(NetworkAvg, 1046, "network_avg", "ms", F32)                                                                      \
    X(NetworkP95, 1047, "network_p95", "ms", F32)                                                                      \
    X(DecodeAvg, 1048, "decode_avg", "ms", F32)                                                                        \
    X(DecodeP95, 1049, "decode_p95", "ms", F32)                                                                        \
    X(SlackAvg, 1050, "slack_avg", "ms", F32)                                                                          \
    X(SlackP95, 1051, "slack_p95", "ms", F32)                                                                          \
    X(TotalAvg, 1052, "total_avg", "ms", F32)                                                                          \
    X(TotalP95, 1053, "total_p95", "ms", F32)                                                                          \
    X(HoldAvg, 1054, "hold_avg", "ms", F32)                                                                            \
    X(HoldP95, 1055, "hold_p95", "ms", F32)                                                                            \
    X(DisplayAvg, 1056, "display_avg", "ms", F32)                                                                      \
    X(DisplayP95, 1057, "display_p95", "ms", F32)                                                                      \
    X(Link0Ping, 1060, "link0_ping", "ms", F32)                                                                        \
    X(Link0Share, 1061, "link0_share", "%", F32)                                                                       \
    X(Link1Ping, 1062, "link1_ping", "ms", F32)                                                                        \
    X(Link1Share, 1063, "link1_share", "%", F32)                                                                       \
    X(Link2Ping, 1064, "link2_ping", "ms", F32)                                                                        \
    X(Link2Share, 1065, "link2_share", "%", F32)                                                                       \
    X(Link3Ping, 1066, "link3_ping", "ms", F32)                                                                        \
    X(Link3Share, 1067, "link3_share", "%", F32)                                                                       \
    X(StreamResets, 1070, "stream_resets", "", I32)                                                                    \
    X(BadLinks, 1071, "bad_links", "", I32)                                                                            \
    X(EncoderReinits, 1072, "encoder_reinits", "", I32)                                                                \
    X(ThrottleEvents, 1073, "throttle_events", "", I32)                                                                \
    X(Build, 1074, "build", "", ShortString)                                                                           \
    X(L1Note, 1075, "named_status", "", ShortString)                                                                   \
    X(L2Note, 1076, "links_status", "", ShortString)                                                                   \
    X(L3Note, 1077, "frames_status", "", ShortString)                                                                  \
    X(ReadMs, 1078, "reader_cost", "ms", F32)                                                                          \
    X(SceneKey, 1080, "scene_key", "", ShortString)                                                                    \
    X(SceneName, 1081, "scene_name", "", ShortString)                                                                  \
    X(ScenePid, 1082, "scene_pid", "", I32)                                                                            \
    X(DisplayHz, 1083, "display_hz", "Hz", F32)                                                                        \
    X(Link0Address, 1090, "link0_address", "", ShortString)                                                            \
    X(Link1Address, 1091, "link1_address", "", ShortString)                                                            \
    X(Link2Address, 1092, "link2_address", "", ShortString)                                                            \
    X(Link3Address, 1093, "link3_address", "", ShortString)

namespace fph {
namespace metric {
#define FPH_STREAM_ID(n, id, name, unit, type) constexpr uint16_t kPcStream##n = id;
FPH_PC_STREAM_METRICS(FPH_STREAM_ID)
#undef FPH_STREAM_ID
} // namespace metric
bool isPcStreamMetric(uint16_t id);
std::vector<link::WireValue> pcStreamValues(const MetricStore& store);
void setPcStreamNa(MetricStore& store, Reason reason, double now);
// Names only verified endpoints matching a known headset adapter; all other cases retain Link N.
std::string streamLinkName(const MetricStore& store, unsigned index);
// One full PC snapshot fits a datagram. Includes PC identity so a lease is required on receive.
struct PcMetrics {
    link::Id128 pcId{};
    uint32_t snapshotId = 0;
    std::vector<link::WireValue> values;
    uint32_t unknownValues = 0; // Receive-side only; not part of the wire encoding.
};
std::vector<uint8_t> encodePcMetrics(const PcMetrics& m);
bool decodePcMetrics(const uint8_t* data, size_t size, PcMetrics& out);
// Retains no stale data after the PC stops sending; receive times use the local clock.
class PcStreamReceiver {
public:
    void apply(const PcMetrics& m, MetricStore& store, double now);
    void expire(MetricStore& store, double now);
    void reset(MetricStore& store, double now);

private:
    double heard_ = -1e9;
    bool active_ = false;
};
} // namespace fph
