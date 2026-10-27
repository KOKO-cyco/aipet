/**
 * @file tuya_config.h
 * @brief IoT fallback configuration.
 *
 * The device license (UUID / AUTHKEY) is normally read from flash
 * (tuya_authorize_read) - burn it once with the tyutool authorize command.
 * The values below are only used when the flash license is missing.
 */

#ifndef TUYA_CONFIG_H_
#define TUYA_CONFIG_H_

#include "tuya_cloud_types.h"

#ifndef TUYA_PRODUCT_ID
#define TUYA_PRODUCT_ID "mqnzcaztvpwkkcyj"
#endif

#define TUYA_OPENSDK_UUID    "uuid472dc38b2848b53b"
#define TUYA_OPENSDK_AUTHKEY "yx146G6FpfXJIq5WuyizvWx2PGLfLcAJ"

#endif /* TUYA_CONFIG_H_ */
