/**
 * @file app_spf6.h
 * @brief `.spf` 帧序列容器的**共享读取器**（当前是 SPF7）。
 *
 * 谁在用
 * ------
 *   - `app_sprite2d.c`  桌宠精灵（`/sdcard/sprite/fish_elf.spf`）
 *
 * （老产品还有 `app_role2d.c` 读同一格式的 `fish_role.spf`，那个页面删了。）
 *
 * 格式解析**只留一份**：头、名字表、偏移表、PackBits 解压、行跨度（spans）。
 * 二进制格式的解析器有两份实现，迟早会有一份悄悄跑偏 —— 而症状是"角色画出来
 * 不对"，很难查。
 *
 * 这里**不含策略**
 * ----------------
 * 读失败怎么办（重试？退回常驻 idle？重建 SD 链路？）、什么时候读、多久读一次 ——
 * 全是调用方的事。本模块只提供"给定帧号，告诉我它在文件里的字节区间"和
 * "把这段字节解成像素"，读由调用方用 `spf6_read_at()` 自己发起。
 * 精灵的浮层要能"卡拔了也一直在"，角色页只要"读不到就不画"，两边的容错
 * 要求根本不同，塞进这里只会让两边都别扭。
 *
 * 文件格式（小端）
 * ----------------
 *     [magic "SPF7"][u16 nc][u16 rsv][u16 gw][u16 gh]
 *     [nc x { u16 frames, u16 fps, u16 w, u16 h, s16 dx, s16 dy, u16 role }]
 *     [名字表：nc x { u8 len, ASCII }，补 0 到 4 字节对齐]
 *     [u32 offs[total+1]]      每帧 RLE 的起始偏移；最后一项 = 数据区结束
 *     [各帧的 PackBits 数据，按全局帧序排列]
 *
 * 各组的 `w/h/dx/dy` 都是**自己裁剪框**的（不是全图共用一个框）；`dx/dy` 是相对
 * **锚点框左上角**的偏移，锚点框 = 所有组裁剪框的并集。所以切动作时角色不会跳。
 * 生成端是 `tools/pack_seq.py`。
 */

#ifndef __APP_SPF6_H__
#define __APP_SPF6_H__

#include <stdint.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 动作名的最长字节数（含结尾 0）。最长的现名是 `rnd_lookaround`（14）。 */
#define SPF6_NAME_MAX       16

/** 动作组数上限（和 tools/pack_seq.py 的 `--order` 长度对齐）。
 *  现在实打实用到 22 组（待机1 + 贴边2 + 点击5 + 拖拽3 + 说话1 + AI表情2 + 随机8），
 *  留点余量给以后加动作。超了在 spf6_open 里直接报错，不静默截断。 */
#define SPF6_MAX_CLIPS      24

/** 一行里非透明像素的范围 [x0, x1)；x0 >= x1 表示整行透明。 */
typedef struct {
    uint16_t x0, x1;
} sp2d_span_t;

/** 动作组在固件里扮演什么角色 —— 决定"什么时候播它"。
 *  打包器按名字前缀写进记录的 role 字段（idle* / click* / drag* / …）。 */
typedef enum {
    SP2D_ROLE_IDLE = 0,     /**< 待机：常态循环，其它都没得播时播它 */
    SP2D_ROLE_CLICK,        /**< 点击回应：戳一下播一遍 */
    SP2D_ROLE_DRAG,         /**< 拖拽中：**悬空循环**（手按着的时候） */
    SP2D_ROLE_RANDOM,       /**< 随机表演：闲久了隔一阵自己演一个 */
    SP2D_ROLE_SNAP_R,       /**< 贴右边缘时显示的那张（素材已按右边缘裁好） */
    SP2D_ROLE_SNAP_L,       /**< 同上，左边缘用的镜像版 */
    SP2D_ROLE_DRAG_UP,      /**< 拖拽起手：地上 -> 飘起来，**演一遍** */
    SP2D_ROLE_DRAG_DOWN,    /**< 松手落地：飘 -> 落回地面，**演一遍** */
    SP2D_ROLE_TALK,         /**< AI 回复中：循环播（"对屏碎碎念"），说完回待机 */
    SP2D_ROLE_AI,           /**< AI 表情（开心/思考）：**只按名字点名播**，
                             *   不进精灵的随机池 —— 否则桌宠自己就会演起它们 */
} sp2d_role_e;

