/**
 * @file app_speaker.h
 * @brief Onboard speaker playback: ring buffer + dedicated drain task.
 *
 * The board has an internal audio DAC + onboard amplifier
 * (.tuyaopen/ide/board.json -> audio, interface INTERNAL, and the codec device
 * is registered by the board in tuya_t5ai_board.c), so this needs nothing but
 * the standard TDL audio calls.
 *
 * Decoupling contract:
 *   - app_speaker_write() is NON-BLOCKING. Call it from anywhere, including
 *     real-time emulator callbacks. If the ring buffer is full the block is
 *     dropped rather than waited on.
 *   - Playback itself happens in a dedicated task, because tdl_audio_play()
 *     blocks. Waiting inside a real-time callback stalls the caller.
 *
 * Rate: input is expected ALREADY at the device rate (see app_speaker_rate()).
 * Any resampling (e.g. an emulator's native rate -> 16000) is the caller's job,
 * since only the caller knows the true source rate.
 */

#ifndef __APP_SPEAKER_H__
#define __APP_SPEAKER_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Find the audio device, open it for playback, allocate the ring buffer
 *        and start the drain task.
 *
 * @return OPRT_OK on success, error code otherwise.
 */
OPERATE_RET app_speaker_init(void);

/**
 * @brief Device sample rate in Hz (0 if not initialised).
 *
 * Feed app_speaker_write() data at exactly this rate.
 */
uint32_t app_speaker_rate(void);

/**
 * @brief Re-fill the ring buffer with ~300ms of silence and reset the
 *        drop/underrun counters. Call right before starting a game: the
 *        boot-time prime is consumed while the menus are up, and without this
 *        the buffer would sit at zero for the whole session.
 */
void app_speaker_reprime(void);

/**
 * @brief Discard everything queued in the ring buffer right now.
 *
 * 电影 seek / 暂停用：底层缓冲最多压着 400ms 还没放出来的音频，不清的话
 * seek 之后会先把旧位置的声音放完才听到新位置，暂停之后还会把按下的那一瞬
 * 间之后的话放完（听感就是"暂停了还在说"）。丢掉的是已经排队、还没进 DAC
 * 的数据，正在放的那一块（<=20ms）不受影响。
 */
void app_speaker_clear(void);

/**
 * @brief Queue mono 16-bit PCM. Non-blocking; drops on a full ring buffer.
 *
 * @param mono  mono samples at app_speaker_rate() Hz
 * @param frames number of samples
 * @return 真正进了环形缓冲的**字节数**（= 帧数 x 2）。
 *
 * 生产者如果要自己算"播到第几秒"（fed - queued 那套），**必须**用这个返回值
 * 而不是自己记"我想写的帧数"：ring 满时这次调用会少写甚至一块不写，按想写的
 * 量累加会让位置虚高，画面恒定领先声音，而且丢得越多偏得越多。
 */
uint32_t app_speaker_write(const int16_t *mono, size_t frames);

/**
 * @brief Set output volume.
 * @param vol 0..100
 */
void app_speaker_set_volume(uint8_t vol);

/**
 * @brief Stop the drain task and close the device.
 */
void app_speaker_stop(void);

/**
 * @brief 立刻静音（把 DAC 增益拨到 0），用于"退出页面要马上没声音"。
 *
 * 不走 close/open：这个音频设备是共享的（AI 侧采集也挂在上面），close 会连
 * DMA/ISR 一起拆掉，真机上表现为 `Assert at: xQueueGiveFromISR`。
 * 已经递交到驱动 FIFO 的那一段（约 1 秒）照样播，但播出来是静音。
 * 音量由 drain 任务在驱动队列放空后自动还原，调用方不用管。
 */
void app_speaker_silence_now(void);

/**
 * @brief Available free space in the ring buffer (bytes).
 *
 * Returns 0 if the speaker is not initialised.  Useful for callers that
 * need to pace their writes to avoid drops.
 */
uint32_t app_speaker_room(void);

/**
 * @brief How much audio is queued in the ring buffer right now, in ms.
 *
 * A producer that wants to report "where playback actually IS" has to subtract
 * this from what it has fed: anything still queued has not been heard yet. The
 * movie audio track uses it to turn "samples written" into "seconds played", so
 * the A/V drift it logs is a real offset rather than a constant buffer depth.
 *
 * Returns 0 if the speaker is not initialised.
 */
uint32_t app_speaker_buffered_ms(void);

/**
 * @brief 固定延迟：数据"交给驱动"到"真的从喇叭出来"之间的毫秒数。
 *
 * app_speaker_buffered_ms() 只覆盖**本模块**的环形缓冲。在 drain 任务之下
 * 还有一级驱动内缓冲（platform/T5AI/tuyaos/tuyaos_adapter/src/driver/
 * tkl_audio.c：DRIVER_SPEAK_FIFO_FRAME_NUM(2) x TIME_SAMPLE_MS(20) = 40ms），
 * 稳态下被 DAC 一直喂满。压在那里的数据**已经离开本模块，但还没被听到**。
 *
 * 所以"写了多少采样 -> 播到第几秒"的换算必须把这两级都扣掉，否则算出来的
 * 位置会恒定超前 40ms —— 听感上就是**画面和字幕都走在声音前面**。
 *
 * 未初始化时返回 0。
 */
uint32_t app_speaker_driver_latency_ms(void);

/**
 * @brief Blocks dropped because the ring buffer was full
 *        (producer outrunning the drain task).
 */
uint32_t app_speaker_dropped(void);

/**
 * @brief Times the drain task found the ring buffer completely empty
 *        (DAC starving -- this is what stutter sounds like).
 */
uint32_t app_speaker_underrun(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SPEAKER_H__ */