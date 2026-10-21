/**
 * @file app_font.c
 * @brief SD 卡中文字库的加载：解析 lv_font_conv 的 bin 格式，大块全放 PSRAM。
 *
 * 文件格式（照官方 lv_binfont_loader.c 反推，version 1）
 * ------------------------------------------------------
 *   每个表前面 8 个字节是 [u32 length][char[4] label]，length **含**这 8 个字节：
 *
 *     head  40 字节的 font_header_bin_t（紧贴在 8 字节前缀之后）
 *     cmap  [u32 子表数][子表目录 16 字节 * n][各子表的 list 数据]
 *     loca  [u32 字形总数][偏移数组：u16 或 u32，取决于 index_to_loc_format]
 *     glyf  每个字形 = [描述符 nbits 位][位图数据]，位图在文件里**不一定字节对齐**
 *     kern  （可选，我们不用，表里没有就直接跳过）
 *
 * 与官方 loader 的三点差异
 * ------------------------
 *   1) 官方用 lv_fs 流式读 + lv_malloc（= os_malloc，SRAM）。这里先整块读进
 *      PSRAM 再用内存游标解析，所有大块（字形位图、字形描述数组、cmap）都走
 *      tal_psram_malloc —— lv_malloc 一次都不用，SRAM 一点都不占。
 *   2) 位图数据要**重排**出来：描述符 26 位（16px）/ 28 位（21px），不是 8 的
 *      倍数，所以位图在文件里是"错开的位流"，得逐字节拼进另一块连续缓冲。
 *      官方是逐 bit 读（每字节 8 次函数调用），这里直接按字节拼，快一个量级。
 *   3) 解析完就把原始 blob 释放掉，只留重排后的位图和描述数组 —— 一份 927KB
 *      的 16px 字库最终常驻约 1MB，而不是读一遍留两份。
 *
 * 为什么必须自己做（而不是用官方 loader）见 app_font.h 的开头。
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tkl_fs.h"

#include "lvgl.h"

#include "app_font.h"
#include "app_disp.h"
#include "app_event.h"
#include "app_storage.h"

/* 固件里编译进来的手挑子集（const，在 rodata 里，改不了它的字段） */
LV_FONT_DECLARE(lv_font_SourceHanSerifSC_Regular_16)
LV_FONT_DECLARE(lv_font_SourceHanSerifSC_Regular_21)

#define APP_FONT_TASK_STACK     8192
#define APP_FONT_TASK_PRIO      THREAD_PRIO_4      /* 低于路由(3)和 LVGL(2)：不抢 UI */
#define APP_FONT_BOOT_DELAY_MS  1500               /* 让开机流程先跑完，别和它抢 SD 卡 */

/* 等卡：每 2 秒试一次，最多 75 次（2.5 分钟）。真机上见过卡 2 分 18 秒才挂上 */
#define APP_FONT_TRY_GAP_MS     2000
#define APP_FONT_TRY_MAX        75

/* 单个字库文件的上限：现在是 1.5MB，给到 8MB 防的是"拿错文件去 malloc 一大块" */
#define APP_FONT_FILE_MAX       (8u * 1024u * 1024u)

/* bin 里 head 表的布局（与 lv_binfont_loader.c 的 font_header_bin_t 一致，40 字节） */
typedef struct {
    uint32_t version;
    uint16_t tables_count;
    uint16_t font_size;
    uint16_t ascent;
    int16_t  descent;
    uint16_t typo_ascent;
    int16_t  typo_descent;
    uint16_t typo_line_gap;
    int16_t  min_y;
    int16_t  max_y;
    uint16_t default_advance_width;
    uint16_t kerning_scale;
    uint8_t  index_to_loc_format;
    uint8_t  glyph_id_format;
    uint8_t  advance_width_format;
    uint8_t  bits_per_pixel;
    uint8_t  xy_bits;
    uint8_t  wh_bits;
    uint8_t  advance_width_bits;
    uint8_t  compression_id;
    uint8_t  subpixels_mode;
    uint8_t  padding;
    int16_t  underline_position;
    uint16_t underline_thickness;
} APP_FONT_HEAD_T;

/* cmap 子表目录项（bin 里 16 字节） */
typedef struct {
    uint32_t data_offset;
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    uint16_t data_entries_count;
    uint8_t  format_type;
    uint8_t  padding;
} APP_FONT_CMAP_BIN_T;

/** 单字形位图字节上限：实测 32px 最大的一笔是 33×33 × 4bpp = 545 字节，留点余量。 */
#define APP_FONT_GLYPH_MAX   640

/**
 * @brief 字形缓存行数（**六份字库共用**）。
 *
 * 原来每份字库各 24 行，实测不够：一屏 14px 的字就有几十个，24 行会来回踢，
 * 于是每一帧都在读卡 —— `[pet] 刷新` 从 30ms 涨到 50ms 就是这么来的。
 * 改成六份共用一张 256 行的表：14px 用得最多，就该多占几行，而不是被平均分。
 * 内存 256 × 645B ≈ 165KB，比"每份 24 行 × 6"只多 70KB。
 */
#define APP_FONT_CACHE_N     256

/** 一行缓存：字形号 + 字节数 + 原始位流（**不展开**，展开成 A8 是每次画的时候
 *  现做 —— 展开很便宜，省下的是 8 倍的内存）。 */
typedef struct {
    uint8_t  font;      /* 哪份字库（s_fpx 下标 + 1）；0 = 空行 */
    uint16_t gid;
    uint16_t len;
    uint8_t  data[APP_FONT_GLYPH_MAX];
} APP_FONT_CACHE_LINE_T;

/**
 * @brief 一份从 SD 卡加载出来的字库。
 *
 * **字形位图不进 PSRAM**：GB2312 全集 × 6 个字号 ≈ 5MB，全装进来放不下，所以只把
 * "索引"（cmap + 字形描述）留在 PSRAM，位图按需从卡上读、读过的进 `cache`。
 * 描述符里的 `bitmap_index` 因此改存**文件里的绝对偏移**（不是内存偏移）。
 */
typedef struct {
    lv_font_t                     font;
    lv_font_fmt_txt_cmap_t       *cmaps;      /* PSRAM：子表数组本体 */
    lv_font_fmt_txt_glyph_dsc_t  *glyphs;     /* PSRAM：字形描述数组 */
    uint32_t                      glyph_cnt;
    uint32_t                      cmap_num;
    uint32_t                      psram_bytes;
    uint8_t                       bpp;
    uint8_t                       desc_bits;    /* 每个字形描述符占多少位（位图紧跟在后面，
                                                 * **不是字节对齐的**） */
    int                           px;
    BOOL_T                        ok;

    /* 按需读字形：文件一直开着，读的时候 seek 过去 */
    TUYA_FILE                     fp;
    char                          path[48];   /* 重开用（SD 切档/重建会作废句柄）*/
    uint8_t                       idx;        /* 在 s_fpx[] 里的下标（缓存行的键）*/
    uint32_t                      media_gen;  /* 打开/上次重开时的链路代次 */
    uint16_t                      cache_next; /* 轮转指针（简单的近似 LRU） */
    uint32_t                      last_letter;/* 最近一次查过的字，cmap 查表缓存 */
    uint32_t                      last_gid;
} APP_SDFONT_T;

