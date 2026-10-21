/**
 * @file app_spf6.c
 * @brief `.spf` 帧序列容器的共享读取器。格式与设计取舍见 app_spf6.h。
 *
 * 这个文件里的解析代码原本长在 `app_sprite2d.c` 里（SPF4/SPF6 时期），抽出来是为了
 * 让角色页（app_role2d.c）读同一个格式时不用抄第二份。**逻辑一行没改** —— 只是把
 * 原来读写 `sg_data` 的地方换成了入参 `pf`。
 */

#include <string.h>

#include "tal_api.h"
#include "tkl_fs.h"

#include "app_spf6.h"
#include "app_storage.h"                /* app_storage_busy_enter：与写卡互斥 */

#define SPF6_MAGIC      "SPF7"
#define SPF6_HDR_LEN    12u     /**< magic + nc + rsv + gw + gh */
#define SPF6_REC_LEN    14u     /**< 一组记录：frames fps w h dx dy role */

static inline uint16_t prv_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t prv_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t spf6_rle_worst(uint32_t npx)
{
    /* 最坏情况：整帧都是字面量 —— 每 128 个像素一个控制字节 + 2 字节/像素。
     * 末尾多加 8 字节余量，免得"刚好是 128 的整数倍"时差一个控制字节。 */
    return npx * 2u + (npx + 127u) / 128u + 8u;
}

uint32_t spf6_spans_bytes(uint16_t max_h)
{
    return (uint32_t)max_h * (uint32_t)sizeof(sp2d_span_t);
}

void spf6_close(SPF6_FILE_T *pf)
{
    if (NULL == pf) {
        return;
    }
    if (NULL != pf->f) {
        tkl_fclose(pf->f);
        pf->f = NULL;
    }
    if (NULL != pf->clips) {
        tal_free(pf->clips);
        pf->clips = NULL;
    }
    if (NULL != pf->offs) {
        tal_free(pf->offs);
        pf->offs = NULL;
    }
    pf->clip_count   = 0;
    pf->total_frames = 0;
    pf->max_w        = 0;
    pf->max_h        = 0;
    pf->gw           = 0;
    pf->gh           = 0;
    pf->data_off     = 0;
}

BOOL_T spf6_read_at(SPF6_FILE_T *pf, uint32_t off, uint32_t len, void *buf)
{
    BOOL_T ok;

    if (NULL == pf || NULL == pf->f) {
        return FALSE;
    }

    /* SD 只有一组数据线，读和写**物理上不能同时进行**。这里是全部精灵读卡
     * 的唯一入口（预取线程每批 40~64KB 都走它，实测 1~2.7MB/s），所以临界区
     * 放在这一层最省事：拍照落盘时它会让路，落盘就不会被读到一半的块打断。
     * 2026-10-20：照片写卡成批失败（`write data error, wr_status = 0x7`）
     * 正是叠在预取读卡上发生的。锁不可重入，本函数内部不再套第二层。 */
    app_storage_busy_enter();

    /* 链路被重挂过（切读写档 / 重建）：句柄已经作废，先按路径重开再读。
     * 不做这一步的话，重挂之后这一批必然读坏，攒够 6 批就去重建链路 ——
     * 而重建又会作废别人的句柄，两个读者互相踢成一场风暴。 */
    if (pf->media_gen != app_storage_media_gen()) {
        if (NULL != pf->f) {
            tkl_fclose(pf->f);
            pf->f = NULL;
        }
        pf->f = tkl_fopen(pf->path, "rb");
        if (NULL == pf->f) {
            app_storage_busy_leave();
            return FALSE;
        }
        pf->media_gen = app_storage_media_gen();
    }

    if (tkl_fseek(pf->f, (int64_t)off, SEEK_SET) != 0) {
        app_storage_busy_leave();
        return FALSE;
    }

    ok = ((int32_t)len == tkl_fread(buf, (int32_t)len, pf->f)) ? TRUE : FALSE;

    app_storage_busy_leave();

    return ok;
}

int spf6_frame_span(const SPF6_FILE_T *pf, uint32_t idx, uint32_t *off, uint32_t *len)
{
    uint32_t a, b;

    if (NULL == pf || NULL == pf->offs || idx >= pf->total_frames) {
        return -1;
    }

    a = pf->offs[idx];
    b = pf->offs[idx + 1u];
    if (b <= a) {
        return -1;
    }
    if (NULL != off) { *off = a; }
    if (NULL != len) { *len = b - a; }
    return 0;
}

