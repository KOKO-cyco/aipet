/**
 * @file app_mic_record.h
 * @brief VAD-triggered mic recording: one round per call.
 */

#ifndef __APP_MIC_RECORD_H__
#define __APP_MIC_RECORD_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One round of VAD recording:
 *   - stop speaker
 *   - init VAD + open mic
 *   - wait for voice activity
 *   - record up to 2 s of 16 kHz mono PCM (stop after 2 s silence)
 *   - play back the captured audio
 *   - restore speaker
 *
 * This function blocks until the round is complete.
 *
 * @return OPRT_OK on success, OPRT_TIMEOUT if VAD never saw speech in
 *         MR_VAD_WAIT_TIMEOUT_MS, error code otherwise.
 */
OPERATE_RET app_mic_record_once(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MIC_RECORD_H__ */
