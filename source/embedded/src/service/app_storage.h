/**
 * @file app_storage.h
 * @brief 存储服务：SD 卡挂载、目录枚举、UTF-8 与 OEM 码页互转。
 *
 * 解决什么问题
 * ------------
 * 本平台的 VFS 会把目录里的文件名以 UTF-8 交给应用，而 f_open() 按 FatFs 配置的
 * OEM 代码页（本工程是 CP936/GBK）解释同一串字节 —— 两者只在 ASCII 上一致，所以
 * 中文名文件"看得见、打不开"。唯一稳的办法是把候选路径**真的打开一次**来验证，
 * 先试目录给出的原样字节，再试重新编码成 OEM 码页的形式。
 *
 * 这段逻辑以前在老产品里散着写了好几遍（NES 的 ROM 扫描一遍、角色动画一遍）。
 * 现在收进本模块一次，所有要列目录的地方（字体、精灵、未来的备忘录附件）都用
 * 同一个迭代器。
 *
 * 用法
 * ----
 * 回调式迭代，避免让调用者开大数组（128 个 320 字节的路径就是 40KB）：
 *
 *   static BOOL_T on_music(const APP_FILE_T *f, void *user) {
 *       PR_NOTICE("找到 %s (%u 字节)", f->name, f->size);
 *       return TRUE;                     // FALSE 提前结束枚举
 *   }
 *   int32_t n = app_storage_iterate("/sdcard/music", ".mp3", on_music, NULL);
 *
 * @note 回调里的 @a file 指向栈上缓冲，下一次回调会覆盖，需要留存请自行拷贝。
 */

#ifndef __APP_STORAGE_H__
#define __APP_STORAGE_H__

#include <stdint.h>

#include "tkl_fs.h"
#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_STORAGE_MOUNT       "/sdcard"
#define APP_STORAGE_NAME_MAX    128      /* UTF-8 文件名缓冲 */
#define APP_STORAGE_PATH_MAX    320      /* 完整路径缓冲（OEM 字节可能更长） */

/**
 * SDIO 4 线开关 —— 想退回 1 线，把这里改成 0 就行。
 *
 *   1（当前）：开机协商 4 线，失败自动降级 1 线。板子硬件是完整 4 线
 *             （CLK2 CMD3 D0=4 D1=5 D2=10 D3=11），20MHz 下约 2.5 -> 10MB/s。
 *   0        ：**整段协商与降级代码不参与编译**，行为与加这个功能之前完全一致
 *             —— SDK 默认 1 线，`app_storage_sdio_setup()` 变成空操作，挂载
 *             仍旧由 `app_storage_init()` 懒加载。
 *
 * **2026-10-13 从 0 改成 1。** 起因是实测发现 1 线是全局瓶颈：角色页单帧
 * `read=18ms`（25.6KB → 1.4MB/s，基本就是 1 线 20MHz 的天花板），电影页同样
 * `read=13ms`，开机扫三个目录要 3 秒。4 线能让这些全部快约 4 倍。
 *
 * 之前一直是 0，但 git 里只有一个基线提交、没有"开了又关"的记录 —— 要么从没在
 * 真机上试过，要么试过有问题没记下来。所以**这一版要重点看开机的
 * `[sto] SDIO 4 线 @20MHz 就绪` / `[sto] 4 线起卡失败，降级到 1 线重试`**：
 * 前者说明 4 线通了，后者说明自动降级生效（慢但稳）。两个都没有才是问题。
 *
 * 也可以不改文件、在编译期覆盖（本工程是 -Werror，别写成 -Wunused 的形式）：
 * 给 CMakeLists.txt 加 `add_definitions(-DAPP_STORAGE_SDIO_4BIT=0)`。
 */
#ifndef APP_STORAGE_SDIO_4BIT
#define APP_STORAGE_SDIO_4BIT   1
#endif

typedef struct {
    char     name[APP_STORAGE_NAME_MAX];   /* UTF-8 文件名，可直接喂给 LVGL 字体 */
    char     path[APP_STORAGE_PATH_MAX];   /* 已经验证过能 tkl_fopen 的完整路径   */
    uint32_t size;                         /* 文件字节数                          */
} APP_FILE_T;

