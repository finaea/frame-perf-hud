// GPU 使用率の目安（DRM の fdinfo の drm-engine-gpu から）。読むだけ。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

/**
 * 同じユーザーのプロセスが開いている /dev/dri/ 以下 の fdinfo を読み、GPU が働いていた時間の合計から使用率を出す。
 * /proc 全体を調べるのは重い（約 6ms）ので数秒おきにして、あいだは見つけた fdinfo だけ読み直す。
 * ゲーム中は使用中のクライアントの fdinfo を読むだけで 1 回 1.6ms ほどかかるので、呼ぶ側は 2 秒おき程度にする。
 */
class GpuUsage {
public:
    /**
     * 前回からの GPU 使用率を返す。必要なら /proc を走査し直す。
     * @param now 現在時刻（秒、単調増加）
     * @return 使用率（%、0〜100）。初回や取れないときは NaN
     */
    double update(double now);

    /** @return 最後の走査で見つけた DRM クライアントの数 */
    size_t clientCount() const { return clients_.size(); }

private:
    static constexpr double kRescanSec = 30.0;  ///< /proc を走査し直す間隔（ダッシュボードが開いていると 1 回最大 20ms かかるので長め）

    /** 1 つの DRM クライアント（dup された fd は同じ client-id なので 1 つにまとめる）。 */
    struct Client {
        std::string fdinfoPath;  ///< /proc/<pid>/fdinfo/<fd>
        uint64_t lastBusyNs = 0;
        bool haveLast = false;
        bool dormant = false;  ///< 走査の時点で一度も GPU を使っていない（次の走査まで読まない）
    };

    std::map<long, Client> clients_;  ///< drm-client-id → クライアント
    double lastScan_ = -1.0;
    double lastTime_ = -1.0;

    /** /proc を走査して /dev/dri/ 以下 を開いている fd を探し直す（前回の値は client-id で引き継ぐ）。 */
    void rescan();

    /**
     * fdinfo を読んで client-id と GPU の働いた時間を取り出す。
     * @param path fdinfo のパス
     * @param clientId client-id の書き込み先
     * @param busyNs drm-engine-gpu（ns）の書き込み先
     * @return 両方読めたら true
     */
    static bool readFdinfo(const std::string& path, long& clientId, uint64_t& busyNs);
};
