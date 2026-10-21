// /proc と /sys からの読み取り（読むだけ。書き込みは一切しない）。
#pragma once

#include "wifi_link.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

/** 値が取れなかったことを表す NaN。 */
constexpr double kNoValue = std::numeric_limits<double>::quiet_NaN();

/**
 * 消費電力センサーの 1 チャンネル分（max34417 の power*_input）。
 */
struct PowerChannel {
    std::string chip;    ///< hwmon の name（max34417_10 など）
    std::string label;   ///< power*_label（vph、apc0 など）
    double watts = kNoValue;
};

/**
 * 1 回の読み取り結果。取れなかった値は kNoValue（NaN）。
 */
struct SensorSample {
    // CPU
    double cpuUsagePct = kNoValue;     ///< 全体の使用率
    double cpuMaxCorePct = kNoValue;   ///< 最も忙しいコアの使用率
    double cpuFreqMHz = kNoValue;      ///< いちばん速いクラスタの現在クロック
    double cpuBigFreqMHz = kNoValue;   ///< cpu7（大コア）の現在クロック
    // GPU
    double gpuFreqMHz = kNoValue;
    double gpuMaxFreqMHz = kNoValue;
    // 温度（℃）
    double cpuTempC = kNoValue;        ///< cpu*-thermal / cpuss*-thermal の最大
    double gpuTempC = kNoValue;        ///< gpuss-*-thermal の最大
    double ddrTempC = kNoValue;
    double batteryTempC = kNoValue;
    double displayTempC = kNoValue;    ///< ディスプレイ（左）付近（iio vadc）
    double exhaustTempC = kNoValue;    ///< ファンの排気付近（iio vadc）
    double heatsinkTempC = kNoValue;   ///< ヒートシンクのフィン付近（iio vadc）
    // 熱による制限（cooling_device の cur_state > 0）
    bool throttleCpu = false;
    bool throttleGpu = false;
    // Steam Link の直通回線
    WifiInfo wifi;
    // 電力（W）
    double powerMainW = kNoValue;      ///< 主電源レール（vph）
    double powerRailsW = kNoValue;     ///< vph 以外の全チャンネルの合計（vph から分かれたレールなので vph とは足さない）
    std::vector<PowerChannel> powerChannels;
    // バッテリー
    double batteryPct = kNoValue;
    double batteryVoltageV = kNoValue;
    double batteryCurrentMa = kNoValue;  ///< current_now（符号はドライバのまま）
    std::string batteryStatus;           ///< Charging / Discharging / Not charging / Full
    bool chargerOnline = false;
    bool haveBattery = false;            ///< the battery's power_supply was found (else its values are "not found")
    bool haveCharger = false;            ///< the charger's power_supply was found
    // その他
    double fanRpm = kNoValue;          ///< 回転数（fan1_input / 2）
    double memUsedGiB = kNoValue;
    double memTotalGiB = kNoValue;
};

/**
 * センサーの場所を起動時に一度だけ探し、以後は開いたままのファイルを読み直すクラス。
 */
class Sensors {
public:
    Sensors() = default;
    ~Sensors();
    Sensors(const Sensors&) = delete;
    Sensors& operator=(const Sensors&) = delete;

    /**
     * hwmon・thermal_zone・power_supply などを走査して、読むファイルを開いておく。
     * 走査はここだけ（起動時の 1 回）。
     * @param root 前に付けるフォルダ（テストの作り物の /sys・/proc 用。ふだんは空）
     */
    void discover(const std::string& root = "");

    // One reader per scheduler source (core/metric_registry.h SourceId). Each re-reads its open files and writes
    // only its own fields of the sample. How often they run is the scheduler's business (the intervals are in
    // core/metric_registry.cpp): power and the wireless link every 2 s, the battery and the iio temperatures every
    // 5 s, the rest every update

