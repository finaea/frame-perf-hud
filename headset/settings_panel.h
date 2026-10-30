// SteamVR ダッシュボードに出す設定パネル（描画・ボタンの当たり判定・操作の反映）。
#pragma once

#include "autostart.h"
#include "config.h"
#include "layout_editor.h"  // T05
#include "link_ui.h"        // T05: PC pairing banner (ticket 06's state)
#include "logs_ui.h"        // T11: the Logs page
#include "profiles_ui.h"    // R2: the Profiles page
#include "update_check.h"  // vendor/frame-updater/cpp（CMake の include dir で見つかる）

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

class FontSet;
struct Pen;
struct UiText;
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/** 設定パネルのボタンが表す操作。 */
enum class SettingsAction {
    None,
    ShowOn,
    ShowOff,
    LanguageEn,  ///< the only language for now (other languages are welcome: see README)
    AttachHead,   ///< 固定先を頭に
    AttachLeft,   ///< 固定先を左手（のコントローラー）に
    AttachRight,  ///< 固定先を右手（のコントローラー）に
    PresetLeftBottom,
    PresetCenterBottom,
    PresetRightBottom,
    PresetLeftTop,
    PresetRightTop,
    PresetWrist,  ///< 選んでいる手首の位置と向きを標準（WristPose の既定値）に戻す
    MoveLeft,     ///< 左へ（頭: 2cm、手首: パネル自身の軸で 1cm）
    MoveRight,    ///< 右へ
    MoveUp,       ///< 上へ
    MoveDown,     ///< 下へ
    MoveNear,     ///< 頭: 見える方向のまま 5cm 近づける。手首: パネルの面の向き（見ている人の方）へ 1cm
    MoveFar,      ///< 頭: 見える方向のまま 5cm 遠ざける。手首: 面の向きの逆へ 1cm
    YawLeft,      ///< 面を左へ向ける（yaw を減らす）
    YawRight,     ///< 面を右へ向ける（yaw を増やす）
    PitchUp,      ///< 面を上へ向ける（pitch を増やす）
    PitchDown,    ///< 面を下へ向ける（pitch を減らす）
    RollLeft,     ///< 画面内で左に回す（面を見て反時計回り。roll を増やす）
    RollRight,    ///< 画面内で右に回す（面を見て時計回り。roll を減らす）
    FaceMe,       ///< 今の位置のまま、面を頭に向ける（roll はそのまま。頭のときだけ）
    FaceForward,  ///< 回転なし（0, 0, 0）に戻す（頭のときだけ）
    AngleStep1,   ///< 向きのボタンの刻みを 1° に（設定パネルの中だけで扱う）
    AngleStep5,   ///< 向きのボタンの刻みを 5° に（設定パネルの中だけで扱う）
    FadeOn,       ///< 手首のとき、傾けると消す
    FadeOff,      ///< 手首のとき、傾けても消さない
    FadeAngleDown,  ///< 消える角度を 5° 小さく（35° まで）
    FadeAngleUp,    ///< 消える角度を 5° 大きく（90° まで）
    CurveDown,      ///< R3 Detached: 10° less arc (config.detachedCurveDeg, 0..90, saved)
    CurveUp,        ///< R3 Detached: 10° more arc
    SizeDown,
    SizeUp,
    AlphaDown,
    AlphaUp,
    ClockOff,     ///< 時計を出さない
    Clock12,      ///< 時計を 12 時間制に
    Clock24,      ///< 時計を 24 時間制に
    Reset,
    Quit,  ///< アプリを終了（2 回目の押下で確定したときだけ返る）
    AutostartOn,   ///< 自動起動を有効に（systemctl --user enable）
    AutostartOff,  ///< 自動起動を無効に（systemctl --user disable）
    UpdateCheckNow,     ///< ［確認］: 24 時間のキャッシュを無視してその場で確かめる（呼び出し側で扱う）
    UpdateInstall,      ///< ［更新］の 1 回目: 確認の表示に切り替えるだけ
    UpdateConfirmYes,   ///< 確認の「更新する」: 実際にインストールを始める（呼び出し側で扱う）
    UpdateConfirmNo,    ///< 確認の「やめる」: 確認を取り消す
    UpdateRetry,        ///< 失敗のあとの「もう一度」（呼び出し側で扱う）
    UpdateDismiss,      ///< 「入れました」/「失敗しました」の表示を閉じる（呼び出し側で扱う）
    // T05
    PageLayout,      ///< show the layout editor (handled inside the panel)
    PagePlacement,   ///< show the placement cards (handled inside the panel)
    AttachDetached,  ///< pin the HUD in the room (the caller places it in front when it has no pose yet)
    BringInFront,    ///< Detached: 1 m in front of the HMD (the caller does it: it needs the HMD pose)
    PcAllow,         ///< pairing banner: allow SettingsPanel::bannerPcId() (the caller tells the link server)
    PcDeny,          ///< pairing banner: deny it
    PcForgetAll,     ///< footer: forget every allowed PC
    // T11: the Logs page
    PageLogs,          ///< show the Logs page (handled inside the panel)
    RecordOn,          ///< start Record (the caller does it; not saved in the config)
    RecordOff,         ///< stop Record
    SaveNow,           ///< save the last 10 minutes + this session's summary (the caller does it)
    CrashRecorderOn,   ///< config.crashRecorder (saved)
    CrashRecorderOff,
    SessionLogsOn,     ///< config.sessionLogs (saved)
    SessionLogsOff,
    // R2: the Profiles page. The caller does them on selectedProfileId(); Use / Reset with unsaved changes and Delete
    // only come back on the second tap
    PageProfiles,     ///< show the Profiles page (handled inside the panel)
    ProfileRow0,      ///< select row 0-7 (ProfileRow0 + i; handled inside the panel)
    ProfileRow1,
    ProfileRow2,
    ProfileRow3,
    ProfileRow4,
    ProfileRow5,
    ProfileRow6,
    ProfileRow7,
    ProfileNew,       ///< save the live layout as a new profile
    ProfileUse,       ///< load the selected profile as the live layout
    ProfileSave,      ///< overwrite the active profile with the live layout
    ProfileRename,    ///< SteamVR's keyboard for the selected profile's name
    ProfileDelete,    ///< delete the selected profile
    ProfileReset,     ///< live layout -> the built-in default (profiles untouched)
};

