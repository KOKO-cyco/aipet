/**
 * @file app_mic_probe.h
 * @brief 麦克风电平探针：给 bring-up 测试加客观的采音证据。
 *
 * 为什么需要：原来两个测试只统计"收到多少字节"。字节数只能证明**回调被调了**，
 * 不能区分"麦克风采到的是真实声音"还是"一路静音/直流"。加上峰值统计后，
 * 日志里的 peak 数值可以直接判断麦克风是否真的在拾音。
 *
 * 峰值按 |sample| 取最大，16-bit 满量程 32767。
 *   < 100   基本是静音（或麦克风根本没接）
 *   几百~几千 有环境噪声
 *   > 2000  有人在近距离说话
 */

#ifndef __APP_MIC_PROBE_H__
#define __APP_MIC_PROBE_H__

#include <stdint.h>

typedef struct {
    uint32_t frames;     /* 回调次数 */
    uint32_t bytes;      /* 累计字节数（16-bit mono） */
    int32_t  peak;       /* 全程最大 |sample| */
    int32_t  last_peak;  /* 最近一帧的峰值 */
} APP_MIC_PROBE_T;

/** 求一帧 PCM 的绝对峰值。 */
static inline int32_t app_mic_probe_peak(const uint8_t *data, uint32_t len)
{
    const int16_t *s = (const int16_t *)data;
    uint32_t       n = len / 2u;
    int32_t        peak = 0;

    for (uint32_t i = 0; i < n; i++) {
        int32_t v = (int32_t)s[i];
        if (v < 0) {
            v = -v;
        }
        if (v > peak) {
            peak = v;
        }
    }

    return peak;
}

#endif /* __APP_MIC_PROBE_H__ */
