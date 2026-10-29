/**
 * @file app_pages.h
 * @brief 页面清单：新增页面时在这里加一行 extern，并在 app_pages.c 里注册。
 *
 * 现在只有两个页面：
 *   - pet   桌宠（唯一的主界面，开机就进；顶部状态栏是它的一个控件，
 *           见 app_statusbar.h，不是页面）
 *   - nosd  SD 卡不在位时的门页
 *
 * 备忘录和笔记不是路由页 —— 它们是桌宠页右侧面板里的页面，见 page_pet.c 的文件头。
 */

#ifndef __APP_PAGES_H__
#define __APP_PAGES_H__

#include "app_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 页面名（导航用的字符串，必须是静态生存期） */
#define PAGE_PET        "pet"       /* 桌宠主界面，见 page_pet.c */
#define PAGE_NOSD       "nosd"      /* 卡不在位时的门页，见 page_nosd.c */

extern const APP_PAGE_T app_page_pet;
extern const APP_PAGE_T app_page_nosd;

/** @brief 把所有页面注册进路由。用户主流程启动时调用一次。 */
OPERATE_RET app_pages_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_PAGES_H__ */