/** T05: which page the dashboard tab shows. */
enum class SettingsPage { Layout, Placement, Logs, Profiles };  // T11: Logs, R2: Profiles

/**
 * 操作を設定に反映する（Quit と None は何もしない）。位置と向きのボタンは、今の固定先（頭・左手・右手）の値を動かす。
 * @param action 操作
 * @param config 書き換える設定
 * @param angleStepDeg 向きのボタン（左右・上下・回す）の刻み（度。1 か 5）。その刻みの目盛りに寄せて動かす
 * @return 設定が変わったら true
 */
bool applySettingsAction(SettingsAction action, Config& config, double angleStepDeg = 1.0);

/**
 * 設定パネルの画像を描き、レーザーポインターの位置からボタンを判定する係。
 */
class SettingsPanel {
public:
    /** 「アプリを終了」を 1 回押してから、確定の 2 回目を待つ秒数。 */
    static constexpr double kQuitConfirmSec = 3.0;

    /**
     * @param fonts 使うフォント（このオブジェクトより長く生きていること）
     */
    explicit SettingsPanel(const FontSet& fonts);
    ~SettingsPanel();
    SettingsPanel(const SettingsPanel&) = delete;
    SettingsPanel& operator=(const SettingsPanel&) = delete;

    /**
     * 今の設定と、ポインターが乗っている・押しているボタンの状態でパネルを描く。
     * @param config 今の設定（言語もここから）
     * @param autostart 自動起動の今の状態
     * @param update 新しい版の確認・インストールの今の状態
     */
    void render(const Config& config, const AutostartStatus& autostart, const frame_updater::UpdateStatus& update,
                const LinkUiState& link = LinkUiState(), const LogsUiState& logs = LogsUiState(),
                const ProfilesUiState& profiles = ProfilesUiState());

    /**
     * T05: hands the editor the current layout. Call before each batch of pointer events, so a press works on the
     * layout the user sees (a PC may have changed it since the last draw).
     * @return true if it differed from what the tab shows (redraw it)
     */
    bool syncLayout(const fph::Layout& layout) { return editor_.sync(layout); }

    /**
     * ポインターが動いた。
     * @param x 左端からの px
     * @param y 上端からの px
     * @return 乗っているボタンが変わった（描き直しが要る）なら true
     */
    bool pointerMove(double x, double y);

