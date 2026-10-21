// 自動起動（systemd ユーザーサービス）の状態の読み取りと、有効・無効の切り替え。
// 読むのはユニットとリンクの有無だけ。切り替えは systemctl --user enable / disable を
// シェルを通さずに子プロセスで実行し、終わるのを待たない（UI を固めない）。
#pragma once

#include <string>
#include <sys/types.h>

/**
 * 設定パネルに出すための、自動起動の今の状態。
 */
struct AutostartStatus {
    enum class State {
        NotInstalled,  ///< ユニットのファイルが無い（ボタンはグレー）
        Enabled,       ///< steamvr.service.wants にリンクがある
        Disabled,      ///< ユニットはあるがリンクが無い
    };
    State state = State::NotInstalled;
    bool busy = false;    ///< systemctl を実行中
    bool failed = false;  ///< 直前の切り替えが失敗した
};

/**
 * 自動起動の状態を読み、systemctl --user enable / disable を非同期で実行する係。
 */
class Autostart {
public:
    Autostart();
    ~Autostart();
    Autostart(const Autostart&) = delete;
    Autostart& operator=(const Autostart&) = delete;

    /**
     * ユニットとリンクの有無を読み直す（stat 2 回だけなので軽い）。
     * @return 状態が変わったら true
     */
    bool refresh();

    /**
     * 有効・無効の切り替えを始める。実行中・ユニットが無い・すでにその状態のときは何もしない。
     * @param enable true で enable、false で disable
     * @return 子プロセスを始めたら true
     */
    bool request(bool enable);

    /**
     * 実行中の子プロセスが終わっていれば回収し（waitpid の WNOHANG）、結果を反映する。
     * @return 状態が変わった（描き直しが要る）なら true
     */
    bool poll();

    /** @return 今の状態 */
    const AutostartStatus& status() const { return status_; }

    /** @return ユニットのファイルのパス（README・ログ用） */
    const std::string& unitPath() const { return unitPath_; }

private:
    static constexpr const char* kUnitName = "frame-perf-overlay.service";

    std::string unitPath_;      ///< $XDG_CONFIG_HOME/systemd/user/frame-perf-overlay.service
    std::string linkPath_;      ///< $XDG_CONFIG_HOME/systemd/user/steamvr.service.wants/frame-perf-overlay.service
    std::string systemctl_;     ///< systemctl の絶対パス（子では PATH を探さない）
    AutostartStatus status_;
    pid_t child_ = -1;
    int errorPipe_ = -1;        ///< 子の標準エラーを受け取る（失敗の理由をログに残す）
    std::string childOutput_;
    bool requestedEnable_ = false;

    /**
     * 子の出力を、たまっている分だけ読む（ブロックしない）。
     */
    void drainPipe();
};
