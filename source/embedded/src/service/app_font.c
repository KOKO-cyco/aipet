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

/** 一份从 SD 卡加载出来的字库。font/dsc 是静态的，大块指到 PSRAM。 */
typedef struct {
    lv_font_t                     font;
    lv_font_fmt_txt_dsc_t         dsc;
    lv_font_fmt_txt_cmap_t       *cmaps;      /* PSRAM：子表数组本体 */
    lv_font_fmt_txt_glyph_dsc_t  *glyphs;     /* PSRAM：字形描述数组 */
    uint8_t                      *bmp;        /* PSRAM：重排后的字形位图 */
    uint32_t                      bmp_bytes;
    uint32_t                      glyph_cnt;
    uint32_t                      cmap_num;
    uint32_t                      psram_bytes;
    int                           px;
    BOOL_T                        ok;
} APP_SDFONT_T;

/* 代理字体：地址恒定，UI 的 style 里存的就是它们的地址 */
static lv_font_t    s_proxy16;
static lv_font_t    s_proxy21;
static BOOL_T       s_proxied = FALSE;

static APP_SDFONT_T  s_f16;
static APP_SDFONT_T  s_f21;
static BOOL_T        s_loading  = FALSE;
static THREAD_HANDLE s_task_hdl = NULL;

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
    if (NULL != f->bmp) {
        tal_psram_free(f->bmp);
        f->bmp = NULL;
    }
    f->cmap_num    = 0;
    f->bmp_bytes   = 0;
    f->psram_bytes = 0;
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

    /* ---- 第一遍：展开描述符，顺便累加位图总长 ---- */
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

        g->bitmap_index = total;
        if (0 != g->box_w * g->box_h) {
            total += size;
        }
    }

    /* ---- 第二遍：把错位位流重排成连续位图 ---- */
    f->bmp = (uint8_t *)tal_psram_malloc(total);
    if (NULL == f->bmp) {
        PR_ERR("[font] 位图区分配失败（%u 字节）", (unsigned)total);
        tal_psram_free(ofs);
        return FALSE;
    }
    f->bmp_bytes   = total;
    f->psram_bytes += total;

    for (i = 1; i < loca_count; i++) {
        uint32_t next, size;

        if (0 == f->glyphs[i].box_w * f->glyphs[i].box_h) {
            continue;
        }
        next = (i + 1 < loca_count) ? ofs[i + 1] : glyph_len;
        size = (next > ofs[i] + desc_bytes) ? (next - ofs[i] - desc_bytes) : 0;

        prv_bits_to_bytes(c->blob, c->size,
                          (glyph_start + ofs[i]) * 8u + nbits,
                          f->bmp + f->glyphs[i].bitmap_index, size);
    }

    tal_psram_free(ofs);
    return TRUE;
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
    tkl_fclose(fp);

    if (got != (uint32_t)fsize) {
        PR_ERR("[font] %s 短读 %u/%u", path, (unsigned)got, (unsigned)fsize);
        tal_psram_free(blob);
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
        return OPRT_COM_ERROR;
    }
    if (!prv_seek(&cur, 8) || !prv_take(&cur, &h, sizeof(h))) {
        tal_psram_free(blob);
        return OPRT_COM_ERROR;
    }
    if (1 != h.version || 0 != h.compression_id ||
        (4 != h.bits_per_pixel && 1 != h.bits_per_pixel && 2 != h.bits_per_pixel)) {
        PR_ERR("[font] %s 不支持: version=%u bpp=%u 压缩=%u",
               path, (unsigned)h.version, (unsigned)h.bits_per_pixel,
               (unsigned)h.compression_id);
        tal_psram_free(blob);
        return OPRT_NOT_SUPPORTED;
    }

    /* ---- cmap / loca / glyf ---- */
    cmaps_start = (uint32_t)head_len;
    cmap_len    = prv_label(&cur, cmaps_start, "cmap");
    if (cmap_len < 0) {
        PR_ERR("[font] %s 缺 cmap 表", path);
        tal_psram_free(blob);
        return OPRT_COM_ERROR;
    }
    loca_start = cmaps_start + (uint32_t)cmap_len;
    loca_len   = prv_label(&cur, loca_start, "loca");
    if (loca_len < 0) {
        PR_ERR("[font] %s 缺 loca 表", path);
        tal_psram_free(blob);
        return OPRT_COM_ERROR;
    }
    glyph_start = loca_start + (uint32_t)loca_len;
    glyf_len    = prv_label(&cur, glyph_start, "glyf");
    if (glyf_len < 0) {
        PR_ERR("[font] %s 缺 glyf 表", path);
        tal_psram_free(blob);
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
        return OPRT_COM_ERROR;
    }

    /* 到这一步 blob 用完了：字形数据已经重排进 f->bmp，释放掉省一份 */
    tal_psram_free(blob);

    /* ---- 填 lv_font_t（其余字段照官方 loader 的写法） ---- */
    f->dsc.glyph_bitmap  = f->bmp;
    f->dsc.glyph_dsc     = f->glyphs;
    f->dsc.cmaps         = f->cmaps;
    f->dsc.cmap_num      = (uint16_t)f->cmap_num;
    f->dsc.kern_dsc      = NULL;
    f->dsc.kern_classes  = 0;
    f->dsc.kern_scale    = h.kerning_scale;
    f->dsc.bpp           = h.bits_per_pixel;
    f->dsc.bitmap_format = h.compression_id;

    f->font.get_glyph_dsc    = lv_font_get_glyph_dsc_fmt_txt;
    f->font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    f->font.release_glyph    = NULL;
    f->font.line_height      = (int32_t)h.ascent - (int32_t)h.descent;
    f->font.base_line        = -(int32_t)h.descent;
    f->font.subpx            = h.subpixels_mode;
    f->font.kerning          = LV_FONT_KERNING_NONE;
    f->font.underline_position  = (int8_t)h.underline_position;
    f->font.underline_thickness = (int8_t)h.underline_thickness;
    f->font.dsc             = &f->dsc;
    f->font.fallback        = NULL;
    f->font.user_data       = NULL;
    f->ok                   = TRUE;

    PR_NOTICE("[font] %s 加载完成：%upx 字形 %u 个，位图 %uKB，PSRAM 共 %uKB",
              path, (unsigned)h.font_size, (unsigned)f->glyph_cnt,
              (unsigned)(f->bmp_bytes / 1024u), (unsigned)(f->psram_bytes / 1024u));
    return OPRT_OK;
}