/** 一组动作。 */
typedef struct {
    char     name[SPF6_NAME_MAX];   /**< 动作名（SPF7 名字表；空串 = 老文件没有） */
    uint16_t frames;        /**< 本组帧数 */
    uint16_t fps;           /**< 本组播放帧率 */
    uint16_t w, h;          /**< 本组的帧尺寸（各组可以不同） */
    int16_t  dx, dy;        /**< 本组裁剪框左上角相对锚点框的偏移（可为负） */
    uint16_t role;          /**< 本组的角色，见 sp2d_role_e */
    uint32_t first;         /**< 本组首帧在全局帧数组里的下标 */
    uint32_t data_off;      /**< 本组首帧在文件里的偏移（读帧 O(1)） */
} sp2d_clip_t;

/** 一个打开着的 `.spf` 文件。 */
typedef struct {
    sp2d_clip_t *clips;         /**< 动作表（spf6_open 分配，spf6_close 释放） */
    uint32_t    *offs;          /**< 帧号 -> 文件偏移（total_frames+1 项，同上） */
    uint16_t     clip_count;
    uint32_t     total_frames;
    uint16_t     max_w, max_h;  /**< 各组里最大的帧尺寸 —— 流式缓冲按它分配 */
    uint16_t     gw, gh;        /**< 锚点框尺寸 */
    uint32_t     data_off;      /**< 帧数据区起始（= offs[0]） */
    TUYA_FILE    f;             /**< 文件句柄；NULL = 没开 */
    char         path[64];      /**< 打开时的路径（链路重挂后按它重开）*/
    uint32_t     media_gen;     /**< 打开时/上次重开时的链路代次 */
} SPF6_FILE_T;

/**
 * @brief 打开并解析一个 `.spf`。
 *
 * @param[in,out] pf      输出。**失败时内部已清理干净**，调用方不必再 close。
 * @param[in]     path    SD 卡上的路径。
 * @param[in]     max_w   允许的单帧宽上限（调用方自己的缓冲约束）。
 * @param[in]     max_h   同上，高。
 * @return OPRT_OK；OPRT_INVALID_PARM 格式/参数不对；OPRT_MALLOC_FAILED 表分配失败；
 *         OPRT_NOT_FOUND 文件打不开。
 *
 * @note 版本不匹配时的报错里会写清"用 tools/pack_seq.py 重新生成" ——
 *       老格式不保留兼容路径，留一条只会让"到底跑的是哪版"更难查。
 */
OPERATE_RET spf6_open(SPF6_FILE_T *pf, const char *path, uint16_t max_w, uint16_t max_h);

/** @brief 关文件并释放表。可重复调用。 */
void spf6_close(SPF6_FILE_T *pf);

/** @brief 从文件 @a off 处读 @a len 字节到 @a buf。纯 seek+read，不重试。 */
BOOL_T spf6_read_at(SPF6_FILE_T *pf, uint32_t off, uint32_t len, void *buf);

/**
 * @brief 取第 @a idx 帧（全局帧号）在文件里的字节区间。
 * @return 0 成功；-1 帧号越界或区间非法（调用方按自己的策略处理）。
 */
int spf6_frame_span(const SPF6_FILE_T *pf, uint32_t idx, uint32_t *off, uint32_t *len);

/** @brief 第 @a idx 组的描述；越界返回 NULL。 */
const sp2d_clip_t *spf6_clip_of(const SPF6_FILE_T *pf, uint32_t idx);

/** @brief 按名字找组号；找不到返回 -1（名字比较区分大小写，与文件里一致）。 */
int spf6_clip_by_name(const SPF6_FILE_T *pf, const char *name);

/** @brief 一组里 RLE 的**最坏**字节数（分配暂存缓冲用）。 */
uint32_t spf6_rle_worst(uint32_t npx);

/** @brief 一组的行跨度表字节数（分配用；spans 按 max_h 行算）。 */
uint32_t spf6_spans_bytes(uint16_t max_h);

/**
 * @brief PackBits 解压（tools/pack_seq.py 的 packbits 是编码端，两边必须一致）。
 *
 * 控制字节 c：0..127 后面跟 c+1 个字面像素（2 字节/个，小端）；
 * 129..255 后面跟 1 个像素、重复 257-c 次；128 编码端不产生。
 *
 * 越界一律**截断**而不是报错：宁可这一帧少几个像素，也不要因为一段坏数据把整块
 * 画面丢掉。@return 实际解出的像素数（正常等于 @a n）。
 */
uint32_t spf6_inflate(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t n);

/**
 * @brief 扫出一段连续帧里每帧每行的非透明范围。
 *
 * @a spans 的步长按 @a h 走（缓冲按 max_h 分配，短组用不满也没关系）。
 */
void spf6_build_spans(const uint16_t *px, sp2d_span_t *spans,
                      uint32_t count, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SPF6_H__ */
