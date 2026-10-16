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
#define TUYA_PRODUCT_ID "sayojo04prxu4kts"
#endif

#define TUYA_OPENSDK_UUID    "uuid6fe85fc0cfccde00"             // TODO: your uuid
#define TUYA_OPENSDK_AUTHKEY "mAAQGYfRNzt2jn7AFM87qMvBGVmncG0L" // TODO: your authkey

#endif /* TUYA_CONFIG_H_ */