/** 目录项（见 app_storage_iterate_dirs）。 */
typedef struct {
    char     name[APP_STORAGE_NAME_MAX];   /* UTF-8 目录名                        */
    char     path[APP_STORAGE_PATH_MAX];   /* 已经验证过能 tkl_dir_open 的路径    */
} APP_DIR_T;

/** 枚举命中回调。返回 FALSE 提前结束枚举。 */
typedef BOOL_T (*APP_STORAGE_HIT_CB)(const APP_FILE_T *file, void *user);

/** 目录枚举命中回调。返回 FALSE 提前结束枚举。 */
typedef BOOL_T (*APP_STORAGE_DIR_CB)(const APP_DIR_T *dir, void *user);

/**
 * @brief SDIO 线宽协商：先按 **4 线** 起卡，起不来自动退 **1 线**。
 *
 * 为什么需要它：SDK 里 `sd_card_driver.c` 被 Tuya 改成运行期决定线宽
 * （`bus_width = tuya_sdio_line_mode()==4 ? 4LINE : 1LINE`），而那个变量初值
 * 是 1BIT、唯一能改它的 `tkl_sdio_init()` **全 SDK 没有任何调用者** —— 不改就
 * 永远 1 线。板子硬件是完整 4 线（CLK2 CMD3 D0=4 D1=5 D2=10 D3=11），4 倍带宽
 * 是白捡的：20MHz 下约 2.5MB/s -> 约 10MB/s。
 *
 * 必须在**任何** SD 访问之前调一次（尤其是任何 `tkl_fs_mount` 之前）：线宽只在
 * `bk_sd_card_init()` 里生效一次，卡起来之后再改就晚了。
 *
 * 幂等；失败返回错误（卡不在，或两种线宽都起不来）。
 *
 * @note `APP_STORAGE_SDIO_4BIT == 0` 时本函数是**空操作**（直接返回 OK，
 *       挂载仍走 `app_storage_init()` 的老路径），调用点不用加条件编译。
 */
OPERATE_RET app_storage_sdio_setup(void);

/** @brief 当前生效的 SDIO 数据线数：4 / 1，0 表示没起来。
 *         `APP_STORAGE_SDIO_4BIT == 0` 时恒返回 1（SDK 默认）。 */
int32_t app_storage_sdio_lines(void);

/**
 * @brief 把卡切成"能写"的那一档。**写卡前必须调，写完必须调 write_end 切回来。**
 *
 * 为什么需要它（2026-10-20 实测，方案 B）：这块板 **4 线每一档时钟都写不进**
 * （`wr_status = 0x7`，13M/6.5M/3.2M/1.6M 全败），只有 **1 线 @13M 一次通过**；
 * 可 1 线的读带宽只有 4 线的 1/6~1/10，常驻 1 线会让精灵预取从 22~60ms/批 掉到
 * 150~320ms/批，桌宠肉眼可见掉帧。两件事本来只能二选一。
 *
 * 所以改成**分时使用**：平时停在 4 线（读最快）跑动画，只在按快门那一刻切到写档，
 * 写完立刻切回 4 线。切档 = unmount -> 换线宽重起卡 -> mount，约 1~1.5 秒，而抓
 * 图本身就要等 2 秒左右，用户感知不到。
 *
 * 线宽只在 `bk_sd_card_init()` 里生效一次，运行期改不了，所以只能整条重起 ——
 * 切档期间本函数会占住 SD 临界区（见 app_storage_busy_enter），读者一律排队等着，
 * 不会有人在卷消失的瞬间去读它。
 *
 * @note **锁不可重入**：本函数内部自己取放临界区，调用方别再包一层。
 * @note 开机扫描没找到任何能写的档时返回 OPRT_COM_ERROR（那时 write_end 是空操作）。
 *
 * @return OPRT_OK 现在可以写；其它值：这张卡没有能写的档位 / 切档失败。
 */
OPERATE_RET app_storage_write_begin(void);

/** @brief 切回读最快的那一档。必须与 `app_storage_write_begin()` 成对调用。 */
void app_storage_write_end(void);

