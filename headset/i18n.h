// UI string tables. Drawing code never contains UI text; it looks it up here.
// English only for now; other languages are welcome (see README). Logs and --print aren't translated.
#pragma once

#include <string>

/** UI language. */
enum class Language { En };

/**
 * Default language when config.json has none. Always English until a second language exists.
 * @return the language
 */
Language systemLanguage();

/**
 * パネルと設定パネルの文言一式。
 */
struct UiText {
    // ---- 性能パネル ----
    const char* frame;          ///< フレームの段の見出し
    const char* waiting;        ///< フレームの計測待ち
    const char* noSteamVr;      ///< SteamVR につながっていない
    const char* reproj;         ///< 再投影（後ろに % が続く）
    const char* dropped;        ///< 落ちたフレーム（後ろに数が続く）
    const char* power;          ///< 電力の段の見出し
    const char* unmeasured;     ///< vph minus the measured rails (followed by W). Never the all-channels sum
    const char* fan;            ///< ファン
    const char* temp;           ///< 温度の段の見出し
    const char* cpuTempUnit;    ///< 大きい CPU 温度の単位（℃ CPU）
    const char* celsius;        ///< 温度の単位（℃ / °C）
    const char* battTemp;       ///< 電池の温度
    const char* display;        ///< 画面付近の温度
    const char* exhaust;        ///< 排気の温度
    const char* heatsink;       ///< 放熱板の温度
    const char* maxCoreOpen;    ///< 「（最大コア 」
    const char* maxCoreClose;   ///< 「）」
    const char* direct;         ///< Steam Link の直通回線
    const char* homeWifi;       ///< 家の Wi-Fi（直通回線に相手がいないとき・どちらにもつながっていないとき）
    const char* notConnected;   ///< 直通回線にも家の Wi-Fi にもつながっていない
    const char* linkRate;       ///< リンク速度
    const char* battery;        ///< 本体の電池
    const char* leftHand;       ///< 左手のコントローラー
    const char* rightHand;      ///< 右手のコントローラー
    const char* noControllers;  ///< コントローラーがどちらもいない
    const char* memory;         ///< メモリ
    // ---- 設定パネル ----
    const char* settingsTitle;
    const char* cardPanel;              ///< 左のカードの見出し（パネル）
    const char* sentenceBreak;          ///< 最下行で 2 つの文をつなぐ区切り（。/ . ）
    const char* panelShown;
    const char* panelHidden;
    const char* rowShow;
    const char* rowLanguage;
    const char* rowPosition;
    const char* rowNudge;
    const char* rowSize;
    const char* rowOpacity;
    const char* rowAutostart;           ///< 自動起動
    const char* autostartNextLaunch;    ///< 次回の SteamVR 起動から
    const char* autostartNotInstalled;  ///< 未インストール
    const char* autostartBusy;          ///< 切り替え中
    const char* autostartFailed;        ///< 切り替えに失敗
    const char* on;
    const char* off;
    const char* bottomLeft;
    const char* bottomCenter;
    const char* bottomRight;
    const char* topLeft;
    const char* topRight;
    const char* moveLeft;
    const char* moveRight;
    const char* moveUp;
    const char* moveDown;
    const char* nearer;
    const char* farther;
    const char* reset;
    const char* quit;           ///< アプリを終了
    const char* quitConfirm;    ///< もう一度押すと終了
    const char* positionNow;    ///< いまの位置（後ろに横・縦・前が続く）
    const char* posX;
    const char* posY;
    const char* posZ;
    const char* cardFacing;     ///< 向きのカードの見出し
    const char* yawLeft;        ///< 面を左へ向ける
    const char* yawRight;       ///< 面を右へ向ける
    const char* pitchUp;        ///< 面を上へ向ける
    const char* pitchDown;      ///< 面を下へ向ける
    const char* rollLeft;       ///< 画面内で左に回す（反時計回り）
    const char* rollRight;      ///< 画面内で右に回す（時計回り）
    const char* faceMe;         ///< 自分に向ける
    const char* faceForward;    ///< 正面向き（回転なし）
    const char* angleStep1;     ///< 向きのボタンを 1° ずつ動かす
    const char* angleStep5;     ///< 向きのボタンを 5° ずつ動かす
    const char* facingNow;      ///< いまの向き（後ろに左右・上下・回転が続く）
    const char* yawName;
    const char* pitchName;
    const char* rollName;
    const char* footer;         ///< 変更はすぐ反映される旨
    // ---- 更新（vendor/frame-updater/strings.md より） ----
    const char* rowUpdateCheck;          ///< 設定ファイルの説明用（「新しい版の確認」）
    const char* updateUpToDateFormat;    ///< `UpToDate`（%s は版）
    const char* updateChecking;          ///< `checking` で、まだ答えがない
    const char* updateAvailableFormat;   ///< `Available`（%s は版）
    const char* updateButton;            ///< 「更新する」を押す（確認へ）
    const char* updateManual;            ///< `installable` が false のときの案内
    const char* updateReleasePage;       ///< リリースページの URL の前
    const char* updateConfirmFormat;     ///< 確認の質問（%s は版）
    const char* updateConfirmHint;       ///< 確認の補足
    const char* updateConfirmYes;        ///< 確認の実行ボタン
    const char* updateConfirmNo;         ///< 確認のやめるボタン
    const char* updateInstallingFormat;  ///< `Installing`（%s は下の手順の文言）
    const char* updateInstalledFormat;   ///< `Installed`（%s は版）
    const char* updateInstallFailed;     ///< `InstallFailed` の見出し（理由が続く）
    const char* updateCheckFailed;       ///< `CheckFailed` の見出し（理由が続く）
    const char* updateCheckNow;          ///< ［確認］ボタン（strings.md のまま。幅は足りている）
    const char* updateRetry;             ///< `InstallFailed` のやり直しボタン
    const char* updateDismiss;           ///< `Installed` / `InstallFailed` を閉じる
    const char* updateLogHint;           ///< 失敗したときの補足（ログの場所）
    // ---- 固定先・手首・時計 ----
    const char* rowAttachment;    ///< 位置のカードの「固定先」
    const char* attachHead;       ///< 固定先: 頭
    const char* attachLeft;       ///< 固定先: 左手（のコントローラー）
    const char* attachRight;      ///< 固定先: 右手（のコントローラー）
    const char* wristPreset;      ///< 手首のときの位置のボタン（手首の標準の位置と向きに戻す）
    const char* wristOffsetFormat;  ///< 手首のときの、いまの位置（%.0f は標準の位置からの距離、cm）
    const char* rowWristFade;     ///< 傾けると消す
    const char* rowFadeAngle;     ///< 消える角度
    const char* fadeRangeFormat;  ///< 消える角度の値（%.0f が 2 つ: 薄くなり始める角度、消えきる角度）
    const char* rowClock;         ///< 時計
    const char* clock12;          ///< 12 時間制のボタン
    const char* clock24;          ///< 24 時間制のボタン
    const char* clockLabel;       ///< 性能パネルの時計の見出し
    // ---- T05: dashboard pages, Detached mode, the layout editor, PC pairing ----
    const char* pageLayout;          ///< page switch: the layout editor
    const char* pagePlacement;       ///< page switch: attach point, position, size, ...
    const char* attachRoom;          ///< attach point: Detached (pinned in the room)
    const char* bringInFront;        ///< Detached: put it 1 m in front of the HMD
    const char* detachedHint;        ///< Detached: how to move it with the laser
    const char* roomNotPlaced;       ///< Detached: no pose yet
    const char* rowCurve;            ///< R2 Detached: the curvature row
    const char* editorTitle;         ///< layout card title
    const char* editorHint;          ///< layout card hint
    const char* editorNewColumn;     ///< the empty lane on the right
    const char* editorDropToRemove;  ///< palette card while a widget is dragged over it
    const char* paletteTitle;        ///< palette card title
    const char* paletteNetwork;      ///< palette chip for Network (the cards name the adapter)
    const char* paletteCpuItems;     ///< R3: short palette chip labels (the full titles shrank unreadably)
    const char* paletteRamItems;
    const char* paletteHeadsetCamera;
    const char* paletteEyeCamera;
    const char* paletteSoon;         ///< a palette chip that isn't available yet
    const char* selectedNone;        ///< no widget selected
    const char* noSettings;          ///< the selected widget has no settings
    const char* removeWidget;        ///< remove the selected widget
    const char* rowRows;             ///< process lists: rows shown
    const char* rowSameName;         ///< process lists: one row per process name
    const char* rowAdapter;          ///< Network: which adapter
    const char* adapterDongle;
    const char* adapterHome;
    const char* rowsFormat;          ///< card detail: "%u rows"
    const char* summedSuffix;        ///< card detail after rowsFormat when summed by name
    const char* layoutSaved;         ///< status after an applied edit
    const char* layoutStale;         ///< status after a rejected (stale) edit
    const char* layoutSaveFailed;    ///< status after a failed save
    const char* pcWantsFormat;       ///< pairing banner (%s = the PC's name)
    const char* pcAllow;
    const char* pcDeny;
    const char* forgetPcsFormat;     ///< footer button (%zu = allowed PCs)
    const char* rowCameras;          ///< T10 Headset camera: which outward cams
    const char* rowFullSize;         ///< T10 Headset camera: full-size frames in the headset
    const char* rowFps;              ///< T10 camera widgets: headset frame rate
    const char* rowCompact;          ///< R2: Frame, Power, Battery, Temp: the short card
    const char* rowHideSystem;       ///< R2 process lists: leave SteamOS / Steam / SteamVR out (hide_rules.json)
    const char* hiddenSuffix;        ///< R2: card detail after rowsFormat when Hide system is on
    // ---- T11: the Logs page ----
    const char* pageLogs;            ///< page switch: session logs, Save / Record, crash recorder
    const char* cardRecord;          ///< card title: Save and Record
    const char* rowRecord;           ///< Record On / Off
    const char* saveNow;             ///< Save button
    const char* recordHint;          ///< under the buttons: what Save and Record write
    const char* cardCrash;           ///< card title: crash recorder
    const char* rowCrashRecorder;    ///< crash recorder On / Off
    const char* crashHint;           ///< what the crash recorder does
    const char* rowPowerOff;         ///< "Last power-off"
    const char* cardLastSession;     ///< card title: the last session's card
    const char* noSessionYet;        ///< no session ended since start
    const char* logsFolder;          ///< where the files are
    // ---- R2: the Profiles page ----
    const char* pageProfiles;        ///< page switch
    const char* cardProfiles;        ///< the list's title
    const char* profileCountFormat;  ///< "%zu / %zu" profiles used
    const char* profileNone;         ///< no profiles yet
    const char* profileActive;       ///< chip on the active profile
    const char* profileUnsaved;      ///< chip: the live layout differs from it
    const char* profileUse;
    const char* profileSave;         ///< overwrite the active profile
    const char* profileRename;
    const char* profileDelete;
    const char* profileNew;
    const char* profileReset;        ///< live layout -> the built-in default
    const char* profileTapAgain;     ///< Use / Reset armed: the live layout's changes would be lost
    const char* profileDeleteAgain;  ///< Delete armed
    const char* profileNotSaved;     ///< no active profile and the live layout isn't saved in any
    const char* profileHint;         ///< what a profile holds
    const char* profileKeyboard;     ///< the keyboard's description when renaming
};