    /**
     * T05: smooth scroll on the tab (VREvent_ScrollSmooth), over the editor's lanes.
     * @param dy the event's ydelta
     * @return true if a redraw is needed
     */
    bool scroll(double dy);

    /** T05: takes the editor's queued edit (base version + edited layout). @return false if none */
    bool takeLayoutCommit(uint32_t& baseVersion, fph::Layout& layout) { return editor_.takeCommit(baseVersion, layout); }

    /** T05: how the edit taken last went (shown in the editor). */
    void layoutCommitResult(LayoutEditor::CommitOutcome outcome) { editor_.commitResult(outcome); }

    /** HTTP camera: takes the editor's Edit tap next to an address. @return false if none */
    bool takeAddressEdit(LayoutEditor::AddressEdit& out) { return editor_.takeAddressEdit(out); }
    /** HTTP camera: a line in the editor's status spot (why an address wasn't applied). */
    void layoutNote(const std::string& note) { editor_.showNote(note); }

    /** T05: @return true while the editor holds a press or a drag (poll the pointer at the fast rate) */
    bool editorBusy() const { return editor_.busy(); }

    /** T05: switches the page (tests, --dump-settings-png). */
    void showPage(SettingsPage page) { page_ = page; }
    /** T05: @return the page shown */
    SettingsPage page() const { return page_; }

    /** T05: @return the PC the pairing banner asks about (its id), "" when no banner is shown */
    const std::string& bannerPcId() const { return bannerPcId_; }

    /** R2: @return the profile selected on the Profiles page ("" = none) */
    const std::string& selectedProfileId() const { return selectedProfile_; }
    /** R2: selects a profile (tests, --dump-settings-png) */
    void selectProfile(const std::string& id) { selectedProfile_ = id; }

    /**
     * R2: whether the live layout has unsaved changes right now. Call before each pointerDown: Use / Reset ask for a
     * second tap on this, not on the state the page was last drawn with (a PC's edit may have come in since).
     */
    void setProfilesUnsaved(bool unsaved) { lastProfiles_.unsaved = unsaved; }

    /** R2 (tests): the centre of a button as last laid out. @return false if it isn't on the tab */
    bool buttonCenter(SettingsAction action, double& x, double& y) const {
        const Button* b = findButton(action);
        if (b == nullptr) return false;
        x = b->x + b->w / 2;
        y = b->y + b->h / 2;
        return true;
    }

    /** T05: @return the editor (tests) */
    const LayoutEditor& editor() const { return editor_; }

    /**
     * ボタンが押された。「アプリを終了」は 1 回目で確認の表示に変わり、kQuitConfirmSec 秒以内の 2 回目で Quit を返す。
     * @param x 左端からの px
     * @param y 上端からの px
     * @param now 現在時刻（秒、単調増加）
     * @return 押されたボタンの操作（ボタンの外・終了の 1 回目なら None）
     */
    SettingsAction pointerDown(double x, double y, double now);

    /**
     * ボタンが離された。T05: also ends an editor drag (a drop may queue an edit: takeLayoutCommit).
     * @param x 左端からの px
     * @param y 上端からの px
     * @return 押している表示を消した（描き直しが要る）なら true
     */
    bool pointerUp(double x, double y);

    /**
     * ポインターがパネルから外れた。
     * @return 描き直しが要るなら true
     */
    bool pointerLeave();

    /**
     * 時間で変わる表示（終了の確認の期限切れ）を進める。
     * @param now 現在時刻（秒、単調増加）
     * @return 描き直しが要るなら true
     */
    bool tick(double now);

    /**
     * 描いた画像を OpenVR 用の非乗算済み RGBA にして返す。
     * @return width() * height() * 4 バイト
     */
    const std::vector<uint8_t>& toRgba();

    /**
     * 描いた画像を PNG で保存する。
     * @param path 保存先
     * @return 保存できたら true
     */
    bool writePng(const std::string& path) const;

    /**
     * 見た目の確認用に「もう一度押すと終了」の状態にする（--dump-settings-png 用）。
     */
    void armQuitForPreview();

    /**
     * 見た目の確認用に、更新の確認（「%s に更新しますか？」）の状態にする（--dump-settings-png 用）。
     */
    void armUpdateConfirmForPreview();

    /** @return 画像の幅（px） */
    int width() const;
    /** @return 画像の高さ（px） */
    int height() const;