/** @brief 开机扫到的"能写"线宽；**0 = 这张卡写不进**（照片一定存不上）。 */
int32_t app_storage_write_lines(void);

/** @brief 挂载 SD 卡（幂等，已挂载时直接成功）。 */
OPERATE_RET app_storage_init(void);

/**
 * @brief SD 卡链路整体重建（unmount -> 卡 deinit/init -> 重新 mount）。
 *
 * **这是"读错"那条路**，跟"卡在不在"（CD 引脚，见下面的 app_storage_cd_init）
 * 是两回事：卡还插着、但读出来的数据是坏的（SDIO FIFO 读超时之后的死态），
 * 轻量复位救不回来，只有完整重建有效。
 *
 * **前提：调用方必须已关闭 SD 上的全部文件句柄**，否则 unmount 行为未定义。
 * 典型调用者是动画播放循环：角色页上它唯一常开的句柄就是它自己那个。
 *
 * @return OPRT_OK 重建并重新挂载成功。
 */
OPERATE_RET app_storage_recover(void);

/**
 * @brief "把卡弄成能读的" —— 能读就直接返回，读不动才重建。
 *
 * 跟 `app_storage_recover()` 的区别是它**会先验证**：换卡之后 SDIO 链路是死的
 * （卡被物理拔走过），FatFs 却不知道，这时直接扫目录会扫出残缺的结果；但开机
 * 那次链路是刚起好的，白白重建两秒没意义。
 *
 * 给**预热线程**用（见 app_preload）：插卡后它抢在浮层前面把链路救活，然后才扫。
 *
 * @return OPRT_OK 链路可用（本来就可用，或重建成功）；其它值：卡不在 / 救不回来。
 */
OPERATE_RET app_storage_prepare(void);

/**
 * @brief SD 故障类型 —— **"卡不在"和"读错"两条路唯一的汇总点**。
 *
 * 只有两个来源：CD 引脚（卡在不在）和链路重建（卡在、但救不回来）。页面的门控、
 * 播放器的收摊、要不要弹 SD 故障页、弹哪一句文案，全看这一个值。
 */
typedef enum {
    APP_SD_OK      = 0,   /**< 一切正常                                    */
    APP_SD_ABSENT  = 1,   /**< 卡不在位（CD 引脚判定，已去抖）             */
    APP_SD_DAMAGED = 2,   /**< 卡在位，但链路重建后仍读不动                */
} APP_SD_FAULT_E;

/** @brief 当前 SD 故障类型（APP_SD_OK = 无故障）。 */
APP_SD_FAULT_E app_storage_fault(void);

/**
 * @brief 目录索引还在预热（卡刚插上 / 刚开机，各模块的列表还没扫好）。
 *
 * **只给"要不要放用户进 SD 页面"用**，跟 `app_storage_fault()` 是两回事：
 *   - 链路重建成功 ≠ 预热完了 —— 缓存还是冷的，放进去照样要现场扫 3 秒
 *   - 预热期间 SD 本身是**能读**的，浮层小精灵照样要能读 .spf 播动作
 *     （所以读者一律看 `app_storage_ready()`，不看这个）
 *
 * 置位：开机（有卡）时、每次 CD 报"插入"时。
 * 清除：预热线程把三个列表都扫完（见 app_preload），成功失败都清。
 *
 * 门控语义：`app_storage_ready() && !app_storage_warming()` 才放行。预热没完时
 * 推门页显示"正在识别存储卡…"，扫完自动撤掉 —— 用户进去时缓存一定是热的。
 */
BOOL_T app_storage_warming(void);

/** @brief 预热跑完了（成功失败都算）：解除 warming。由预热线程调用。 */
void app_storage_warm_done(void);

/**
 * @brief 等 SD 完全就绪（卡在位、链路正常、目录索引预热完）。**阻塞**。
 *
 * 开机序列用它把 splash 顶在前面。
 *
 * @param[in] timeout_ms 最长等多久。**传 0 = 永久等待**（不超时）—— 桌宠的素材、
 *            字库、备忘录全在卡上，卡对本产品相当于外挂 flash，"卡不在"不是一种
 *            可降级的状态，所以开机必须等到底。传非 0 则是"等这么久就放弃"，
 *            返回 FALSE（**超时不是错误**，调用方自己决定怎么办）。
 * @return TRUE 就绪；FALSE 超时（只有 timeout_ms != 0 时才可能返回 FALSE）。
 */
