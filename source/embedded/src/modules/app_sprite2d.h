/**
 * @file app_sprite2d.h
 * @brief 2D 精灵：**帧序列**播放器。见 app_sprite2d.c。
 *
 * 为什么是帧序列而不是骨骼
 * ------------------------
 * 两条路都验证过，最终选了帧序列：
 *
 *   |            | 帧序列(本文件) | 骨骼刚性(当初实测)          |
 *   |------------|---------------|---------------------------|
 *   | 每帧开销    | ~2ms（实测）   | 8~9ms（12 部件，实测）      |
 *   | 画质        | 现成美术，无接缝 | 刚性切片，有"皮影戏"风险   |
 *   | 工作量      | 切帧 + 打包    | 切部件 + 定轴心 + 绑定      |
 *   | 动作可组合  | ❌            | ✅                         |
 *
 * 图集是 RGBA 的，所以"帧序列画不出来"这个顾虑不成立 —— 它天然带 alpha。
 *
 * 数据流
 * ------
 *   SD 卡上的 .spf 文件  →  表 + 待机组常驻 PSRAM  →  其余帧按需从 SD 流式读
 *
 * 放 SD 而不是编进固件：和项目里字库一致，且固件分区有限。
 *
 * 怎么用（桌宠页的用法）
 * ----------------------
 *   1) `app_sprite2d_init()` 一次（幂等）
 *   2) `app_sprite2d_size()` 拿到锚点框尺寸 → 按它 `tal_psram_malloc` 一块画布缓冲
 *   3) 每帧 `app_sprite2d_render_clip(buf, w, h, 组号, 帧号, 背景色)`，再把画布标脏
 *
 * 组号用 `app_sprite2d_clip_name()` 按名字找（没有 name→index 的查表接口，
 * 组数只有二十来个，线性 strcmp 就够）。
 */

#ifndef __APP_SPRITE2D_H__
#define __APP_SPRITE2D_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 动作组数上限 / 单帧尺寸上限（超了就在加载时报错，不静默截断）。
 *
 *  组数上限 24：现在实打实用到 21 组（待机1 + 贴边2 + 点击4 + 拖拽3 + 说话1
 *  + AI 表情3 + 随机8），留点余量给以后加动作。
 *
 *  宽度上限 288：最宽的那几组（比如"蓝鲸现世"）裁剪框接近这个数 —— 素材本身
 *  如此，不是脏数据。流式单帧缓冲按**实际最大帧**分配，所以放宽上限不额外吃内存。 */
#define SP2D_MAX_CLIPS      24
#define SP2D_MAX_W          288
#define SP2D_MAX_H          208

/** 精灵文件路径。由 `third_party/sprite/build_fish.sh` 产出（SPF7 格式，见 app_spf6.h）。 */
#define SP2D_PATH_SPF       "/sdcard/sprite/fish_elf.spf"

/** 上一次渲染的统计。 */
typedef struct {
    uint32_t us_total;      /**< 整帧耗时 */
    uint32_t px;            /**< 实际混合的像素数（已扣掉跳过的透明区） */
    uint16_t frame_idx;     /**< 当前动作内第几帧 */
    uint16_t frame_cnt;     /**< 当前动作的帧数 */
    uint16_t clip_idx;      /**< 当前第几组动作 */
    uint16_t clip_cnt;      /**< 动作组数 */
    uint8_t  fallback;      /**< 1 = 本帧 SD 读失败，画的是常驻 idle */
} SP2D_STAT_T;

/**
 * @brief 从 SD 加载精灵（幂等）。
 * @return OPRT_OK；OPRT_NOT_FOUND 文件不存在；OPRT_MALLOC_FAILED PSRAM 不足。
 */
OPERATE_RET app_sprite2d_init(void);

/** @brief 释放帧数据。 */
void app_sprite2d_deinit(void);

/** @brief 取锚点框尺寸（加载成功后才有意义）。 */
void app_sprite2d_size(int *w, int *h);

/**
 * @brief 把当前帧混合到目标（不铺背景）。
 *
 * 自动调度版：按 @a t_ms 在"待机随机轮播 / 一次性动作插播"之间自己选。
 * 需要精确控组就用 `app_sprite2d_render_clip()`。
 *
 * @param[in,out] dst     目标基址
 * @param[in]     stride  目标行跨距（像素）
 * @param[in]     dst_w   目标宽（像素），用于裁剪
 * @param[in]     dst_h   目标高（像素）
 * @param[in]     ox      精灵左上角在目标里的 x
 * @param[in]     oy      精灵左上角在目标里的 y
 * @param[in]     t_ms    动画时间（毫秒），决定播到第几帧
 */
void app_sprite2d_render_to(uint16_t *dst, int stride, int dw, int dh,
                            int ox, int oy, uint32_t t_ms);

/**
 * @brief 把当前帧渲染到独立缓冲（先铺背景）。自动调度版。
 */
void app_sprite2d_render(uint16_t *dst, int w, int h,
                         uint32_t t_ms, uint16_t bg);

/**
 * @brief 按**组号 + 组内帧号**直接渲染，不走进 idle / 随机那套调度。
 *
 * 调用方要精确控制放哪一组时用它。帧号按本组帧数取模。
 */
