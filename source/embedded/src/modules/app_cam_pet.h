/**
 * @file app_cam_pet.h
 * @brief 合影用的角色立绘（绿幕抠像后的 RGB565A8 常量位图）。
 *
 * 数据由 `third_party/sprite/make_compose_pet.py` 生成，源图
 * `third_party/my_pet/右侧半身比耶.png`。换图就重新跑一次脚本，别手改 .c。
 *
 * 为什么编进 flash 而不是放卡上：只有约 38KB，而且合影是拍照流程的一部分，
 * 不该因为卡上少一个文件就静默失败。
 */

#ifndef __APP_CAM_PET_H__
#define __APP_CAM_PET_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;    /* RGB565A8：前 w*h*2 字节 RGB565，后 w*h 字节 alpha */
    int            w;
    int            h;
} APP_CAM_PET_T;

/** @brief 角色立绘（永远有效；没跑过生成脚本时 w/h 为 0）。 */
const APP_CAM_PET_T *app_cam_pet_get(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CAM_PET_H__ */