BOOL_T app_storage_wait_ready(uint32_t timeout_ms);

/**
 * @brief 目录索引代次。**只在卡真的换了的时候 +1**（CD 引脚报拔出 / 插入）。
 *
 * 各模块的目录扫描结果按它做失效判断：扫完记下当时的代次，下次再要扫描时先比
 * 一下 —— 一样就直接用上次的结果（见 app_preload 的后台预热）。卡换了，代次
 * 不同，自然重扫。
 *
 * **链路重建（app_storage_recover）刻意不碰它。** 重建只是把 SDIO 链路重新拉
 * 起来，卡还是那张卡、目录内容一个字节都没变 —— 让它作废缓存的话，每次插卡后
 * 紧跟的那次重建都会把预热白扫的结果再扫一遍。
 *
 * 初值 1（不是 0），"还没扫过"用 0 表示，两者一比就知道要扫。
 */
uint32_t app_storage_gen(void);

/**
 * @brief "正在长时间占用 SD"的临界区。**目录扫描必须整段包在里面。**
 *
 * 链路重建会把整个卷 unmount 掉，它必须等到没有扫描在跑 —— 否则扫描读到一半
 * 卷就没了。真机上见过：预热正在枚举 `/sdcard/nes_games`，浮层触发重建，
 * 结果只扫出 6 个游戏（正常 10 个），列表就这么少了几项。
 *
 * 所以两边互斥：扫描包 busy，`app_storage_recover()` 内部也包 busy。
 *
 * 锁是**普通互斥，不可重入**：扫描函数内部包一层就够，调用方**别再套第二层**。
 * 锁在 `app_storage_cd_init()` 里建；那之前调用是空操作（那时还没有并发者）。
 */
void app_storage_busy_enter(void);
void app_storage_busy_leave(void);

/**
 * @brief "卡还在、但连续读坏"时的恢复：**关掉自己的句柄 -> 重建链路 -> 重开**。
 *
 * 这是"读错"那条路的公共实现 —— 每个读者只要一行：
 * @code
 *   if (bad_run >= 6) {
 *       if (APP_SD_OK != app_storage_recover_reopen(&sg_f, path)) { break; }
 *       bad_run = 0; continue;
 *   }
 * @endcode
 *
 * **进去先判卡在不在**：卡不在是"掉线"，直接返回 APP_SD_ABSENT、一个字都不动，
 * 交给 CD + 路由那条路（弹"请插入 SD 卡"页）。卡在位才是"读错"，才值得重建。
 *
 * @param[in,out] f    读者的句柄。函数会**先关掉它**（`app_storage_recover()`
 *                     要求"调用前已关闭全部句柄"），重建成功后按 @a path 重开。
 *                     传 NULL 表示"没有常开句柄"（读完就关的读者）。
 * @param[in]     path 重开用的路径（@a f 为 NULL 时不用）。
 *
 * @return APP_SD_OK      重建成功、句柄已重开（或本来就没有句柄）—— 继续读
 *         APP_SD_ABSENT  卡不在位，没做任何事 —— 走掉线逻辑
 *         APP_SD_DAMAGED 卡在位但救不回来 —— 收摊，让路由弹"SD 卡损坏"
 */
APP_SD_FAULT_E app_storage_recover_reopen(TUYA_FILE *f, const char *path);

/**
 * @brief SD 能不能用 —— 等价于 `app_storage_fault() == APP_SD_OK`。
 *
 * 卡不在位、或卡在位但链路已经判死（APP_SD_DAMAGED），都算"不能用"。
 */
BOOL_T app_storage_ready(void);

/***********************************************************
******************** 卡在位检测（CD） ***********************
***********************************************************/