/**
 * 言語の文言の表を返す。
 * @param language 言語
 * @return 文言の表（プログラムの終わりまで有効）
 */
const UiText& uiText(Language language);

/**
 * SteamVR がアプリを抑えているときのバッジの文言（例: 「72Hz の半分に制限（再投影）」）。
 * @param language 言語
 * @param hz リフレッシュレート
 * @param throttled 抑えている段数（1 以上）
 * @return 文言
 */
std::string throttleBadgeText(Language language, double hz, int throttled);

/**
 * 熱で制限がかかっているときのバッジの文言（例: 「熱で制限中 CPU・GPU」）。
 * @param language 言語
 * @param cpu CPU が制限中か
 * @param gpu GPU が制限中か
 * @return 文言
 */
std::string thermalBadgeText(Language language, bool cpu, bool gpu);

/**
 * 本体の電池の状態を短い文言にする。
 * @param language 言語
 * @param status power_supply の status（Charging など）
 * @param chargerOnline 充電器がつながっているか
 * @return 文言
 */
std::string batteryStatusText(Language language, const std::string& status, bool chargerOnline);

/**
 * 更新の手順（`UpdateStatus::step`）を文言にする。知らない手順はそのまま返す。
 * @param language 言語
 * @param step "start" / "download" / "verify" / "extract" / "install"
 * @return 文言
 */
std::string updateStepText(Language language, const std::string& step);

/**
 * 更新のエラーコード（`UpdateStatus::error`）を文言にする。知らないコードは「other」の文言にする。
 * @param language 言語
 * @param error frame-update.sh / update_check.h のエラーコード
 * @return 文言
 */
std::string updateErrorText(Language language, const std::string& error);

/**
 * 設定ファイルに書く言語の名前。
 * @param language 言語
 * @return "en"
 */
const char* languageCode(Language language);

/**
 * 設定ファイルの言語の名前を読む。
 * @param code "en"
 * @param language 読めたときの書き込み先
 * @return 知っている名前なら true
 */
bool parseLanguage(const std::string& code, Language& language);