    /**
     * 向きのボタン（左右・上下）の今の刻み。設定ファイルには保存せず、起動するたびに 1° から始まる。
     * @return 刻み（度。1 か 5）
     */
    double angleStepDeg() const { return angleStepDeg_; }

private:
    /** ボタン 1 個。文言は描くときに言語の表から引く。 */
    struct Button {
        SettingsAction action;
        double x, y, w, h;
    };

    const FontSet& fonts_;
    cairo_surface_t* surface_ = nullptr;
    cairo_t* cr_ = nullptr;
    std::vector<uint8_t> rgba_;
    std::vector<Button> buttons_;
    /** T05: which set of card buttons is laid out (was upstream's wristLayout_ bool). */
    enum class ButtonMode { Layout, Head, Wrist, Detached, Logs, Profiles };  // T11: Logs, R2: Profiles
    ButtonMode mode_ = ButtonMode::Head;  ///< 今のボタンの配置（固定先・ページが変わったら render() で置き直す）
    SettingsPage page_ = SettingsPage::Placement;  ///< R2: the tab opens on Placement, the first page
    LayoutEditor editor_;                       ///< T05
    std::string bannerPcId_;                    ///< T05: the PC the banner shows ("" = no banner)
    bool forgetEnabled_ = false;                ///< T05: "Forget PCs" is pressable (some PC is allowed)
    SettingsAction hover_ = SettingsAction::None;
    bool autostartInstalled_ = true;  ///< 最後に描いたときユニットがあったか（無ければ自動起動のボタンは押せない）
    SettingsAction pressed_ = SettingsAction::None;
    bool quitArmed_ = false;
    double quitArmedUntil_ = 0.0;
    bool updateConfirmArmed_ = false;  ///< 「更新する」を 1 回押して、確認の表示を出している間
    // R2: the Profiles page
    std::string selectedProfile_;                    ///< "" = none
    std::vector<std::string> profileRowIds_;         ///< the ids of the rows as last drawn
    ProfilesUiState lastProfiles_;                   ///< as last drawn (the two-tap rule needs `unsaved`)
    SettingsAction profileArmed_ = SettingsAction::None;  ///< Use / Reset / Delete waiting for the second tap
    double profileArmedUntil_ = 0.0;
    std::vector<SettingsAction> profileDisabled_;    ///< Profiles page buttons drawn disabled (not hittable)
    double angleStepDeg_ = 1.0;        ///< 向きのボタンの刻み（度）。「1° ずつ / 5° ずつ」で切り替える（起動中だけ覚える）

    /**
     * 3 つのカードのボタンの配置を作る（起動時と、固定先が頭と手首のあいだで変わったとき。下の段と更新の帯は
     * render() のたびに置き直す）。手首のときは、位置のボタンが「手首の標準の位置」だけになり、向きのカードの
     * いちばん下の段が「自分に向ける / 正面向き」から「傾けると消す」「消える角度」に変わる。
     */
    void layoutButtons();

    /**
     * 座標にあるボタンを探す。
     * @param x 左端からの px
     * @param y 上端からの px
     * @return ボタンの操作。無ければ None
     */
    SettingsAction hitTest(double x, double y) const;

    /**
     * ボタンの文言を言語の表から引く。
     * @param action ボタンの操作
     * @param text 言語の表
     * @return 文言
     */
    std::string labelOf(SettingsAction action, const UiText& text) const;

    /**
     * 操作からボタンを探す。
     * @param action 操作
     * @return ボタン。無ければ nullptr
     */
    const Button* findButton(SettingsAction action) const;

    /**
     * 下の段・更新の帯のボタンを置く（文言の幅が言語や状態で変わるので、描くたびに位置を決め直す）。
     * 既にあるボタンは位置を置き直すだけ、無ければ足す。
     * @param action 操作
     * @param x 左
     * @param y 上
     * @param w 幅
     * @param h 高さ
     */
    void placeFooterButton(SettingsAction action, double x, double y, double w, double h);

    /**
     * セグメント切り替え（丸い枠のピルの中で、選択中がアクセントの塗り ＋ ✓）を描く。2 択でも 3 択でも使う。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param actions 左から順の操作（ボタンとして置いてあること）
     * @param selected 選択中の番号（0 から。-1 = どれでもない）
     * @param usable 押せるか（押せないときは枠なし・薄い文字）
     */
    void drawSegmented(const Pen& pen, const UiText& text, std::initializer_list<SettingsAction> actions, int selected,
                       bool usable, double size = 24) const;

