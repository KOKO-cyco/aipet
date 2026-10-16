/**
 * @file app_preload.h
 * @brief 后台把 SD 链路拉起来，并放行"需要 SD 的页面"。
 *
 * 解决什么问题
 * ------------
 * 宠物页要用卡上的精灵（`fish_elf.spf`）和字库，所以声明了 `APP_PAGE_NEEDS_SD`。
 * 开机那一两秒卡还没热身完，直接放它进去会读到空。这个线程在后台等卡可用、
 * 把链路拉起来，然后解除门控（`app_storage_warm_done()`）——
 * **这是门控唯一的出口**，它起不来就等于把用户永久挡在宠物页外面
 * （`tuya_app_main.c` 里对 `app_preload_init()` 失败有兜底：直接解除门控）。
 *
 * 换卡也管
 * --------
 * `app_storage_gen()` 在每次拔插 / 链路重建成功时 +1；代次变了就重跑一遍，
 * 所以换了卡不用重启。
 *
 * 老产品里这个线程还负责"把电影/音乐/游戏目录先扫好"，那些页面删掉之后
 * 扫描就没了 —— 现在它只剩"拉链路 + 放行"这一件事。
 *
 * 优先级取最低档：这是"有空才做"的事，绝不允许挤占 UI / 音频。
 */

#ifndef __APP_PRELOAD_H__
#define __APP_PRELOAD_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 起后台线程（幂等）。放在路由跑起来之前调用。
 *
 * 线程自己会等卡可用（`app_storage_fault()`），所以开机没卡也不会白忙；
 * 插卡后代次变化（`app_storage_gen()`），它自然补做一遍。
 */
OPERATE_RET app_preload_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_PRELOAD_H__ */
