/**
 * @file app_bringup.c
 * @brief 麦克风 / VAD 开机自检编排。
 *
 * 一轮 = 两个测试：
 *   A. app_mic_test_start()   —— 录 2s -> 停 1s -> 回放（验麦克风与功放）
 *   B. app_mic_record_once()  —— 等 VAD 触发 -> 录 -> 回放（验人声判定）
 *
 * 日志前缀统一为 [bringup] / [mic_test] / [mic_vad]，
 * 每轮开始都打一条醒目的横幅，方便对着串口判断该做什么。
 */

#include "tal_api.h"

#include "app_bringup.h"
#include "app_mic_test.h"
#include "app_mic_record.h"
#include "app_speaker.h"

static void prv_banner(const char *title)
{
    PR_NOTICE("============================================================");
    PR_NOTICE("[bringup] %s", title);
    PR_NOTICE("============================================================");
}

void app_bringup_mic_run(void)
{
    int r;

    PR_NOTICE("[bringup] 麦克风 / VAD 自检开始，共 %d 轮", APP_BRINGUP_ROUNDS);

    for (r = 1; r <= APP_BRINGUP_ROUNDS; r++) {
        prv_banner("测试 A：麦克风采集回放（录 2s -> 停 1s -> 回放）");
        PR_NOTICE("[bringup] 第 %d/%d 轮 —— 现在开始对着板子说话（约 3 秒）",
                  r, APP_BRINGUP_ROUNDS);
        app_mic_test_start();

        prv_banner("测试 B：VAD 人声触发（说话 -> VAD 触发 -> 录音 -> 回放）");
        PR_NOTICE("[bringup] 第 %d/%d 轮 —— 请对着板子说话，等 VAD 触发（20 秒超时）",
                  r, APP_BRINGUP_ROUNDS);
        app_mic_record_once();
    }

    prv_banner("自检结束，进入正常应用");

    /* 把扬声器通道停掉：让后面的 app_audio_init() 从干净状态重新打开 codec。 */
    app_speaker_stop();
}
