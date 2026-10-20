/**
 * @file pet_snapshot.h
 * @brief 一帧精灵的静态快照（给拍照页的"实景合影"预览用）。
 *
 * demo 的拍照页往预览里贴的是那张 932KB 的内嵌 `pet_char`。本产品没有那张图 ——
 * 角色是 SD 卡上的实时动画。所以这里用精灵引擎**渲染一帧**到一个 lv_canvas 里，
 * 得到同样的视觉效果，代价是一块画布缓冲（约 77KB PSRAM）和一个一次性的一帧
 * SD 读。页面删掉时缓冲跟着释放。
 */

#ifndef __PET_SNAPSHOT_H__
#define __PET_SNAPSHOT_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 渲染精灵的第 0 组第 0 帧，做成一个 lv_canvas 子对象。
 *
 * @param[in] parent 父对象（拍照页的预览场景）。
 * @param[in] bg     画布底色（RGB565）。用预览场景自己的底色，快照才不像贴上去的。
 * @return 画布对象；精灵没加载 / PSRAM 不足时返回 NULL（调用方跳过贴图即可）。
 */
lv_obj_t *pet_snapshot_create(lv_obj_t *parent, uint16_t bg);

#ifdef __cplusplus
}
#endif

#endif /* __PET_SNAPSHOT_H__ */