    /**
     * 1 つのボタン（位置・微調整・− / ＋・既定に戻す）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param action 操作
     * @param selected 選ばれている（位置のボタン）ならアクセントの塗り ＋ ✓
     * @param size 文字の大きさ（入りきらなければ小さくする）
     * @param rotateIcon 回す向きの絵（-1 = 文言の左に反時計回り ⟲、+1 = 文言の右に時計回り ⟳、0 = なし）。
     *                   フォントに ⟲ ⟳ の字が無いので線で描く
     * @param check selected のとき ✓ を付けるか（false なら塗りだけの強調ボタン。更新の帯の［更新する］）
     */
    void drawButton(const Pen& pen, const UiText& text, SettingsAction action, bool selected, double size,
                    int rotateIcon = 0, bool check = true) const;

    /**
     * 見出しと、右上の状態のピル（● パネル表示中 / ○ パネル非表示）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     * @return 状態のピルの左端の x（新しい版の確認の帯はこの左に置く）
     */
    double drawHeader(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * T05: the page switch (Layout | Placement) next to the title. Places its buttons, so not const.
     * @return its right edge
     */
    double drawPageSwitch(const Pen& pen, const UiText& text, double left);

    /**
     * T05: "PC '<name>' wants to connect: Deny / Allow", in the update bar's place while a PC is pending.
     */
    void drawPairingBanner(const Pen& pen, const UiText& text, const LinkUiState::Pending& pc, double left, double right);

    /** The Logs page: what the logs are on the left, the 4 ways to log (2×2 cards) on the right. */
    void drawLogsPage(const Pen& pen, const UiText& text, const Config& config, const LogsUiState& logs) const;

    /** R2: the Profiles page: the list (left) and the selected profile's actions (right). Places its buttons. */
    void drawProfilesPage(const Pen& pen, const UiText& text, const ProfilesUiState& profiles);
    /** R2: a Profiles page button; a disabled one is drawn faded and isn't hittable */
    void drawProfileButton(const Pen& pen, const UiText& text, SettingsAction action, double x, double y, double w,
                           bool enabled, bool primary);

    /**
     * 左のカード「パネル」（表示・大きさ・透明度・時計・既定に戻す）を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     */
    void drawPanelCard(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 真ん中のカード「位置」（固定先・位置のボタン・微調整の十字・近く / 遠く・いまの位置）を描く。
     * 頭のときは位置のボタンが 5 つ（左上・右上・左下・中央下・右下）、手首のときは「手首の標準の位置」の 1 つ。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     */
    void drawPositionCard(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 右のカード「向き」（回す・上下・左右の十字、刻みの 1° / 5°、いまの向き）を描く。いちばん下の段は、頭のときは
     * 自分に向ける / 正面向き、手首のときは「傾けると消す」と「消える角度」。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     */
    void drawFacingCard(const Pen& pen, const UiText& text, const Config& config) const;

    /**
     * 下の段（言語・自動起動・終了）と、いちばん下の 1 行の説明を描く。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     * @param autostart 自動起動の今の状態
     */
    void drawFooter(const Pen& pen, const UiText& text, const Config& config, const AutostartStatus& autostart,
                    const LinkUiState& link);

    /**
     * 見出しの行の、見出しと状態のピルの間に出す、新しい版の確認・インストールの帯（いつも見えている 1 行 ＋ 右のボタン）。
     * @param pen 描画の道具
     * @param text 言語の表
     * @param config 今の設定
     * @param update 新しい版の確認・インストールの今の状態
     * @param left 帯の左端の x
     * @param right 帯の右端の x
     */
    void drawUpdateBar(const Pen& pen, const UiText& text, const Config& config, const frame_updater::UpdateStatus& update,
                       double left, double right);
};

/**
 * ダッシュボードの下に並ぶサムネイル（アイコン）を描く。言語によらず同じ（「Perf」の文字とグラフ）。
 * @param fonts 使うフォント
 * @param size 一辺の px
 * @param rgba 非乗算済み RGBA の書き込み先
 * @param pngPath 空でなければ PNG にも保存する
 */
void renderThumbnail(const FontSet& fonts, int size, std::vector<uint8_t>& rgba, const std::string& pngPath = "");
