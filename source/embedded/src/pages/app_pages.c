/**
 * @file app_pages.c
 * @brief 页面注册表。
 *
 * 新增一个功能页面只需要两步：
 *   1) 写 pages/page_xxx.c，导出 `const APP_PAGE_T app_page_xxx = {...};`
 *   2) 在本文件加一行注册 —— 完事，路由/按键/事件/显示归属全部自动接上。
 */

#include "tal_api.h"

#include "app_pages.h"

OPERATE_RET app_pages_register(void)
{
    const APP_PAGE_T *pages[] = {
        &app_page_pet,          /* 桌宠：唯一的主界面 */
        &app_page_nosd,         /* 卡不在位时的门页 */
    };

    for (uint32_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        OPERATE_RET rt = app_router_register(pages[i]);

        if (OPRT_OK != rt) {
            PR_ERR("[pages] 注册 %s 失败: %d", pages[i]->name, rt);
            return rt;
        }
    }

    /* 卡不在位时的门页（见 page_nosd.c）。注册了它，路由才会在"进需要卡的页面
     * 而卡不在"时拦下来 —— 不注册的话那些页面会照进，然后在里面各自报错。 */
    (void)app_router_set_sd_gate(PAGE_NOSD);

    PR_NOTICE("[pages] 已注册 %u 个页面",
              (unsigned)(sizeof(pages) / sizeof(pages[0])));
    return OPRT_OK;
}