/**
 * @brief 初始化卡在位检测：CD 引脚轮询 + 去抖。
 *
 * 必须在 `board_register_hardware()` **之后**调用（引脚归板级外设管）。
 * 状态**变化**时发 `APP_EVT_SD_STATUS`（`data = (intptr_t)1` 插入 / `0` 拔出）
 * —— **只在变化时发一次**，不是周期性广播。
 *
 * 电平（板级实测）：**拔出 = 高 / 插入 = 低**。TF 座的 CD 触点插卡时短到 GND，
 * 引脚内部上拉，所以没卡时读到高。
 *
 * 轮询挂在 `tal_sw_timer` 上，不占线程；20ms 一次、连续 5 次同电平才算数
 * （**去抖 100ms**）。状态变化时同步更新 `app_storage_fault()`。
 *
 * @note 各模块"读错→重建"的阈值都按 200ms 取（见 `app_storage_recover_reopen`），
 *       比去抖窗口大一倍 —— 否则卡拔掉后、CD 还没确认的那 100ms 里
 *       `app_storage_card_present()` 还是真，读者会误以为"卡在、只是读坏了"
 *       而白重建一趟。
 */
OPERATE_RET app_storage_cd_init(void);

/** @brief 卡当前插着吗（CD 引脚电平，已去抖）。 */
BOOL_T app_storage_card_present(void);

/**
 * @brief 枚举目录下"真的能打开"的常规文件，逐个交给回调。
 *
 * @param[in]  dir  目录，如 "/sdcard/nes_games"
 * @param[in]  ext  后缀过滤（不区分大小写），如 ".nes"；NULL 表示不过滤
 * @param[in]  cb   命中回调
 * @param[in]  user 透传给回调
 * @return >=0 命中的文件数；<0 错误码（目录打不开等）
 */
int32_t app_storage_iterate(const char *dir, const char *ext,
                            APP_STORAGE_HIT_CB cb, void *user);

/**
 * @brief 枚举目录下的**子目录**（不含文件），逐个交给回调。
 *
 * 与 app_storage_iterate() 同一套思路：目录名同样是 FatFs 的 OEM 码页字节，
 * 转成 UTF-8 再交给 UI；路径则先试原样、再试 OEM 重编码，**用 opendir 真的
 * 打开一次**来确认哪一串能用（见 app_storage_open_dir）。
 *
 * 电影列表用它把"一个文件夹 = 一部电影"识别出来，只显示文件夹名。
 *
 * @param[in]  dir  父目录，如 "/sdcard/movie"
 * @param[in]  cb   命中回调
 * @param[in]  user 透传给回调
 * @return >=0 命中的目录数；<0 错误码（父目录打不开等）
 */
int32_t app_storage_iterate_dirs(const char *dir, APP_STORAGE_DIR_CB cb, void *user);

/**
 * @brief 由"父目录 + 目录项返回的原始名字"拼出可打开的**子目录**路径。
 *        先试原样，再试 OEM 重编码；都不行返回错误。
 */
OPERATE_RET app_storage_open_dir(const char *dir, const char *name,
                                 char *path_out, int32_t path_sz);

/** @brief 探测一个路径能否打开，可选回传大小。 */
BOOL_T app_storage_probe(const char *path, uint32_t *size_out);

/** @brief UTF-8 -> OEM(CP936) 字节，供 f_open 使用。@return 写入字节数。 */
int32_t app_storage_utf8_to_oem(const char *in, char *out, int32_t outsz);

/** @brief OEM(CP936) 字节 -> UTF-8，供 UI 显示。@return 写入字节数。 */
int32_t app_storage_oem_to_utf8(const char *in, char *out, int32_t outsz);

/**
 * @brief 由"目录 + 目录项返回的原始名字"拼出可打开的完整路径。
 *        先试原样，再试 OEM 重编码；都不行返回错误。
 */
OPERATE_RET app_storage_open(const char *dir, const char *name,
                             char *path_out, int32_t path_sz);

/** @brief 把整个文件读进调用者的缓冲（长度不匹配视为失败）。 */
OPERATE_RET app_storage_read_all(const char *path, void *dst, uint32_t expect);

#ifdef __cplusplus
}
#endif

#endif /* __APP_STORAGE_H__ */