    /** /proc/stat → cpuUsagePct, cpuMaxCorePct (a delta: NaN on the first read after resetCpuBaseline()). */
    void readCpuUsage(SensorSample& sample);
    /** Forgets the /proc/stat baseline, so the next readCpuUsage() starts a fresh delta. */
    void resetCpuBaseline();
    /** cpufreq → cpuFreqMHz (fastest cluster), cpuBigFreqMHz. */
    void readCpuClocks(SensorSample& sample);
    /** devfreq → gpuFreqMHz, gpuMaxFreqMHz. */
    void readGpuClock(SensorSample& sample);
    /** /proc/meminfo → memUsedGiB, memTotalGiB. */
    void readMemory(SensorSample& sample);
    /** thermal_zone → cpuTempC, gpuTempC, ddrTempC. */
    void readThermal(SensorSample& sample);
    /** iio vadc → displayTempC, exhaustTempC, heatsinkTempC. */
    void readIioTemps(SensorSample& sample);
    /** cooling_device → throttleCpu, throttleGpu. */
    void readThrottle(SensorSample& sample);
    /** hwmon fan1_input → fanRpm. */
    void readFan(SensorSample& sample);
    /** max34417 → powerMainW, powerRailsW, powerChannels. */
    void readPower(SensorSample& sample);
    /** power_supply battery + charger → the battery fields. */
    void readBattery(SensorSample& sample);
    /**
     * nl80211 + statistics → wifi (rates are a delta since the previous call).
     * @param now monotonic seconds
     */
    void readWifi(SensorSample& sample, double now);
    /** Starts fresh byte-counter deltas (after a pause): the next readWifi gives no rates. */
    void resetWifiCounters();

    /**
     * 見つかったセンサーの一覧（--print 用）。
     * @return 人が読むための複数行の文字列
     */
    std::string describe() const;

private:
    friend struct SensorsTestAccess;

    /** 開いたままにする読み取り専用ファイル。 */
    struct Source {
        std::string path;
        std::string name;  ///< 表示・分類用の名前
        int fd = -1;
    };

    std::vector<Source> cpuTemps_;
    std::vector<Source> gpuTemps_;
    Source ddrTemp_;
    std::vector<Source> cpuFreqs_;     ///< policy*/scaling_cur_freq
    Source cpuBigFreq_;
    Source gpuFreq_;
    double gpuMaxFreqMHz_ = kNoValue;
    std::vector<Source> powers_;       ///< name = "chip/label"
    Source fan_;
    Source batCapacity_, batVoltage_, batCurrent_, batTemp_, batStatus_;
    Source chargerOnline_;
    Source procStat_;
    Source memInfo_;
    Source displayTemp_, exhaustTemp_, heatsinkTemp_;  ///< iio の温度（m℃）
    std::vector<Source> cpuCooling_;                   ///< cooling_device（cpufreq-cpu*）の cur_state
    std::vector<Source> gpuCooling_;                   ///< cooling_device（devfreq-*gpu）の cur_state
    WifiLink wifiLink_;

    std::vector<uint64_t> prevBusy_;   ///< /proc/stat の前回値（[0] が全体、以降コアごと）
    std::vector<uint64_t> prevTotal_;
    std::vector<uint64_t> currentBusy_, currentTotal_;  ///< 次の読み取りにも領域を再利用する

    /**
     * ファイルを読み取り専用で開いて Source を作る。
     * @param path パス
     * @param name 名前
     * @return 開けなかったときは fd = -1 の Source
     */
    static Source openSource(const std::string& path, const std::string& name);

    /**
     * 開いてあるファイルを先頭から読み直す。
     * @param source 読むファイル
     * @param buffer 書き込み先
     * @param size buffer の大きさ
     * @return 読んだバイト数。失敗時は -1
     */
    static long readRaw(const Source& source, char* buffer, size_t size);

    /**
     * 整数ひとつのファイルを読む。
     * @param source 読むファイル
     * @param value 書き込み先
     * @return 読めたら true
     */
    static bool readLong(const Source& source, long long& value);

    /**
     * 1 行の文字列ファイルを読む（末尾の改行は除く）。
     * @param source 読むファイル
     * @return 読んだ文字列。失敗時は空
     */
    static std::string readText(const Source& source);

    /**
     * 温度ファイル群の最大値を℃で返す（m℃ 単位を想定）。
     * @param sources 温度ファイル群
     * @return 最大値。ひとつも読めなければ NaN
     */
    static double maxTempC(const std::vector<Source>& sources);

    /**
     * cooling_device 群のどれかが制限中（cur_state > 0）か。
     * @param sources cur_state のファイル群
     * @return 1 つでも 0 より大きければ true
     */
    static bool anyCooling(const std::vector<Source>& sources);

    /** 開いたファイルをすべて閉じる。 */
    void closeAll();
};