/* 六份字库共用的一张字形缓存（见 APP_FONT_CACHE_N 的说明）。 */
static APP_FONT_CACHE_LINE_T *s_cache;
static uint16_t               s_cache_next;
static uint32_t               s_cache_hit;
static uint32_t               s_cache_miss;

/* 代理字体：地址恒定，UI 的 style 里存的就是它们的地址 */
static lv_font_t    s_proxy16;
static lv_font_t    s_proxy21;
static BOOL_T       s_proxied = FALSE;

static APP_SDFONT_T  s_f16;
static APP_SDFONT_T  s_f21;
static BOOL_T        s_loading  = FALSE;
static THREAD_HANDLE s_task_hdl = NULL;

/* ---- 新 UI 的六个字号（SD 卡上的 puhui<px>.bin） -------------------------
 *
 * 和 cn16/cn21 的代理套路一样，但有一处不同：cn 那份是**挂在 fallback 上**的
 * （内置子集 + SD 全字库），而 puhui 这几档要**整份替换**代理的内容 —— 它们就是
 * 主字体本身，内置的 16/21 只是"SD 还没就绪"时的占位。
 *
 * 所以这里 memcpy 的是整个 lv_font_t（含 dsc 指针，指向下面 s_fpx[] 里的静态
 * 槽位），不是 fallback 字段。 */
#define APP_FONT_PX_N   6

static const struct {
    int         px;
    const char *path;
} sg_px_tab[APP_FONT_PX_N] = {
    { 14, APP_FONT_PUHUI_14 },
    { 16, APP_FONT_PUHUI_16 },
    { 18, APP_FONT_PUHUI_18 },
    { 20, APP_FONT_PUHUI_20 },
    { 24, APP_FONT_PUHUI_24 },
    { 32, APP_FONT_PUHUI_32 },
};

static lv_font_t     s_px[APP_FONT_PX_N];      /* 代理：地址恒定，内容可换 */
static APP_SDFONT_T  s_fpx[APP_FONT_PX_N];     /* SD 字库的实体 */

/*****************************************************************************
 * bin 解析小工具（内存游标 / 位读取）
 ****************************************************************************/

typedef struct {
    const uint8_t *blob;
    uint32_t       size;
    uint32_t       pos;
} APP_FONT_CUR_T;

static BOOL_T prv_seek(APP_FONT_CUR_T *c, uint32_t pos)
{
    if (pos > c->size) {
        return FALSE;
    }
    c->pos = pos;
    return TRUE;
}

static BOOL_T prv_take(APP_FONT_CUR_T *c, void *dst, uint32_t n)
{
    if ((uint64_t)c->pos + n > (uint64_t)c->size) {
        return FALSE;
    }
    memcpy(dst, c->blob + c->pos, n);
    c->pos += n;
    return TRUE;
}

/**
 * 定位一个表：读 8 字节前缀，校验 label，返回 length（含这 8 字节）。
 * @return >=0 表长度；<0 不是要找的表 / 越界。
 */
static int32_t prv_label(APP_FONT_CUR_T *c, uint32_t start, const char *lab)
{
    uint8_t  tmp[8];
    uint32_t len;

    if (!prv_seek(c, start) || !prv_take(c, tmp, sizeof(tmp))) {
        return -1;
    }
    if (0 != memcmp(tmp + 4, lab, 4)) {
        return -1;
    }
    memcpy(&len, tmp, sizeof(len));
    return (int32_t)len;
}

typedef struct {
    const uint8_t *blob;
    uint32_t       byte;
    int            bit;
} APP_FONT_BIT_T;

static void prv_bit_at(APP_FONT_BIT_T *it, const uint8_t *blob, uint32_t pos)
{
    it->blob = blob;
    it->byte = pos;
    it->bit  = 0;
}

/** 从位流里取 n 位（每字节从 MSB 到 LSB，与官方 read_bits 同语义）。 */
static uint32_t prv_bit_get(APP_FONT_BIT_T *it, int n)
{
    uint32_t v = 0;

    for (int i = 0; i < n; i++) {
        v = (v << 1) | (uint32_t)((it->blob[it->byte] >> (7 - it->bit)) & 1u);
        if (++it->bit == 8) {
            it->bit = 0;
            it->byte++;
        }
    }
    return v;
}

static int32_t prv_bit_get_signed(APP_FONT_BIT_T *it, int n)
{
    uint32_t v = prv_bit_get(it, n);

    if (n > 0 && ((v >> (n - 1)) & 1u)) {       /* 符号扩展 */
        v = v - ((uint32_t)1u << n);
    }
    return (int32_t)v;
}

/**
 * 从 bit 流的 bitpos 处起连续取 n 个字节（MSB first）。
 *
 * 位图在 bin 里是"接在描述符后面的错位位流"：描述符 26 位意味着位图的每个字节
 * 都横跨文件的两个字节。这里一次拼一个字节，比逐 bit 读快得多，结果与官方的
 * read_bits 逐位版本逐字节相同（tools/font_bin_probe.py 在 PC 上验证过）。
 */
static void prv_bits_to_bytes(const uint8_t *src, uint32_t src_bytes, uint32_t bitpos,
                              uint8_t *dst, uint32_t n)
{
    for (uint32_t k = 0; k < n; k++) {
        uint32_t bp = bitpos + 8u * k;
        uint32_t bi = bp >> 3;
        uint32_t sh = bp & 7u;
        uint8_t  hi = src[bi];
        /* 最后一个字节可能要借到文件末尾之后那一个字节（位图的尾部 padding），
         * 这里显式挡掉 —— 直接读就越界了。 */
        uint8_t  lo = (0 == sh || bi + 1 >= src_bytes) ? 0u : src[bi + 1];

        dst[k] = (uint8_t)((uint8_t)(hi << sh) | (uint8_t)(lo >> (8 - sh)));
    }
}

/*****************************************************************************
 * 各表的解析
 ****************************************************************************/

static void prv_slot_free(APP_SDFONT_T *f)
{
    if (NULL != f->cmaps) {
        for (uint32_t i = 0; i < f->cmap_num; i++) {
            if (NULL != f->cmaps[i].unicode_list) {
                tal_psram_free((void *)f->cmaps[i].unicode_list);
            }
            if (NULL != f->cmaps[i].glyph_id_ofs_list) {
                tal_psram_free((void *)f->cmaps[i].glyph_id_ofs_list);
            }
        }
        tal_psram_free(f->cmaps);
        f->cmaps = NULL;
    }
    if (NULL != f->glyphs) {
        tal_psram_free(f->glyphs);
        f->glyphs = NULL;
    }
    if (NULL != f->fp) {
        tkl_fclose(f->fp);
        f->fp = NULL;
    }
    f->cmap_num    = 0;
    f->psram_bytes = 0;
    f->last_letter = 0;
    f->last_gid    = 0;
    f->ok          = FALSE;
}