/*****************************************************************************
 * 对外接口
 ****************************************************************************/

const lv_font_t *app_font_cn(void)
{
    return &s_proxy16;
}

const lv_font_t *app_font_21(void)
{
    return &s_proxy21;
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

        if (app_storage_probe(APP_FONT_FILE_16, NULL) ||
            app_storage_probe(APP_FONT_FILE_21, NULL)) {
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
                APP_FONT_FILE_16, APP_FONT_FILE_21);
        s_loading = FALSE;
        return;
    }

    if (OPRT_OK != prv_load(&s_f16, APP_FONT_FILE_16)) {
        PR_WARN("[font] 16px 字库没加载上，内置子集继续生效");
    }
    if (OPRT_OK != prv_load(&s_f21, APP_FONT_FILE_21)) {
        PR_WARN("[font] 21px 字库没加载上，内置子集继续生效");
    }

    prv_attach();
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

    rt = prv_load(f, path);
    prv_attach();
    return rt;
}

BOOL_T app_font_ready(void)
{
    return (BOOL_T)(s_f16.ok && s_f21.ok);
}

void app_font_stat(void)
{
    PR_NOTICE("[font] 状态：16px %s（%uKB / %u 字形） 21px %s（%uKB / %u 字形）"
              " 合计 %uKB PSRAM，空闲 %uKB",
              s_f16.ok ? "SD" : "内置", (unsigned)(s_f16.psram_bytes / 1024u),
              (unsigned)s_f16.glyph_cnt,
              s_f21.ok ? "SD" : "内置", (unsigned)(s_f21.psram_bytes / 1024u),
              (unsigned)s_f21.glyph_cnt,
              (unsigned)((s_f16.psram_bytes + s_f21.psram_bytes) / 1024u),
              (unsigned)(tal_psram_get_free_heap_size() / 1024u));
}
