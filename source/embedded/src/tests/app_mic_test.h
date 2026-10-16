/**
 * @file app_mic_test.h
 * @brief Microphone loopback test: record 2s, wait 1s, play back.
 */

#ifndef __APP_MIC_TEST_H__
#define __APP_MIC_TEST_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record 2 seconds of microphone audio at 16kHz mono, then play it
 *        back through the onboard speaker after a 1-second pause.
 *
 * This temporarily takes over the audio device from app_speaker, so the
 * normal speaker playback is stopped for the duration of the test.
 *
 * @return OPRT_OK on success, error code otherwise.
 */
OPERATE_RET app_mic_test_start(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_MIC_TEST_H__ */