/** 解析 cmap：子表数组 + 每个子表的 list 数据都复制到 PSRAM。 */
static BOOL_T prv_load_cmaps(APP_FONT_CUR_T *c, APP_SDFONT_T *f, uint32_t cmaps_start)
{
    uint32_t            sub;
    APP_FONT_CMAP_BIN_T dir;
    uint32_t            i;

    if (!prv_seek(c, cmaps_start + 8) || !prv_take(c, &sub, sizeof(sub))) {
        PR_ERR("[font] cmap 子表数读不到");
        return FALSE;
    }
    if (0 == sub || sub > 64) {
        PR_ERR("[font] cmap 子表数 %u 不合理", (unsigned)sub);
        return FALSE;
    }

    f->cmaps = (lv_font_fmt_txt_cmap_t *)tal_psram_malloc(sub * sizeof(lv_font_fmt_txt_cmap_t));
    if (NULL == f->cmaps) {
        PR_ERR("[font] cmap 数组分配失败（%u 项）", (unsigned)sub);
        return FALSE;
    }
    memset(f->cmaps, 0, sub * sizeof(lv_font_fmt_txt_cmap_t));
    f->cmap_num    = sub;
    f->psram_bytes += sub * (uint32_t)sizeof(lv_font_fmt_txt_cmap_t);

    /* 目录是连续的，先整段读出来（后面要按 data_offset 跳着取 list 数据） */
    for (i = 0; i < sub; i++) {
        lv_font_fmt_txt_cmap_t *cm;

        if (!prv_seek(c, cmaps_start + 8 + sizeof(sub) + i * sizeof(dir)) ||
            !prv_take(c, &dir, sizeof(dir))) {
            PR_ERR("[font] cmap 目录第 %u 项读不到", (unsigned)i);
            return FALSE;
        }

        cm = &f->cmaps[i];
        cm->range_start     = dir.range_start;
        cm->range_length    = dir.range_length;
        cm->glyph_id_start  = dir.glyph_id_start;
        cm->type            = dir.format_type;

        if (LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL == dir.format_type) {
            uint8_t *ids;

            if (!prv_seek(c, cmaps_start + dir.data_offset)) {
                return FALSE;
            }
            ids = (uint8_t *)tal_psram_malloc(dir.data_entries_count);
            if (NULL == ids) {
                PR_ERR("[font] cmap[%u] 分配 %u 字节失败",
                       (unsigned)i, (unsigned)dir.data_entries_count);
                return FALSE;
            }
            if (!prv_take(c, ids, dir.data_entries_count)) {
                tal_psram_free(ids);
                return FALSE;
            }
            cm->glyph_id_ofs_list = ids;
            cm->list_length       = dir.range_length;
            f->psram_bytes       += dir.data_entries_count;
        }
        else if (LV_FONT_FMT_TXT_CMAP_SPARSE_FULL == dir.format_type ||
                 LV_FONT_FMT_TXT_CMAP_SPARSE_TINY == dir.format_type) {
            uint16_t *uni;
            uint32_t  bytes = (uint32_t)dir.data_entries_count * 2u;

            if (!prv_seek(c, cmaps_start + dir.data_offset)) {
                return FALSE;
            }
            uni = (uint16_t *)tal_psram_malloc(bytes);
            if (NULL == uni) {
                PR_ERR("[font] cmap[%u] 分配 %u 字节失败", (unsigned)i, (unsigned)bytes);
                return FALSE;
            }
            if (!prv_take(c, uni, bytes)) {
                tal_psram_free(uni);
                return FALSE;
            }
            cm->unicode_list = uni;
            cm->list_length  = dir.data_entries_count;
            f->psram_bytes  += bytes;

            if (LV_FONT_FMT_TXT_CMAP_SPARSE_FULL == dir.format_type) {
                uint16_t *ofs;

                ofs = (uint16_t *)tal_psram_malloc(bytes);
                if (NULL == ofs) {
                    PR_ERR("[font] cmap[%u] 分配偏移表失败", (unsigned)i);
                    return FALSE;
                }
                if (!prv_take(c, ofs, bytes)) {
                    tal_psram_free(ofs);
                    return FALSE;
                }
                cm->glyph_id_ofs_list = ofs;
                f->psram_bytes       += bytes;
            }
        }
        else if (LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY != dir.format_type) {
            PR_ERR("[font] cmap[%u] 未知格式 %u", (unsigned)i, (unsigned)dir.format_type);
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * 解析 glyf：先把 7446 个字形描述符展开成数组（PSRAM），顺带算出位图总长，
 * 再分配位图区把错位位流重排进去。
 *
 * bitmap_index 用"累加的纯位图偏移"（和官方一致），所以 glyph_bitmap 必须是
 * 重排后的连续块 —— 这也是前面那段"零拷贝"思路走不通的原因（描述符不是整字节）。
 *
 * 注意 loca_count 和偏移数组都在 **loca** 表里（紧跟 8 字节前缀），不在 glyf 里。
 * 官方 loader 是 read_label(loca_start) 之后用同一个 fp 游标接着读的；第一次写
 * 的时候按 glyph_start 去读，读出来的是第一个字形的位图数据。
 */
static BOOL_T prv_load_glyf(APP_FONT_CUR_T *c, APP_SDFONT_T *f,
                            const APP_FONT_HEAD_T *h,
                            uint32_t loca_start, uint32_t glyph_start, uint32_t glyph_len)
{
    uint32_t  loca_count;
    uint32_t *ofs = NULL;
    uint32_t  nbits;
    uint32_t  desc_bytes;
    uint32_t  total = 0;
    uint32_t  i;

    if (!prv_seek(c, loca_start + 8) || !prv_take(c, &loca_count, sizeof(loca_count))) {
        PR_ERR("[font] 字形数读不到");
        return FALSE;
    }
    if (0 == loca_count || loca_count > 65535u) {
        PR_ERR("[font] 字形数 %u 不合理", (unsigned)loca_count);
        return FALSE;
    }

    /* ---- loca：字形偏移表（相对 glyph_start） ---- */
    if (!prv_seek(c, loca_start + 8 + sizeof(loca_count))) {
        return FALSE;
    }
    ofs = (uint32_t *)tal_psram_malloc((loca_count + 1) * sizeof(uint32_t));
    if (NULL == ofs) {
        PR_ERR("[font] loca 表分配失败（%u 项）", (unsigned)loca_count);
        return FALSE;
    }
    if (0 == h->index_to_loc_format) {
        for (i = 0; i < loca_count; i++) {
            uint16_t v;

            if (!prv_take(c, &v, sizeof(v))) {
                tal_psram_free(ofs);
                return FALSE;
            }
            ofs[i] = v;
        }
    }
    else if (1 == h->index_to_loc_format) {
        if (!prv_take(c, ofs, loca_count * sizeof(uint32_t))) {
            tal_psram_free(ofs);
            return FALSE;
        }
    }
    else {
        PR_ERR("[font] 未知 index_to_loc_format %u", (unsigned)h->index_to_loc_format);
        tal_psram_free(ofs);
        return FALSE;
    }

    /* 偏移表校验：必须递增、且落在 glyf 表的数据区里（前 8 字节是 "glyf" 前缀）。
     * 表坏了就别往下走 —— 后面全靠 ofs[] 定位，越界读比报错难查得多。 */
    for (i = 0; i < loca_count; i++) {
        if (ofs[i] < 8u || ofs[i] >= glyph_len ||
            (i > 0 && ofs[i] < ofs[i - 1])) {
            PR_ERR("[font] loca[%u]=%u 越界或不递增（glyf 长 %u）",
                   (unsigned)i, (unsigned)ofs[i], (unsigned)glyph_len);
            tal_psram_free(ofs);
            return FALSE;
        }
    }

    /* ---- 第一遍：展开描述符，顺便累加位图总长（只用于日志） ---- */
    nbits = (uint32_t)h->advance_width_bits + 2u * (uint32_t)h->xy_bits
            + 2u * (uint32_t)h->wh_bits;
    /* 描述符在文件里占几个**整字节**：lv_font_conv 把 [描述符][位图] 当一个连续
     * 位流写，glyph 结束时才补齐到字节边界。所以位图长度 = 相邻偏移之差 - 描述符
     * 向上取整的字节数（官方 loader 用的是向下取整，每个字形会多算一个字节，
     * 多出来的那字节是下一个字形的头部，既浪费又会在最后一个字形上读越界）。 */
    desc_bytes = (nbits + 7u) / 8u;

    f->glyphs = (lv_font_fmt_txt_glyph_dsc_t *)tal_psram_malloc(
                    loca_count * sizeof(lv_font_fmt_txt_glyph_dsc_t));
    if (NULL == f->glyphs) {
        PR_ERR("[font] 字形描述数组分配失败（%u 项）", (unsigned)loca_count);
        tal_psram_free(ofs);
        return FALSE;
    }
    memset(f->glyphs, 0, loca_count * sizeof(lv_font_fmt_txt_glyph_dsc_t));
    f->psram_bytes += loca_count * (uint32_t)sizeof(lv_font_fmt_txt_glyph_dsc_t);
    f->glyph_cnt    = loca_count;

    for (i = 0; i < loca_count; i++) {
        APP_FONT_BIT_T             it;
        lv_font_fmt_txt_glyph_dsc_t *g = &f->glyphs[i];
        uint32_t                   adv, next, size;

        prv_bit_at(&it, c->blob, glyph_start + ofs[i]);

        if (0 == h->advance_width_bits) {
            adv = h->default_advance_width;
        }
        else {
            adv = prv_bit_get(&it, h->advance_width_bits);
        }
        if (0 == h->advance_width_format) {
            adv *= 16u;                     /* 28.4 定点 */
        }
        g->adv_w = adv;
        g->ofs_x = (int16_t)prv_bit_get_signed(&it, h->xy_bits);
        g->ofs_y = (int16_t)prv_bit_get_signed(&it, h->xy_bits);
        g->box_w = (uint16_t)prv_bit_get(&it, h->wh_bits);
        g->box_h = (uint16_t)prv_bit_get(&it, h->wh_bits);

        if (0 == i) {                       /* 第 0 号是 .notdef，官方也是清零 */
            g->adv_w = 0;
            g->box_w = 0;
            g->box_h = 0;
            g->ofs_x = 0;
            g->ofs_y = 0;
        }

        next = (i + 1 < loca_count) ? ofs[i + 1] : glyph_len;
        size = (next > ofs[i] + desc_bytes) ? (next - ofs[i] - desc_bytes) : 0;

        if (0 != g->box_w * g->box_h) {
            total += size;
        }
    }

    /* ---- 第二遍：不再把位图搬进 PSRAM，只记下每个字形在文件里的位置 ----
     *
     * 以前这里会分配 total（GB2312 全集下 0.7~1.4MB）并把错位位流重排进去。
     * 现在 `bitmap_index` 存**字形在文件里的起始偏移**，画的时候按需 seek+read
     * （见 prv_glyph_bits），一份字库只多占 24 行的缓存（约 14KB）。
     *
     * 位图从描述符后面紧接着开始，而描述符是 nbits 位（实测 23 位）—— **不是
     * 字节对齐的**，所以读回来之后还得按位拼一次。 */
    f->desc_bits = (uint8_t)nbits;
    for (i = 1; i < loca_count; i++) {
        f->glyphs[i].bitmap_index = glyph_start + ofs[i];
    }

    PR_DEBUG("[font] 字形 %u 个，位图共 %uKB（留在卡上，按需读）",
             (unsigned)loca_count, (unsigned)(total / 1024u));

    tal_psram_free(ofs);
    return TRUE;
}

/*****************************************************************************
 * 字形查表 + 按需读位图
 *
 * LVGL 只要求两个回调：get_glyph_dsc（量尺寸）和 get_glyph_bitmap（拿位图）。
 * 前者查 PSRAM 里的 cmap + 描述数组，后者才去卡上读那几十~几百字节。
 ****************************************************************************/

/** 没有缓存行可用时的临时缓冲（一份字库 24 行缓存用完的情况），以及读卡的原始
 *  字节缓冲（按位拼接时 src/dst 不能是同一块）。 */
static uint8_t s_glyph_scratch[APP_FONT_GLYPH_MAX];
static uint8_t s_glyph_raw[APP_FONT_GLYPH_MAX + 1];

/** 在 unicode_list 里二分（列表升序）。返回下标，找不到 -1。 */
static int prv_ulist_find(const uint16_t *list, uint16_t n, uint16_t key)
{
    int lo = 0;
    int hi = (int)n - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;

        if (list[mid] == key) {
            return mid;
        }
        if (list[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/** 码点 -> 字形号（0 = 没有）。四种 cmap 格式的处理和 LVGL 内置的一样。 */
static uint32_t prv_gid(APP_SDFONT_T *f, uint32_t letter)
{
    if (0 == letter) {
        return 0;
    }
    if (letter == f->last_letter) {
        return f->last_gid;
    }

    for (uint32_t i = 0; i < f->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t *cm = &f->cmaps[i];
        uint32_t gid = 0;
        uint32_t rcp;

        if (letter < cm->range_start) {
            continue;
        }
        rcp = letter - cm->range_start;
        if (rcp >= cm->range_length) {
            continue;
        }

        switch (cm->type) {
        case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
            gid = cm->glyph_id_start + rcp;
            break;
        case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL:
            /* **只加表里的偏移，不要再加 rcp。** `glyph_id_ofs_list[rcp]` 里存的
             * 已经是相对 glyph_id_start 的字形号了；多加一个 rcp 会让字形整体
             * 往后偏 —— 实测「设」显示成「诞」、「今」显示成「傻」，就是这个。 */
            gid = cm->glyph_id_start + ((const uint8_t *)cm->glyph_id_ofs_list)[rcp];
            break;
        case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY: {
            int k = prv_ulist_find(cm->unicode_list, cm->list_length, (uint16_t)rcp);

            if (k >= 0) {
                gid = cm->glyph_id_start + (uint32_t)k;
            }
            break;
        }
        case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL: {
            int k = prv_ulist_find(cm->unicode_list, cm->list_length, (uint16_t)rcp);

            if (k >= 0) {
                gid = cm->glyph_id_start + ((const uint16_t *)cm->glyph_id_ofs_list)[k];
            }
            break;
        }
        default:
            break;
        }

        if (0 != gid) {
            f->last_letter = letter;
            f->last_gid    = gid;
            return gid;
        }
    }

    return 0;
}

/** 重开字库文件。
 *
 *  为什么需要：SD 卡每次切线宽（存照片时切 1 线写）、以及链路重建，都会
 *  unmount/remount，**所有已经打开的句柄一起作废**。精灵那边有同样的处理
 *  （`app_storage_recover_reopen`），字库这里也得跟上，否则存完第一张照片之后
 *  新出现的字就全是空白。 */
static BOOL_T prv_font_reopen(APP_SDFONT_T *f)
{
    if (NULL != f->fp) {
        tkl_fclose(f->fp);
        f->fp = NULL;
    }

    /* **只试一次，不在这里等、也不在这里重建链路。**
     *
     * 这个函数跑在 LVGL 线程里（画字的那一刻），等 80ms 会把整个界面卡住。
     * 链路重挂之后卡是挂着的，直接 fopen 一般就成；不成就是这一帧的字不画，
     * 下一帧再来 —— 大不了晚 16ms 出现，比卡住强。 */
    f->fp = tkl_fopen(f->path, "rb");
    if (NULL != f->fp) {
        f->media_gen = app_storage_media_gen();
    }
    return (NULL != f->fp) ? TRUE : FALSE;
}

/** 取一个字形对应的原始位流：先查缓存，没有再读卡。
 *
 *  位图在文件里紧跟在描述符后面、**不按字节对齐**（描述符 23 位），所以读的时候
 *  要从 `字形偏移 + 描述符字节数` 起多读一个字节，再按位拼成 n 个字节。 */
static const uint8_t *prv_glyph_bits(APP_SDFONT_T *f, uint32_t gid, uint32_t n)
{
    APP_FONT_CACHE_LINE_T *line = NULL;
    const lv_font_fmt_txt_glyph_dsc_t *g;
    uint8_t               *dst;
    uint32_t               byte_ofs, sh, need;
    int32_t                rd;

    if (0 == n || n > APP_FONT_GLYPH_MAX || gid >= f->glyph_cnt) {
        return NULL;
    }

    if (NULL != s_cache) {
        const uint8_t fk = (uint8_t)(f->idx + 1u);

        for (uint32_t i = 0; i < APP_FONT_CACHE_N; i++) {
            if (s_cache[i].font == fk && s_cache[i].gid == gid && s_cache[i].len == n) {
                s_cache_hit++;
                return s_cache[i].data;
            }
        }
        s_cache_miss++;
        if (0 == (s_cache_miss & 0x3Fu)) {
            PR_DEBUG("[font] 字形缓存：命中 %u 缺失 %u（%u 行）",
                     (unsigned)s_cache_hit, (unsigned)s_cache_miss,
                     (unsigned)APP_FONT_CACHE_N);
        }
        line = &s_cache[s_cache_next];
        s_cache_next = (uint16_t)((s_cache_next + 1) % APP_FONT_CACHE_N);
    }

    g        = &f->glyphs[gid];
    sh       = f->desc_bits & 7u;
    byte_ofs = g->bitmap_index + (f->desc_bits >> 3);
    need     = n + ((0 != sh) ? 1u : 0u);
    if (need > APP_FONT_GLYPH_MAX + 1u) {
        return NULL;
    }

    /* 链路重挂过（切读写档 / 重建）：句柄已作废，先重开再读。
     * 这一步让"存完照片读不到字"在下一个字上就恢复，不用等重建。 */
    if (f->media_gen != app_storage_media_gen() && !prv_font_reopen(f)) {
        return NULL;
    }

    if (0 != tkl_fseek(f->fp, (int32_t)byte_ofs, SEEK_SET)) {
        rd = -1;
    } else {
        rd = tkl_fread(s_glyph_raw, (int32_t)need, f->fp);
    }
    if ((int32_t)need != rd) {
        /* 句柄多半被重挂作废了：重开一次再试；还不行就放弃这一帧。 */
        if (!prv_font_reopen(f) || 0 != tkl_fseek(f->fp, (int32_t)byte_ofs, SEEK_SET) ||
            (int32_t)need != tkl_fread(s_glyph_raw, (int32_t)need, f->fp)) {
            PR_WARN("[font] 读字形 %u 失败（偏移 %u，%u 字节）",
                    (unsigned)gid, (unsigned)byte_ofs, (unsigned)need);
            return NULL;
        }
    }

    dst = (NULL != line) ? line->data : s_glyph_scratch;
    prv_bits_to_bytes(s_glyph_raw, need, sh, dst, n);

    if (NULL != line) {
        line->font = (uint8_t)(f->idx + 1u);
        line->gid  = (uint16_t)gid;
        line->len  = (uint16_t)n;
    }

    return dst;
}

/** 把位流按 bpp 展开成 LVGL 要的 A8。 */
static void prv_unpack(const uint8_t *src, uint32_t n, uint8_t bpp,
                       uint8_t *dst, int w, int h, uint32_t stride)
{
    const uint32_t mask = (1u << bpp) - 1u;
    uint32_t       bit  = 0;

    for (int y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * stride;

        for (int x = 0; x < w; x++, bit += bpp) {
            uint32_t byte = bit >> 3;
            uint32_t sh   = 8u - (uint32_t)bpp - (bit & 7u);
            uint32_t v    = (byte < n) ? ((src[byte] >> sh) & mask) : 0;

            row[x] = (uint8_t)(v * 255u / mask);
        }
    }
}

static bool prv_font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                                   uint32_t letter, uint32_t letter_next)
{
    APP_SDFONT_T *f = (APP_SDFONT_T *)font->dsc;
    const lv_font_fmt_txt_glyph_dsc_t *g;
    uint32_t gid;

    (void)letter_next;

    if (NULL == f || !f->ok) {
        return false;
    }
    if ('\t' == letter) {
        letter = ' ';
    }

    gid = prv_gid(f, letter);
    if (0 == gid || gid >= f->glyph_cnt) {
        return false;
    }

    g = &f->glyphs[gid];
    out->adv_w  = (uint16_t)((g->adv_w + 8u) >> 4);   /* 28.4 -> 整数 */
    out->box_w  = g->box_w;
    out->box_h  = g->box_h;
    out->ofs_x  = g->ofs_x;
    out->ofs_y  = g->ofs_y;
    out->format = LV_FONT_GLYPH_FORMAT_A8;             /* 我们自己展开成 A8 */
    out->is_placeholder = false;
    out->glyph_index    = gid;                         /* 位图回调靠它定位 */
    return true;
}

static const void *prv_font_get_glyph_bitmap(lv_font_glyph_dsc_t *g_dsc, uint32_t letter,
                                             lv_draw_buf_t *draw_buf)
{
    const lv_font_fmt_txt_glyph_dsc_t *g;
    const uint8_t *bits;
    APP_SDFONT_T  *f;
    uint32_t       n;

    (void)letter;

    if (NULL == g_dsc || NULL == g_dsc->resolved_font || NULL == draw_buf) {
        return NULL;
    }
    f = (APP_SDFONT_T *)g_dsc->resolved_font->dsc;
    if (NULL == f || g_dsc->glyph_index >= f->glyph_cnt) {
        return NULL;
    }

    g = &f->glyphs[g_dsc->glyph_index];
    n = ((uint32_t)g->box_w * (uint32_t)g->box_h * f->bpp + 7u) / 8u;
    if (0 == n) {
        return NULL;
    }

    bits = prv_glyph_bits(f, g_dsc->glyph_index, n);
    if (NULL == bits) {
        return NULL;
    }

    /* 行距用 **LVGL 给的这个缓冲自己的 stride**：它是按 LV_STRIDE_AUTO 建的，
     * 不一定等于"宽度 × 1 字节"，自己算一个就会写歪。 */
    prv_unpack(bits, n, f->bpp, (uint8_t *)draw_buf->data,
               (int)g->box_w, (int)g->box_h, draw_buf->header.stride);

    /* **返回 draw_buf 本身，不是 draw_buf->data。** 调用方
     * （lv_draw_sw_letter.c 的 draw_letter_cb）拿它当 `lv_draw_buf_t *`，
     * 自己再取 `->data` 和 `->header.stride`；返回 data 的话它会去解引用那块
     * 像素内存，直接踩到野地址（实测 MemFault @0x44bb）。 */
    return draw_buf;
}

/** 读整个文件 -> 解析 -> 释放 blob。成功时 f->ok 置位。 */
static OPERATE_RET prv_load(APP_SDFONT_T *f, const char *path)
{
    APP_FONT_CUR_T    cur;
    APP_FONT_HEAD_T   h;
    TUYA_FILE         fp;
    int64_t           fsize;
    uint8_t          *blob;
    int32_t           head_len, cmap_len, loca_len, glyf_len;
    uint32_t          cmaps_start, loca_start, glyph_start;
    uint32_t          left, got;

    fp = tkl_fopen(path, "rb");
    if (NULL == fp) {
        PR_WARN("[font] 打不开 %s", path);
        return OPRT_NOT_FOUND;
    }
    if (0 != tkl_fseek(fp, 0, SEEK_END)) {
        tkl_fclose(fp);
        return OPRT_COM_ERROR;
    }
    fsize = tkl_ftell(fp);
    if (fsize <= 0 || (uint64_t)fsize > APP_FONT_FILE_MAX) {
        PR_ERR("[font] %s 大小异常: %d", path, (int)fsize);
        tkl_fclose(fp);
        return OPRT_INVALID_PARM;
    }
    if (0 != tkl_fseek(fp, 0, SEEK_SET)) {
        tkl_fclose(fp);
        return OPRT_COM_ERROR;
    }

    blob = (uint8_t *)tal_psram_malloc((uint32_t)fsize);
    if (NULL == blob) {
        PR_ERR("[font] 读 %s 需要 %uKB，PSRAM 不够", path, (unsigned)(fsize / 1024));
        tkl_fclose(fp);
        return OPRT_MALLOC_FAILED;
    }

    left = (uint32_t)fsize;
    got  = 0;
    while (got < (uint32_t)fsize) {
        int32_t rd = tkl_fread(blob + got, (int32_t)(left), fp);

        if (rd <= 0) {
            break;
        }
        got  += (uint32_t)rd;
        left -= (uint32_t)rd;
    }

    if (got != (uint32_t)fsize) {
        PR_ERR("[font] %s 短读 %u/%u", path, (unsigned)got, (unsigned)fsize);
        tal_psram_free(blob);
        tkl_fclose(fp);
        return OPRT_COM_ERROR;
    }

    cur.blob = blob;
    cur.size = (uint32_t)fsize;
    cur.pos  = 0;

    /* ---- head ---- */
    head_len = prv_label(&cur, 0, "head");
    if (head_len < (int32_t)sizeof(h)) {
        PR_ERR("[font] %s 不是 lv_font_conv 的 bin（head 表不对）", path);
        tal_psram_free(blob);
        tkl_fclose(fp);
        return OPRT_COM_ERROR;
    }
    if (!prv_seek(&cur, 8) || !prv_take(&cur, &h, sizeof(h))) {
        tal_psram_free(blob);
        tkl_fclose(fp);
        return OPRT_COM_ERROR;
    }
    if (1 != h.version || 0 != h.compression_id ||
        (4 != h.bits_per_pixel && 1 != h.bits_per_pixel && 2 != h.bits_per_pixel)) {
        PR_ERR("[font] %s 不支持: version=%u bpp=%u 压缩=%u",
               path, (unsigned)h.version, (unsigned)h.bits_per_pixel,
               (unsigned)h.compression_id);
        tal_psram_free(blob);
        tkl_fclose(fp);
        return OPRT_NOT_SUPPORTED;
    }

    /* ---- cmap / loca / glyf ---- */
    cmaps_start = (uint32_t)head_len;
    cmap_len    = prv_label(&cur, cmaps_start, "cmap");
    if (cmap_len < 0) {
        PR_ERR("[font] %s 缺 cmap 表", path);
        tal_psram_free(blob);
        tkl_fclose(fp);
        return OPRT_COM_ERROR;
    }
    loca_start = cmaps_start + (uint32_t)cmap_len;
    loca_len   = prv_label(&cur, loca_start, "loca");
    if (loca_len < 0) {
        PR_ERR("[font] %s 缺 loca 表", path);
        tal_psram_free(blob);
        tkl_fclose(fp);
        return OPRT_COM_ERROR;
    }
    glyph_start = loca_start + (uint32_t)loca_len;
    glyf_len    = prv_label(&cur, glyph_start, "glyf");
    if (glyf_len < 0) {
        PR_ERR("[font] %s 缺 glyf 表", path);
        tal_psram_free(blob);
        tkl_fclose(fp);
        return OPRT_COM_ERROR;
    }

    PR_DEBUG("[font] %s 表：head=%u cmap=%u loca=%u glyf=%u，loca格式=%u bpp=%u",
             path, (unsigned)head_len, (unsigned)cmap_len,
             (unsigned)loca_len, (unsigned)glyf_len,
             (unsigned)h.index_to_loc_format, (unsigned)h.bits_per_pixel);

    if (!prv_load_cmaps(&cur, f, cmaps_start) ||
        !prv_load_glyf(&cur, f, &h, loca_start, glyph_start, (uint32_t)glyf_len)) {
        PR_ERR("[font] %s 解析失败", path);
        prv_slot_free(f);
        tal_psram_free(blob);
        tkl_fclose(fp);
        return OPRT_COM_ERROR;
    }

    /* 解析用完了，但**文件不关** —— 字形位图要按需回来读（prv_glyph_bits）。
     * 路径也记下来：SD 重挂之后要靠它把句柄重开。 */
    tal_psram_free(blob);
    f->fp = fp;
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->media_gen = app_storage_media_gen();

    if (NULL == s_cache) {
        s_cache = (APP_FONT_CACHE_LINE_T *)tal_psram_malloc(
                      APP_FONT_CACHE_N * sizeof(APP_FONT_CACHE_LINE_T));
        if (NULL == s_cache) {
            PR_WARN("[font] 字形缓存分配失败：能显示，但每画一次读一次卡");
        } else {
            for (uint32_t k = 0; k < APP_FONT_CACHE_N; k++) {
                s_cache[k].font = 0;            /* 0 = 空行 */
                s_cache[k].gid  = 0xFFFFu;
                s_cache[k].len  = 0;
            }
            s_cache_next = 0;
            s_cache_hit  = 0;
            s_cache_miss = 0;
        }
    }
    f->cache_next  = 0;
    f->bpp         = h.bits_per_pixel;
    f->last_letter = 0;
    f->last_gid    = 0;

    /* ---- 填 lv_font_t（其余字段照官方 loader 的写法） ---- */
    f->font.get_glyph_dsc    = prv_font_get_glyph_dsc;
    f->font.get_glyph_bitmap = prv_font_get_glyph_bitmap;
    f->font.release_glyph    = NULL;
    f->font.line_height      = (int32_t)h.ascent - (int32_t)h.descent;
    f->font.base_line        = -(int32_t)h.descent;
    f->font.subpx            = h.subpixels_mode;
    f->font.kerning          = LV_FONT_KERNING_NONE;
    f->font.underline_position  = (int8_t)h.underline_position;
    f->font.underline_thickness = (int8_t)h.underline_thickness;
    f->font.dsc              = f;           /* 回调里靠它找回 APP_SDFONT_T */
    f->font.fallback         = NULL;
    f->font.user_data        = NULL;
    f->ok                    = TRUE;

    PR_NOTICE("[font] %s 加载完成：%upx 字形 %u 个（位图按需读），索引 %uKB",
              path, (unsigned)h.font_size, (unsigned)f->glyph_cnt,
              (unsigned)(f->psram_bytes / 1024u));
    return OPRT_OK;
}

/*****************************************************************************
 * 对外接口
 ****************************************************************************/

const lv_font_t *app_font_cn(void)
{
    /* 旧接口保留：现在全产品的正文就是新 UI 的 14px。cn16/cn21 不再加载
     * （省 2.4MB PSRAM）—— 需要它们时各自用 app_font_load_sync() 单独加载。 */
    return app_font_puhui(14);
}

const lv_font_t *app_font_21(void)
{
    return app_font_puhui(20);
}

/** 字号表里的下标；px 不精确匹配时取最接近的一档（永远有结果）。 */
static int prv_px_index(int px)
{
    int best = 0;
    int diff = 0x7fffffff;

    for (int i = 0; i < APP_FONT_PX_N; i++) {
        int d = sg_px_tab[i].px - px;

        if (d < 0) {
            d = -d;
        }
        if (d < diff) {
            diff = d;
            best = i;
        }
    }
    return best;
}

/** 这一档的兜底字体（内置 16 / 21px 子集）。 */
static const lv_font_t *prv_px_fallback(int i)
{
    return (sg_px_tab[i].px >= 20) ? &lv_font_SourceHanSerifSC_Regular_21
                                   : &lv_font_SourceHanSerifSC_Regular_16;
}

/** 字库里有没有这个码点（只查 cmap，不碰位图）。 */
static BOOL_T prv_has_cp(const APP_SDFONT_T *f, uint32_t cp)
{
    for (uint32_t i = 0; i < f->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t *cm = &f->cmaps[i];
        uint32_t                      ofs;

        if (LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY == cm->type ||
            LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL == cm->type) {
            if (cp >= cm->range_start &&
                cp < (uint32_t)cm->range_start + (uint32_t)cm->range_length) {
                return TRUE;
            }
            continue;
        }

        if (NULL == cm->unicode_list || cp < cm->range_start) {
            continue;
        }
        ofs = cp - cm->range_start;
        for (uint32_t k = 0; k < cm->list_length; k++) {
            if (cm->unicode_list[k] == ofs) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

const lv_font_t *app_font_puhui(int px)
{
    return &s_px[prv_px_index(px)];
}

OPERATE_RET app_font_init(void)
{
    if (s_proxied) {
        return OPRT_OK;
    }

    /* 代理 = 内置字体的一份 RAM 副本。UI 存进 style 的是代理的地址，而这个地址
     * 永远不变，所以后面只要改它的 fallback，所有界面立刻生效。 */
    memcpy(&s_proxy16, &lv_font_SourceHanSerifSC_Regular_16, sizeof(s_proxy16));
    memcpy(&s_proxy21, &lv_font_SourceHanSerifSC_Regular_21, sizeof(s_proxy21));
    s_proxy16.fallback = NULL;
    s_proxy21.fallback = NULL;

    s_f16.px = 16;
    s_f21.px = 21;
    s_proxied = TRUE;

    /* 新 UI 的六档：先全部装成内置字体，SD 字库到了再原地换掉。
     * 14/16/18 用内置 16px（行高接近），20/24/32 用内置 21px。 */
    for (int i = 0; i < APP_FONT_PX_N; i++) {
        const lv_font_t *base = (sg_px_tab[i].px >= 20)
                                ? &lv_font_SourceHanSerifSC_Regular_21
                                : &lv_font_SourceHanSerifSC_Regular_16;

        memcpy(&s_px[i], base, sizeof(lv_font_t));
        s_px[i].fallback = NULL;
        s_fpx[i].px      = sg_px_tab[i].px;
    }

    PR_NOTICE("[font] 代理字体就绪（内置子集：%upx / %upx），等待 SD 字库",
              (unsigned)lv_font_SourceHanSerifSC_Regular_16.line_height,
              (unsigned)lv_font_SourceHanSerifSC_Regular_21.line_height);
    return OPRT_OK;
}

/**
 * 把加载好的字库挂到代理的 fallback 上，并让当前界面重绘。
 *
 * 必须拿显示锁：改 fallback 与 LVGL 正在遍历字体是同一份数据；invalidate 是为了
 * 让"当前屏上已经画出来的空白字"有机会用新字库重画一遍 —— 不 invalidate 的话
 * LVGL 认为屏幕没变化，要等到下一次别的原因重绘才看得到效果。
 */
static void prv_attach(void)
{
    if (!s_proxied) {
        return;
    }

    app_disp_lock();
    if (s_f16.ok) {
        s_proxy16.fallback = &s_f16.font;
    }
    if (s_f21.ok) {
        s_proxy21.fallback = &s_f21.font;
    }
    if (s_f16.ok || s_f21.ok) {
        lv_obj_invalidate(lv_screen_active());
    }
    app_disp_unlock();
}

/**
 * 等 SD 卡可用：返回 TRUE 表示字库文件已经能 fopen。
 *
 * 两个坑（真机踩出来的，别再踩）：
 *  1) `app_storage_ready()` 只是"有没有人调过 app_storage_init()"的标志位，它自己
 *     不挂卡。挂载是被别的模块**顺手**触发的（音乐扫描 / 电影页 / NES 扫描），
 *     没人触发就一直不挂 —— 实测见过开机 2 分 18 秒才被挂上，而等 30 秒放弃的
 *     字库任务早就退了。所以这里必须自己调 `app_storage_init()`（幂等）。
 *  2) `app_storage_init()` 只要调过就把标志置 TRUE，`tkl_fs_mount` 失败也一样，
 *     所以 ready() 不能当判据 —— 真正的判据是"文件能不能 fopen"。
 */
static BOOL_T prv_wait_card(void)
{
    for (uint32_t i = 0; i < APP_FONT_TRY_MAX; i++) {
        (void)app_storage_init();

        if (app_storage_probe(APP_FONT_PUHUI_18, NULL) ||
            app_storage_probe(APP_FONT_PUHUI_14, NULL)) {
            if (0 != i) {
                PR_NOTICE("[font] SD 卡在第 %u 次探测后可用（等了约 %u 秒）",
                          (unsigned)(i + 1), (unsigned)(i * APP_FONT_TRY_GAP_MS / 1000u));
            }
            return TRUE;
        }
        tal_system_sleep(APP_FONT_TRY_GAP_MS);
    }
    return FALSE;
}

static void prv_font_task(void *arg)
{
    (void)arg;

    /* 等开机流程跑完（首页已经建好），再动 SD 卡 */
    tal_system_sleep(APP_FONT_BOOT_DELAY_MS);

    if (!prv_wait_card()) {
        PR_WARN("[font] SD 卡上没有 %s / %s，沿用固件内置字库",
                APP_FONT_PUHUI_14, APP_FONT_PUHUI_18);
        s_loading = FALSE;
        return;
    }

    /* 新 UI 的六个字号。**每一档单独成败**：缺哪份就留内置占位，界面照样起。
     * 换代理要拿显示锁 —— 换的这一刻 LVGL 可能正在用这个字体画字。
     *
     * cn16 / cn21 不再加载：全产品的中文都走 puhui14 / puhui18 了，那两份
     * 完整字库是 2.4MB PSRAM，白占。要它们的时候用 app_font_load_sync()。 */
    {
        int loaded = 0;

        for (int i = 0; i < APP_FONT_PX_N; i++) {
            s_fpx[i].idx = (uint8_t)i;      /* 字形缓存用它区分是哪份字库 */
            if (OPRT_OK != prv_load(&s_fpx[i], sg_px_tab[i].path)) {
                PR_WARN("[font] %s 没加载上，%dpx 用内置占位",
                        sg_px_tab[i].path, sg_px_tab[i].px);
                continue;
            }

            app_disp_lock();
            memcpy(&s_px[i], &s_fpx[i].font, sizeof(lv_font_t));
            /* 兜底：字库缺的字（最常见的是生成时没带 ASCII 的子集字号）回退到
             * 内置子集，而不是画一个空白。完整的字库永远走不到这一层。 */
            s_px[i].fallback = prv_px_fallback(i);
            app_disp_unlock();

            if (!prv_has_cp(&s_fpx[i], '0')) {
                PR_WARN("[font] %s 没有 ASCII 数字（顶栏时间/电量会回退内置字体）。"
                        "生成时请带上 0x20-0x7e", sg_px_tab[i].path);
            }
            loaded++;
        }

        if (loaded > 0) {
            /* 已经画在屏上的字是内置字体的形状，得让它们用新字库重画一遍。 */
            app_disp_lock();
            lv_obj_invalidate(lv_screen_active());
            app_disp_unlock();

            /* 换字体不只是重画：已经按内置字体量过尺寸的标签（标题、状态栏、
             * 行内省略号）得重新排一次。页面订阅这个事件，在 LVGL 线程里重建
             * 当前页 —— 这里只发事件，不碰界面。 */
            app_event_publish(APP_EVT_FONT_READY, NULL);
        }
        PR_NOTICE("[font] puhui 字号加载 %d/%d 档", loaded, APP_FONT_PX_N);
    }

    app_font_stat();
    s_loading = FALSE;

    /* 一次性任务，干完自退 —— 和 app_speaker / movie_audio 一个套路：
     * tal_thread_delete 只是打一个异步标记，所以放在最后一行。 */
    tal_thread_delete(s_task_hdl);
    s_task_hdl = NULL;
}

OPERATE_RET app_font_load_async(void)
{
    THREAD_CFG_T cfg = {0};

    if (!s_proxied) {
        return OPRT_COM_ERROR;
    }
    if (s_loading) {
        return OPRT_OK;
    }
    s_loading = TRUE;

    cfg.stackDepth = APP_FONT_TASK_STACK;
    cfg.priority   = APP_FONT_TASK_PRIO;
    cfg.thrdname   = "app_font";

    if (OPRT_OK != tal_thread_create_and_start(&s_task_hdl, NULL, NULL,
                                               prv_font_task, NULL, &cfg)) {
        PR_ERR("[font] 后台任务创建失败");
        s_loading = FALSE;
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}

OPERATE_RET app_font_load_sync(int px, const char *path)
{
    APP_SDFONT_T *f  = (16 == px) ? &s_f16 : &s_f21;
    OPERATE_RET   rt;

    if (!s_proxied) {
        return OPRT_COM_ERROR;
    }
    if (NULL == path) {
        path = (16 == px) ? APP_FONT_FILE_16 : APP_FONT_FILE_21;
    }

    f->idx = (16 == px) ? 6 : 7;    /* 字形缓存的键：别和 s_fpx[] 的 0..5 撞 */
    rt = prv_load(f, path);
    prv_attach();
    return rt;
}

BOOL_T app_font_ready(void)
{
    /* 正文和工作字号在位就算就绪（其它几档缺了有内置占位）。 */
    return (BOOL_T)(s_fpx[prv_px_index(14)].ok && s_fpx[prv_px_index(18)].ok);
}

void app_font_stat(void)
{
    uint32_t px_bytes = 0;
    int      loaded   = 0;

    for (int i = 0; i < APP_FONT_PX_N; i++) {
        px_bytes += s_fpx[i].psram_bytes;
        if (s_fpx[i].ok) {
            loaded++;
        }
    }

    PR_NOTICE("[font] 状态：puhui %d/%d 档，%uKB PSRAM，空闲 %uKB",
              loaded, APP_FONT_PX_N,
              (unsigned)(px_bytes / 1024u),
              (unsigned)(tal_psram_get_free_heap_size() / 1024u));
}
