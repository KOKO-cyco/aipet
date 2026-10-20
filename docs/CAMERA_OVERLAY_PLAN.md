# 相机预览直接合成到 framebuffer（overlay 方案）

> 2026-10-20。当前问题：拍照页打开后 `[pet] 刷新`（一次 `lv_refr_now`）从 ~13ms 涨到
> **70~80ms**，而 `[disp] flush` 只有 1ms、相机转换 19~20ms。定位结果是 **LVGL 绘制
> 236×264 的相机 canvas 本身**（叠加系统内存/CPU 争用）吃掉了大部分时间。
> 本方案把相机画面从 LVGL 的绘制路径里拿掉，改为上屏前直接写进显示 framebuffer。

## 目标

- LVGL 只绘制宠物卡、状态栏和页面控件；
- 相机预览由应用层在 **pre-flush** 阶段直接写入当前 `TDL_DISP_FRAME_BUFF_T`；
- `[pet] 刷新` 回到 15~25ms，预览稳定 10fps，桌宠 idle 动画保持 24fps；
- 不依赖换摄像头；将来换 OV2640（全视野 320×240）可直接复用同一条链路。

## 当前显示结构（前提）

- 逻辑分辨率 480×320，物理面板 320×480 RGB565，**软件 90° 旋转**
  （`lv_port_disp.c` 的 `disp_flush` 用 `lv_draw_sw_rotate` 把每个绘制横带旋转后
  交给 `lv_port_flush_execute`）；
- 2 个绘制缓冲（每个 480×32 逻辑/320×48 物理，`LV_DRAW_BUF_PARTS=10`）；
- 3 个整屏 framebuffer（`ENABLE_LVGL_DUAL_DISP_BUFF=y` + `has_vram=false`），
  每次换 fb 目前都走整屏拷贝（DMA2D，实测 ~1ms）；
- SDK 已有 `lv_port_set_pre_flush_cb()`：在把当前 fb 交给显示驱动**之前**回调，
  参数是当前 fb 指针。设置它会让端口强制整屏同步（`sg_dirty_overflow=true`），
  代价约 1ms，可接受。

## 改动点

### 1. 相机页：去掉 canvas 的绘制

文件：`source/embedded/src/ui/pet_page_camera.c`

- `__build_preview()`：不再创建 `lv_canvas`、不再 `lv_canvas_set_buffer()`，
  改为创建一个**同尺寸透明占位对象**（无背景、无边框），只用于保持 flex 布局；
- `__cam_tick_cb()`：删掉 `lv_obj_invalidate(sg_cam_cv)`，预览更新只发生在
  `app_camera` 内部的 240×320 双缓冲里（10fps）；
- 场景顶部放返回箭头 / “实时预览” chip 的那一条（约 28px）**不参与 overlay**，
  继续由 LVGL 正常绘制。

### 2. 计算面板坐标（逻辑 → 物理）

- 页面构建完成后，对占位对象取 `lv_obj_get_coords()` 得到逻辑矩形；
- 用 `lv_display_rotate_area(lv_display_get_default(), &area)` 转成物理面板坐标
  （320×480），再按需要去掉顶部 chip 条；
- 把最终矩形（x/y/w/h）、预览源尺寸、cover 映射表在进入页面时算一次。

### 3. pre-flush 钩子

- 注册：`lv_port_set_pre_flush_cb(cb)`（进入相机页时）；注销：传 `NULL`（离开时）；
- 回调原型：`void cb(TDL_DISP_FRAME_BUFF_T *fb)`；
- 逻辑：
  1. 相机页不在前台 / `app_camera_gen()` 没变 → 直接返回；
  2. 有新帧 → 把最新 240×320 RGB565 预览按 cover 缩放到面板矩形，
     逐行写入 `fb->frame`（stride = `fb->width * 2`）；
  3. 缩放使用预计算的行/列映射表，只在 10fps 的新帧上执行一次。
- 建议在 `app_camera` 暴露一个直接写 fb 的接口，例如：

  ```c
  /* 把最新预览按 cover 画进 fb 的物理矩形 (x,y,w,h)。返回 TRUE = 画了。 */
  BOOL_T app_camera_preview_to_fb(uint16_t *fb, int fb_w, uint16_t gen,
                                  int x, int y, int w, int h);
  ```

### 4. 生命周期

- 进入相机页：注册钩子、计算矩形、记录当前 gen；
- 离开相机页：注销钩子，对整块矩形 `lv_obj_invalidate()`，让 LVGL 把底层
  UI 重画回来；
- 页面切换动画已关闭（`PET_NAV_ANIM 0`），矩形不会在动画过程中变化。

## 预期收益

| 项 | 现在 | 改后 |
|---|---|---|
| `[pet] 刷新`（一次 lv_refr_now） | 70~80ms | **15~25ms** |
| 相机 overlay 自身 | — | 10fps × ~6ms ≈ 6% CPU |
| 相机转换 | 20ms × 10fps ≈ 20% | 不变 |
| 桌宠 idle | 24fps | 保持（随机插播在拍照页期间仍暂停） |

## 风险与对策

1. **顶部返回箭头 / chip 被盖住**：overlay 矩形排除顶部约 28px，这一条交给 LVGL；
2. **方向或坐标偏移**：先往面板矩形画 4 色测试块验证上下左右。ROTT 输出已经是
   竖图，overlay 不需要再旋转，只做逻辑矩形 → 面板矩形的映射；
3. **3 个 framebuffer 轮转**：钩子每次刷新都往**当前** fb 画，端口随后整屏拷到
   下一个 fb，内容一致；反复进出页面验证无残影；
4. **边界 1px 偏差**：实测微调矩形；
5. **pre-flush 强制整屏拷贝**：约 1ms（DMA2D），可接受；若后续要省掉，可把
   overlay 矩形纳入脏区表。

## 验证

1. 烧录，进拍照页，观察：
   - `[pet] 刷新 …：平均 ?? ms` 应 ≤ 25ms；
   - `[cam] 预览 …（约 10 fps），转换 平均 ?? ms`；
   - `[disp] flush …：平均 ?? ms`（应仍为 1ms 量级）；
2. 目视：预览方向/比例正确、顶部 chip 与返回箭头不被盖、画面无撕裂；
3. 连续进出拍照页 10 次，无残影、无崩溃、无 `assign idle frame fail`；
4. 退出拍照页后，原页面 UI 完整恢复。

## 后续（可选）

- 换 OV2640 后，传感器可输出**全视野 320×240**（GC2145 的 out_win 只裁不缩），
  这条 overlay 链路不需要改结构，只需把预览源尺寸/映射表按 320×240 重算；
- 若 overlay 达标，再考虑把桌宠精灵也走同一条直写 fb 的路径（老项目 heikesong
  的 `prv_ov_compose` + `lv_port_set_pre_flush_cb` 思路），进一步省 LVGL 渲染。