const sp2d_clip_t *spf6_clip_of(const SPF6_FILE_T *pf, uint32_t idx)
{
    if (NULL == pf || NULL == pf->clips || idx >= pf->clip_count) {
        return NULL;
    }
    return &pf->clips[idx];
}

int spf6_clip_by_name(const SPF6_FILE_T *pf, const char *name)
{
    if (NULL == pf || NULL == pf->clips || NULL == name || '\0' == name[0]) {
        return -1;
    }
    for (uint32_t i = 0; i < pf->clip_count; i++) {
        if (0 == strcmp(pf->clips[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

uint32_t spf6_inflate(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t count)
{
    uint32_t si = 0, di = 0;

    while (si < len && di < count) {
        uint8_t c = src[si++];

        if (c < 128u) {                             /* 字面量 */
            uint32_t n = (uint32_t)c + 1u;

            if (si + n * 2u > len) {
                break;
            }
            if (di + n > count) {
                n = count - di;
            }
            for (uint32_t i = 0; i < n; i++) {
                dst[di++] = (uint16_t)((uint16_t)src[si] |
                                       ((uint16_t)src[si + 1] << 8));
                si += 2;
            }
        } else if (c > 128u) {                      /* 行程 */
            uint32_t n;
            uint16_t v;

            if (si + 2u > len) {
                break;
            }
            v  = (uint16_t)((uint16_t)src[si] | ((uint16_t)src[si + 1] << 8));
            si += 2;
            n  = 257u - (uint32_t)c;
            if (di + n > count) {
                n = count - di;
            }
            while (n--) {
                dst[di++] = v;
            }
        }
        /* c == 128：编码端不产生，跳过（1 字节控制字，无载荷） */
    }
    return di;
}

void spf6_build_spans(const uint16_t *px, sp2d_span_t *spans,
                      uint32_t count, int w, int h)
{
    const uint32_t fpx = (uint32_t)w * (uint32_t)h;

    for (uint32_t i = 0; i < count; i++) {
        const uint16_t *f = px + (size_t)i * fpx;
        sp2d_span_t *sp = spans + (size_t)i * h;

        for (int y = 0; y < h; y++) {
            const uint16_t *row = f + (size_t)y * w;
            uint16_t x0 = 0, x1 = 0;

            for (int x = 0; x < w; x++) {
                if (row[x] & 0xF000u) { x0 = (uint16_t)x; break; }
            }
            for (int x = w; x > (int)x0; x--) {
                if (row[x - 1] & 0xF000u) { x1 = (uint16_t)x; break; }
            }
            sp[y].x0 = x0;
            sp[y].x1 = x1;
        }
    }
}

OPERATE_RET spf6_open(SPF6_FILE_T *pf, const char *path, uint16_t max_w, uint16_t max_h)
{
    uint8_t  hdr[SPF6_HDR_LEN];
    uint16_t nc;
    uint32_t total = 0;
    uint32_t maxw = 0, maxh = 0;

    if (NULL == pf || NULL == path) {
        return OPRT_INVALID_PARM;
    }
    memset(pf, 0, sizeof(*pf));

    pf->f = tkl_fopen(path, "rb");
    if (NULL == pf->f) {
        PR_WARN("[spf6] 打不开 %s", path);
        return OPRT_NOT_FOUND;
    }
    snprintf(pf->path, sizeof(pf->path), "%s", path);
    pf->media_gen = app_storage_media_gen();

    /* 之后任何一条失败路径都走 spf6_close(pf) —— 它把句柄和表一起收干净，
     * 所以调用方拿到的永远是"要么全好、要么全空"。 */
    if ((int32_t)SPF6_HDR_LEN != tkl_fread(hdr, (int32_t)SPF6_HDR_LEN, pf->f)) {
        PR_ERR("[spf6] %s 头部读不出来", path);
        spf6_close(pf);
        return OPRT_INVALID_PARM;
    }

    if (0 != memcmp(hdr, SPF6_MAGIC, 4)) {
        PR_ERR("[spf6] %s 不是 %s 容器（头 4 字节 %02X %02X %02X %02X）—— "
               "固件已改用 PackBits 压缩帧 + 名字表，请用 tools/pack_seq.py "
               "重新生成并拷到 SD 卡",
               path, SPF6_MAGIC, hdr[0], hdr[1], hdr[2], hdr[3]);
        spf6_close(pf);
        return OPRT_INVALID_PARM;
    }

    nc = prv_rd16(hdr + 4);
    pf->gw = prv_rd16(hdr + 8);
    pf->gh = prv_rd16(hdr + 10);

    if (0 == nc || nc > SPF6_MAX_CLIPS || 0 == pf->gw || 0 == pf->gh) {
        PR_ERR("[spf6] %s 参数越界：%u 组，锚点框 %ux%u（组数上限 %u）",
               path, nc, pf->gw, pf->gh, SPF6_MAX_CLIPS);
        spf6_close(pf);
        return OPRT_INVALID_PARM;
    }

    pf->clips = (sp2d_clip_t *)tal_malloc((size_t)nc * sizeof(sp2d_clip_t));
    if (NULL == pf->clips) {
        spf6_close(pf);
        return OPRT_MALLOC_FAILED;
    }
    memset(pf->clips, 0, (size_t)nc * sizeof(sp2d_clip_t));
    pf->clip_count = nc;

    for (uint32_t i = 0; i < nc; i++) {
        uint8_t ch[SPF6_REC_LEN];

        if ((int32_t)SPF6_REC_LEN != tkl_fread(ch, (int32_t)SPF6_REC_LEN, pf->f)) {
            PR_ERR("[spf6] %s 动作表读短了（第 %u 组）", path, (unsigned)i);
            spf6_close(pf);
            return OPRT_COM_ERROR;
        }
        pf->clips[i].frames = prv_rd16(ch);
        pf->clips[i].fps    = prv_rd16(ch + 2);
        pf->clips[i].first  = total;
        pf->clips[i].w      = prv_rd16(ch + 4);
        pf->clips[i].h      = prv_rd16(ch + 6);
        pf->clips[i].dx     = (int16_t)prv_rd16(ch + 8);
        pf->clips[i].dy     = (int16_t)prv_rd16(ch + 10);
        pf->clips[i].role   = prv_rd16(ch + 12);

        if (0 == pf->clips[i].frames || 0 == pf->clips[i].w || 0 == pf->clips[i].h) {
            PR_ERR("[spf6] %s 第 %u 组参数为 0（%u 帧 %ux%u）", path, (unsigned)i,
                   pf->clips[i].frames, pf->clips[i].w, pf->clips[i].h);
            spf6_close(pf);
            return OPRT_INVALID_PARM;
        }
        if (pf->clips[i].w > max_w || pf->clips[i].h > max_h) {
            PR_ERR("[spf6] %s 第 %u 组帧 %ux%u 超过本模块上限 %ux%u",
                   path, (unsigned)i, pf->clips[i].w, pf->clips[i].h, max_w, max_h);
            spf6_close(pf);
            return OPRT_INVALID_PARM;
        }
        if (pf->clips[i].w > maxw) { maxw = pf->clips[i].w; }
        if (pf->clips[i].h > maxh) { maxh = pf->clips[i].h; }
        total += pf->clips[i].frames;
    }
    pf->max_w = (uint16_t)maxw;
    pf->max_h = (uint16_t)maxh;

    /* ---- 名字表：nc x {u8 len, ASCII}，整体补 0 到 4 字节对齐 ----
     *
     * 补对齐的基准是**整个头部**（12 + nc*14 + 名字区），因为打包器就是按
     * `len(buf) % 4` 补的 —— 这里必须用同一个基准，否则偏移表会读错位置。 */
    {
        uint32_t used = SPF6_HDR_LEN + (uint32_t)nc * SPF6_REC_LEN;
        uint8_t  buf[SPF6_NAME_MAX];

        for (uint32_t i = 0; i < nc; i++) {
            uint8_t ln;

            if (1 != tkl_fread(&ln, 1, pf->f)) {
                PR_ERR("[spf6] %s 名字表读短了（第 %u 组长度）", path, (unsigned)i);
                spf6_close(pf);
                return OPRT_COM_ERROR;
            }
            if (0 == ln || ln >= SPF6_NAME_MAX) {
                PR_ERR("[spf6] %s 第 %u 组名字长度 %u 越界（1..%d）",
                       path, (unsigned)i, ln, SPF6_NAME_MAX - 1);
                spf6_close(pf);
                return OPRT_INVALID_PARM;
            }
            if ((int32_t)ln != tkl_fread(buf, (int32_t)ln, pf->f)) {
                PR_ERR("[spf6] %s 第 %u 组名字读短了", path, (unsigned)i);
                spf6_close(pf);
                return OPRT_COM_ERROR;
            }
            memcpy(pf->clips[i].name, buf, ln);
            pf->clips[i].name[ln] = '\0';
            used += 1u + (uint32_t)ln;
        }

        uint32_t pad = (4u - (used & 3u)) & 3u;

        if (pad > 0u) {
            uint8_t skip[3];

            if ((int32_t)pad != tkl_fread(skip, (int32_t)pad, pf->f)) {
                PR_ERR("[spf6] %s 名字表的对齐填充读不出来", path);
                spf6_close(pf);
                return OPRT_COM_ERROR;
            }
        }
    }

    /* ---- 偏移表：total+1 项，最后一项 = 数据区结束 ---- */
    pf->total_frames = total;
    pf->offs = (uint32_t *)tal_malloc((size_t)(total + 1u) * sizeof(uint32_t));
    if (NULL == pf->offs) {
        spf6_close(pf);
        return OPRT_MALLOC_FAILED;
    }

    for (uint32_t i = 0; i <= total; i++) {
        uint8_t b[4];

        if (4 != tkl_fread(b, 4, pf->f)) {
            PR_ERR("[spf6] %s 偏移表读短了（第 %u 项 / 共 %u）",
                   path, (unsigned)i, (unsigned)(total + 1u));
            spf6_close(pf);
            return OPRT_COM_ERROR;
        }
        pf->offs[i] = prv_rd32(b);

        if (i > 0u && pf->offs[i] < pf->offs[i - 1u]) {
            PR_ERR("[spf6] %s 偏移表不是单调的（第 %u 项 %u < 前一项 %u）",
                   path, (unsigned)i, (unsigned)pf->offs[i],
                   (unsigned)pf->offs[i - 1u]);
            spf6_close(pf);
            return OPRT_INVALID_PARM;
        }
    }

    pf->data_off = pf->offs[0];
    if (pf->data_off < SPF6_HDR_LEN) {
        PR_ERR("[spf6] %s 数据区偏移 %u 落在头部里", path, (unsigned)pf->data_off);
        spf6_close(pf);
        return OPRT_INVALID_PARM;
    }

    /* 每组的首帧文件偏移：读帧时 O(1) 定位，不用再走一遍表 */
    for (uint32_t i = 0; i < nc; i++) {
        pf->clips[i].data_off = pf->offs[pf->clips[i].first];
    }

    /* ---- 文件大小自检：最后一帧的末尾必须真的在文件里 ----
     *
     * 拷贝被截断 / 卡把尾部写坏了的时候，偏移表（在文件开头）是完好的，只有
     * 读到靠后的帧才短读。而短读会被精灵当成"卡读坏"去重建链路 —— 于是表现成
     * "每次播到某个动作就重建 SD"，很难往"文件是残的"上想。
     *
     * 这里直接比一次大小，把结论打进日志。**不判失败**：前面的动作还能用，
     * 让它们继续播，总比整只宠物起不来强。 */
    {
        int64_t fsz = -1;

        if (0 == tkl_fseek(pf->f, 0, SEEK_END)) {
            fsz = tkl_ftell(pf->f);
        }
        if (fsz > 0 && (uint64_t)fsz < (uint64_t)pf->offs[pf->total_frames]) {
            PR_ERR("[spf6] %s 被**截断**：实际 %u 字节，偏移表要求 %u 字节 "
                   "（差 %u）—— 卡上这份是残的，重新拷一遍；"
                   "靠后的动作会读不出来并反复触发链路重建",
                   path, (unsigned)fsz, (unsigned)pf->offs[pf->total_frames],
                   (unsigned)(pf->offs[pf->total_frames] - (uint32_t)fsz));
        }
    }

    return OPRT_OK;
}
