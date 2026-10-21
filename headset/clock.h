// 性能パネルに出す時計の文字列（本体のタイムゾーンの時刻）。
#pragma once

#include <ctime>
#include <string>

/**
 * 時刻を時計の文字列にする。
 * @param local 本体のタイムゾーンでの時刻
 * @param format 12（例: "3:05 PM"）か 24（例: "15:05"）。それ以外は空
 * @return 文字列
 */
std::string formatClock(const std::tm& local, int format);

/**
 * 今の時刻を時計の文字列にする。
 * @param format 12 か 24（formatClock と同じ）
 * @return 文字列。時刻を読めなければ "--:--"
 */
std::string localClock(int format);