void app_sprite2d_render_clip(uint16_t *dst, int w, int h,
                              int clip, int frame, uint16_t bg);

/** 一帧**实际画过**的矩形（画布坐标，闭区间 [x0,x1]×[y0,y1]）。
 *
 * 不是裁剪框：框里有透明 pad 和起跳余量，角色真正画到的只有其中一块
 * （待机 ~129×152，画布 193×199）。脏矩形渲染只清、只标这一块。 */
typedef struct {
    int16_t x0, y0, x1, y1;
} SP2D_RECT_T;

/**
 * @brief 整帧渲染，但**只动角色那一块**：先把上一帧实际画过的矩形铺回背景色，
 * 再画当前帧，输出两帧的并集。
 *
 * 和 `app_sprite2d_render_clip()` 的区别：那个每帧把整块画布
 * （193×199 ≈ 150KB）memset 一遍、整块标脏；这个只碰 ~40KB，页面拿并集
 * 去 `lv_obj_invalidate_area()`。读失败退回 idle 时按**实际画出的 idle 帧**
 * 取矩形，所以切组 / 读失败都不会留残影。
 *
 * SD 读 + RLE 解压在预取线程（sp2d_pf）里做，本函数**只从解码槽取**：
 * 槽没好就复读本组上一帧 / 常驻首帧 / 常驻 idle，绝不在 LVGL 线程里读卡。
 *
 * @param[out] dirty 两帧并集（画布坐标）。可以为 NULL。
 * @return TRUE = 画了（dirty 有效，调用方该标脏）；FALSE = 没画（参数错 /
 *         没加载 / 这一帧什么都取不到），画布保持原样、别标脏。
 */
BOOL_T app_sprite2d_render_clip_dirty(uint16_t *dst, int w, int h,
                                      int clip, int frame, uint16_t bg,
                                      SP2D_RECT_T *dirty);

/**
 * @brief 清掉"上一帧画过哪"的记录（下一次从整块画布重新铺起）。
 *
 * 什么时候调：画布重建、进出桌宠页。不调的话上一次页面的矩形状态会串到
 * 下一次（表现为残留一小块上一帧的角色）。同时放掉预取槽的占用。 */
void app_sprite2d_dirty_reset(void);

/**
 * @brief 让某一组"倒带"：作废它的预取槽和 held，从组首重新供帧。
 *
 * 什么时候调：**同一个动作被重新点了一次**（上一遍还没演完）。这时页面把
 * 帧号归零，但组的名字没变，预取线程不会换代 —— 槽里装的还是上一遍靠后的
 * 帧，held 也是靠后的姿势。不作废的话，重播的前几拍会在"组首帧 ↔ 上次播到
 * 的姿势"之间来回跳（实测的抽搐）。
 *
 * 跨组请求也会调它（顺带把新组的旧槽清掉），所以实现里不区分"是不是同一组"。
 * 只能在 LVGL 线程里调；内部只拿短锁，不读卡。
 */
void app_sprite2d_rewind(int clip);

/**
 * @brief 同上，但**不铺背景** —— 只把精灵混合上去。
 *
 * 背景已经由调用方填好的时候用它。桌宠页就是这么用的：它先把拍到的照片里
 * 对应的那一块拷进画布当背景（照片是常驻的背景层），再调这个把宠物叠上去。
 * 不铺背景是为了让照片透出来 —— RGB565 画布没有 alpha 通道，
 * "铺个色再画" 和 "铺照片再画" 只能二选一。
 */
void app_sprite2d_blit_clip(uint16_t *dst, int w, int h, int clip, int frame);

/** @brief 动作组数（没加载返回 0）。 */
int app_sprite2d_clip_count(void);

/** @brief 第 @a clip 组的帧数（越界返回 0）。 */
int app_sprite2d_clip_frames(int clip);

/** @brief 第 @a clip 组的名字（越界返回 "?"，不会返回 NULL）。 */
const char *app_sprite2d_clip_name(int clip);

/**
 * @brief 读预取线程的命中/缺失累计（**只读，不清零** —— 清零归 2s 统计窗口）。
 *
 * 桌宠页的帧率自适用它算 miss 率：复读也算 miss，miss 率高 = SD 供不上，
 * 该把拍子拉长保流畅。调用方自己算差量；计数为累计值（永不清零），
 * 只有 uint32 回绕时后值会小于前值，遇到就重建基线。
 */
void app_sprite2d_pf_get(uint32_t *hit, uint32_t *miss);

/**
 * @brief 取第 @a clip 组的**裁剪框**（相对锚点框左上角）。
 *
 * 角色在画布里的位置由它决定，而**原点 = 脚底中点 = `(dx + w/2, dy + h)`**
 * —— 各组的内容框都对齐到同一个原点，所以切动作时脚不动。
 *
 * 要"按原点摆放"（而不是按锚点框摆放）就得先拿这个：锚点框是并集，它的中心
 * 和角色的原点不是一回事（差了十几像素）。
 *
 * 越界或没加载时四个输出都是 0。指针可以为 NULL（不取那一项）。
 */
void app_sprite2d_clip_box(int clip, int *dx, int *dy, int *w, int *h);

/** @brief 取上次渲染统计。 */
void app_sprite2d_stat(SP2D_STAT_T *out);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SPRITE2D_H__ */
