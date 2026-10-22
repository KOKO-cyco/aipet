#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
adjust_gui.py — 角色动作调优工具（肉眼调大小/位置，防形变）

用法（需要带 tkinter 的 Python，本机用 Anaconda）:
    python adjust_gui.py                  # 打开 GUI，默认源 = 豆包/统一动作集
    python adjust_gui.py --src <目录>      # 指定源目录（里面放 *.webm）
    python adjust_gui.py --selftest        # 不弹窗，自检数据加载与参数换算

功能:
  读取源目录所有 webm 的【第一帧】，在窗口画布上逐个人眼调整:
    拖动 = 移动位置        滚轮 = 整体缩放（以脚底中心为锚）
    Alt+滚轮 = 只调宽       Shift+滚轮 = 只调高
    方向键 = 微调 1px (Shift=10px)       右侧面板可直接输入数值
  默认锁定长宽比(防角色形变)；解锁后单独拉宽/高时实时显示形变百分比。
  红色半透明轮廓 = 待机的姿势(ghost)，供其他动作对齐。
  标准角色虚影 = 右侧下拉选一个标准角色(canonical 首帧 或 任意动作)，它的半透明
                角色会投放到所有动作窗口，调大小/位置时对齐它；金色虚线框 = 其包围盒。
  [复制标准角色参数] 一键把标准角色的 中心/脚底/宽/高 搬到当前动作。
  调完 [保存参数] 写 豆包/调优参数.json，[渲染输出] 调 apply_adjust.py
  按参数重出全部 webm 并合并成 豆包/全部动作_调优版.webm。

ffmpeg 探测顺序: imageio_ffmpeg → managed venv 自带 → PATH → WSL(兜底, 自动转路径)。
"""
import os
import sys
import json
import glob
import subprocess
import threading
from pathlib import Path

try:                                # GUI 需要 tkinter; --selftest 不弹窗, 缺 tkinter 也能跑
    import tkinter as tk
except ImportError:
    tk = None

import numpy as np
from PIL import Image, ImageTk, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent                      # 待处理动作/
CACHE = ROOT / "豆包" / "_gui_cache"
PARAMS_JSON = ROOT / "豆包" / "调优参数.json"
CW, CH = 1280, 720                     # 逻辑画布(与视频同尺寸)
DISP = 1.0                             # 显示缩放(启动时按屏幕实测回填, 见 _fit_display/_fit_canvas)
VW, VH = int(CW * DISP), int(CH * DISP)
AVAIL_W, AVAIL_H = 0, 0                # 工作区尺寸(窗口最大化后实测)
ORDER = ["待机", "开心", "伤心", "傲娇", "思考", "拍照", "舞狮", "记笔记"]
STD_PNG = ROOT / "豆包" / "首帧_已抠像.png"   # 标准大小虚影(与统一动作集首尾帧同一基准)


# ---------------- ffmpeg 探测（原生优先，WSL 兜底） ----------------

def _find_native_ffmpeg():
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        pass
    cands = glob.glob(str(Path.home() / ".workbuddy/binaries/python/envs/default/"
                          "Lib/site-packages/imageio_ffmpeg/binaries/*.exe"))
    if cands:
        return cands[0]
    from shutil import which
    return which("ffmpeg")


def _wsl_has_ffmpeg():
    try:
        r = subprocess.run(["wsl", "ffmpeg", "-version"], capture_output=True, timeout=20)
        return r.returncode == 0
    except Exception:
        return False


class FfmpegRunner:
    """原生 ffmpeg 优先；找不到时退回 WSL（自动把 Windows 路径转 /mnt/...）。"""

    def __init__(self):
        self.exe = _find_native_ffmpeg()
        self.mode = "native" if self.exe else ("wsl" if _wsl_has_ffmpeg() else None)
        if self.mode is None:
            raise RuntimeError("找不到 ffmpeg（原生 / PATH / WSL 都没有）")

    def _wslize(self, a):
        if len(a) > 1 and a[1] == ":" and (a[0].isalpha()):        # C:\x -> /mnt/c/x
            p = a.replace("\\", "/")
            return "/mnt/" + p[0].lower() + p[2:]
        return a

    def run(self, args, **kw):
        if self.mode == "wsl":
            cmd = ["wsl", "ffmpeg"] + [self._wslize(a) for a in args]
        else:
            cmd = [self.exe] + args
        return subprocess.run(cmd, **kw)

    def desc(self):
        return f"{self.mode}:{self.exe}" if self.mode == "native" else "wsl:ffmpeg"


FF = None


def ff():
    global FF
    if FF is None:
        FF = FfmpegRunner()
    return FF


# ---------------- 帧提取 / bbox 测量（GUI 与渲染共用口径） ----------------

def grab_first_frame(webm: Path, out_png: Path) -> bool:
    out_png.parent.mkdir(parents=True, exist_ok=True)
    r = ff().run(["-y", "-c:v", "libvpx-vp9", "-i", str(webm),
                  "-frames:v", "1", str(out_png)], capture_output=True)
    return out_png.exists() and out_png.stat().st_size > 1000


def body_bbox(png: Path):
    """角色本体包围盒 (x0,y0,x1,y1)，口径 alpha>200（不含脚底半透明阴影）。"""
    a = np.asarray(Image.open(png).convert("RGBA"))[..., 3]
    m = a > 200
    ys, xs = np.where(m)
    if not len(ys):
        return None
    return [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())]


def identity_params(bbox):
    """恒等变换参数: cx/脚底/宽/高 = 源帧角色 bbox 原样。"""
    return dict(cx=(bbox[0] + bbox[2]) / 2.0, bottom=float(bbox[3]),
                w=float(bbox[2] - bbox[0] + 1), h=float(bbox[3] - bbox[1] + 1))


def params_to_render(p: dict, bbox):
    """GUI 参数 -> key_greenscreen.py 的 (--scale, --stretch, --place)。
    整帧 x 缩放 = w/bw；整帧 y 缩放 = h/bh = scale*stretch → stretch = sy/sx。
    底中锚点使脚底位置由 bottom 精确控制。"""
    bw = bbox[2] - bbox[0] + 1
    bh = bbox[3] - bbox[1] + 1
    sx = p["w"] / bw
    sy = p["h"] / bh
    return dict(scale=sx, stretch=sy / sx, place=(p["cx"], p["bottom"]))


def scan_source(src_dir: Path):
    """返回 [(name, webm_path)]，按 ORDER 优先，其余按名字。"""
    vids = {}
    for p in sorted(src_dir.glob("*.webm")):
        vids[p.stem] = p
    names = [n for n in ORDER if n in vids] + sorted(n for n in vids if n not in ORDER)
    return [(n, vids[n]) for n in names]


def load_frames(src_dir: Path, verbose=True):
    """抽首帧(带缓存) + 测 bbox。返回 frames{name:PIL}, meta{name:{src,bbox,png}}"""
    fdir = CACHE / "frames"
    fdir.mkdir(parents=True, exist_ok=True)
    frames, meta = {}, {}
    for name, webm in scan_source(src_dir):
        png = fdir / f"{name}.png"
        if not png.exists() or webm.stat().st_mtime > png.stat().st_mtime:
            if not grab_first_frame(webm, png):
                print(f"!! {name}: 首帧抽取失败")
                continue
        bbox = body_bbox(png)
        if bbox is None:
            print(f"!! {name}: 测不到角色本体(alpha>200)")
            continue
        frames[name] = Image.open(png).convert("RGBA")
        meta[name] = dict(src=str(webm), bbox=bbox, png=str(png))
        if verbose:
            print(f"  {name}: bbox={bbox} ({bbox[2]-bbox[0]+1}x{bbox[3]-bbox[1]+1}px)")
    return frames, meta


# ---------------- 标准大小虚影 ----------------

def load_standard():
    """标准大小虚影 = canonical 首帧(豆包/首帧_已抠像.png)的轮廓与包围盒。
    它是统一动作集的首尾帧基准(与每个动作的首帧完全一致)，
    作为大小/位置的对齐标准; 固定不变, 不随源目录切换而变。
    文件不存在时返回 None(降级为只有待机 ghost)。"""
    try:
        a = np.asarray(Image.open(STD_PNG).convert("RGBA"))
    except Exception:
        return None
    m = a[..., 3] > 128
    ys, xs = np.where(m)
    if not len(ys):
        return None
    return dict(rgba=a, bbox=[int(xs.min()), int(ys.min()),
                              int(xs.max()), int(ys.max())])


# ---------------- GUI ----------------

def _font(sz):
    for f in ("Microsoft YaHei", "msyh", "SimHei"):
        try:
            from tkinter import font as tkfont
            return tkfont.Font(family=f, size=sz)
        except Exception:
            continue
    return None


class AdjustApp:
    def __init__(self, root, src_dir: Path):
        self.root = root
        self.src_dir = src_dir
        self.frames, self.meta = load_frames(src_dir)
        if not self.frames:
            raise RuntimeError(f"源目录没有可用动作: {src_dir}")
        self.names = [n for n, _ in scan_source(src_dir) if n in self.frames]
        self.cur = self.names[0]
        self.params = {n: identity_params(self.meta[n]["bbox"]) for n in self.names}
        self.aspect = {n: self.params[n]["w"] / self.params[n]["h"] for n in self.names}
        self._load_saved_params()          # 上次调过的参数自动续上
        self.lock = True
        self.show_ghost = True
        self.ghost_name = "待机" if "待机" in self.names else None
        self.std_canon = load_standard()   # canonical 标准角色(可作虚影源)
        self.show_std = True
        self.std_choice = tk.StringVar(
            value="canonical(首帧)" if self.std_canon
            else (self.names[0] if self.names else ""))
        self._drag = None
        self._photos = []          # 防 GC
        self._trace_cb = {}        # trace 回调名(同步面板时临时摘除, 防半截数字触发重绘)
        self._build()

    # ---------- 构建界面 ----------
    def _load_saved_params(self):
        """启动时若有 调优参数.json 且覆盖当前全部动作, 自动续上(可从上次接着调)。"""
        try:
            if not PARAMS_JSON.exists():
                return
            d = json.loads(PARAMS_JSON.read_text(encoding="utf-8"))
            acts = d.get("actions", {})
            if set(acts) >= set(self.names):
                for n in self.names:
                    self.params[n].update({k: float(acts[n][k]) for k in ("cx", "bottom", "w", "h")})
        except Exception:
            pass

    def _build(self):
        r = self.root
        r.title("角色动作调优工具 — 拖动移动 / 滚轮缩放 / Alt调宽 / Shift调高 / 方向键微调")
        r.resizable(False, False)
        r.configure(bg="#f2f3f5")

        # 顶栏
        top = self.top_bar = tk.Frame(r, bg="#f2f3f5")
        top.pack(fill="x", padx=8, pady=6)
        tk.Label(top, text="源目录:", bg="#f2f3f5").pack(side="left")
        self.src_var = tk.StringVar(value=str(self.src_dir))
        self.src_entry = tk.Entry(top, textvariable=self.src_var, width=46)
        self.src_entry.pack(side="left", padx=4)
        tk.Button(top, text="浏览", command=self._browse).pack(side="left")
        tk.Button(top, text="保存参数", command=self.save).pack(side="left", padx=(16, 4))
        tk.Button(top, text="渲染输出", command=self.render).pack(side="left")
        std_txt = "" if self.std_canon else " | canonical 缺失(可选动作作标准)"
        self.status = tk.Label(top, text=f"ffmpeg {ff().desc()} | {len(self.names)} 个动作{std_txt}",
                               bg="#f2f3f5", fg="#555")
        self.status.pack(side="right")

        body = self.body = tk.Frame(r, bg="#f2f3f5")
        body.pack(fill="both", expand=True, padx=8, pady=4)

        # 左侧动作列表
        lf = tk.Frame(body, bg="#f2f3f5")
        lf.pack(side="left", fill="y")
        tk.Label(lf, text="动作", bg="#f2f3f5").pack()
        self.list = tk.Listbox(lf, width=11, font=("Microsoft YaHei", 12), exportselection=False)
        for n in self.names:
            self.list.insert("end", n)
        self.list.pack(fill="y")
        self.list.bind("<<ListboxSelect>>", lambda e: self.select(self.list.get(self.list.curselection())))
        # 列表聚焦时方向键默认会切换选项; 拦截为调位置(与画布焦点行为一致)
        for ks in ("<Key-Up>", "<Key-Down>", "<Key-Left>", "<Key-Right>"):
            self.list.bind(ks, lambda e: (self._key(e), "break")[1])

        # 中间画布
        self.cv = tk.Canvas(body, width=VW, height=VH, bg="#8a8d94", highlightthickness=1,
                            highlightbackground="#c7c9cf")
        self.cv.pack(side="left", padx=6)
        self.cv.bind("<ButtonPress-1>", self._press)
        self.cv.bind("<B1-Motion>", self._motion)
        self.cv.bind("<MouseWheel>", self._wheel)
        self.cv.bind("<Button-4>", lambda e: self._wheel(e, 1))
        self.cv.bind("<Button-5>", lambda e: self._wheel(e, -1))
        r.bind("<KeyPress>", self._key)
        self.cv.focus_set()

        # 右侧参数面板
        pf = tk.Frame(body, bg="#f2f3f5")
        pf.pack(side="left", fill="y", padx=6)
        self.cur_lbl = tk.Label(pf, text="", bg="#f2f3f5", font=("Microsoft YaHei", 12, "bold"))
        self.cur_lbl.pack(anchor="w")
        grid = tk.Frame(pf, bg="#f2f3f5")
        grid.pack(anchor="w", pady=4)
        self.vars = {}
        for i, (key, lab) in enumerate([("cx", "中心X"), ("bottom", "脚底Y"), ("w", "宽"), ("h", "高")]):
            tk.Label(grid, text=lab, bg="#f2f3f5").grid(row=i // 2, column=(i % 2) * 2, sticky="e")
            v = tk.StringVar()
            tk.Entry(grid, textvariable=v, width=8).grid(row=i // 2, column=(i % 2) * 2 + 1, padx=2)
            self.vars[key] = v
        self.lock_var = tk.BooleanVar(value=True)
        tk.Checkbutton(pf, text="锁定长宽比(防形变)", variable=self.lock_var,
                       command=self._toggle_lock, bg="#f2f3f5").pack(anchor="w")
        # 形变%: 手动输入(0.01 精度), 聚焦后方向键上下 ±0.01 微调
        df = tk.Frame(pf, bg="#f2f3f5")
        df.pack(anchor="w")
        tk.Label(df, text="形变%", bg="#f2f3f5").pack(side="left")
        self.deform_var = tk.StringVar(value="0.00")
        self.deform_entry = tk.Entry(df, textvariable=self.deform_var, width=6)
        self.deform_entry.pack(side="left", padx=2)
        self.deform_entry.bind("<Return>", lambda e: self.apply_deform())
        self.deform_entry.bind("<FocusOut>", lambda e: self.apply_deform())
        self.deform_entry.bind("<Key-Up>", self._deform_step)
        self.deform_entry.bind("<Key-Down>", self._deform_step)
        self.scale_lbl = tk.Label(pf, text="", bg="#f2f3f5", fg="#666")
        self.scale_lbl.pack(anchor="w")
        btns = tk.Frame(pf, bg="#f2f3f5")
        btns.pack(anchor="w", pady=4)
        tk.Button(btns, text="上一个", command=lambda: self.step(-1)).pack(side="left")
        tk.Button(btns, text="下一个", command=lambda: self.step(1)).pack(side="left", padx=4)
        tk.Button(pf, text="复制待机参数", command=self.copy_idle).pack(anchor="w")
        tk.Button(pf, text="恢复默认(源原样)", command=self.reset_default).pack(anchor="w", pady=2)
        self.ghost_var = tk.BooleanVar(value=True)
        tk.Checkbutton(pf, text="显示待机轮廓(ghost)", variable=self.ghost_var,
                       command=self.redraw, bg="#f2f3f5").pack(anchor="w", pady=(6, 0))
        # 标准角色(虚影): 选中后半透明投放到所有动作窗口, 作为统一调整基准
        tk.Label(pf, text="标准角色(虚影):", bg="#f2f3f5").pack(anchor="w", pady=(8, 0))
        self.std_menu = tk.OptionMenu(pf, self.std_choice, *self._std_options(),
                                      command=lambda _v: self.redraw())
        self.std_menu.pack(anchor="w")
        self.std_var = tk.BooleanVar(value=True)
        tk.Checkbutton(pf, text="显示标准角色虚影", variable=self.std_var,
                       command=self.redraw, bg="#f2f3f5").pack(anchor="w")
        tk.Button(pf, text="复制标准角色参数", command=self.copy_std).pack(anchor="w", pady=2)

        # 底部提示
        self.hint_lbl = tk.Label(r, text="拖动=移动 | 滚轮=缩放(脚底为锚) | Alt+滚轮=只调宽 | Shift+滚轮=只调高 | "
                                         "方向键=微调1px(Shift=10px) | 半透明虚影=标准角色(下拉可选, 所有动作对齐它) | "
                                         "红色虚影=待机姿势",
                                 bg="#e8eaee", fg="#444", font=("Microsoft YaHei", 11))
        self.hint_lbl.pack(fill="x", padx=8, pady=(0, 6))

        # 画布填满剩余工作区(右面板完整可见的关键一步), 最后才首绘
        self._fit_canvas()

        self._ghost_img = None
        self.select(self.cur)

    def _fit_canvas(self):
        """画布填满剩余工作区(保持 16:9, 与视频帧同比例):
        实测 左侧列表+右侧面板+顶栏+底栏 各占多少, 差额全给画布,
        整窗请求尺寸因此不超工作区 -> 右侧参数面板完整可见, 不会被挤出屏幕。
        顶栏(源目录输入框+状态文本)自身超宽时, 按需收窄输入框(小屏兜底)。"""
        global DISP, VW, VH
        r = self.root
        disp = DISP
        for _ in range(24):
            r.update_idletasks()
            wide = max(self.top_bar.winfo_reqwidth(), self.hint_lbl.winfo_reqwidth())
            w = int(self.src_entry.cget("width"))
            if wide > AVAIL_W and w > 14:
                self.src_entry.config(width=w - 4)      # 顶栏超宽: 收窄源目录输入框
                continue
            ex_w = self.body.winfo_reqwidth() - self.cv.winfo_reqwidth()
            ex_h = (self.top_bar.winfo_reqheight()
                    + self.body.winfo_reqheight() - self.cv.winfo_reqheight()
                    + self.hint_lbl.winfo_reqheight())
            disp = min((AVAIL_W - ex_w) / CW, (AVAIL_H - ex_h) / CH)
            break
        DISP = max(0.6, round(disp, 4))
        VW, VH = int(CW * DISP), int(CH * DISP)
        self.cv.config(width=VW, height=VH)
        r.update_idletasks()

    # ---------- 源切换 ----------
    def _browse(self):
        from tkinter import filedialog
        d = filedialog.askdirectory(initialdir=str(ROOT / "豆包"))
        if not d:
            return
        self.src_var.set(d)
        self._reload(Path(d))

    def _reload(self, src: Path):
        try:
            self.frames, self.meta = load_frames(src)
            self.src_dir = src
            self.names = [n for n, _ in scan_source(src) if n in self.frames]
            if not self.names:
                raise RuntimeError("目录里没有 webm")
            self.params = {n: identity_params(self.meta[n]["bbox"]) for n in self.names}
            self.aspect = {n: self.params[n]["w"] / self.params[n]["h"] for n in self.names}
            self.ghost_name = "待机" if "待机" in self.names else None
            self._rebuild_std_menu()
            self.list.delete(0, "end")
            for n in self.names:
                self.list.insert("end", n)
            self.status.config(text=f"ffmpeg {ff().desc()} | {len(self.names)} 个动作")
            self.select(self.names[0])
        except Exception as e:
            from tkinter import messagebox
            messagebox.showerror("加载失败", str(e))

    # ---------- 选择/绘制 ----------
    def select(self, name):
        if name not in self.frames:
            return
        self.cur = name
        self.list.selection_clear(0, "end")
        self.list.selection_set(self.names.index(name))
        self.sync_panel()
        self.redraw()

    def step(self, d):
        i = self.names.index(self.cur)
        self.select(self.names[(i + d) % len(self.names)])

    def _toggle_lock(self):
        self.lock = self.lock_var.get()
        if self.lock:                    # 重新锁定: 高度回到源比例(形变归零)
            p = dict(self.params[self.cur])
            p["h"] = p["w"] / self.aspect[self.cur]
            self.params[self.cur] = p
            self.sync_panel()
            self.redraw()

    # ---------- 标准角色 ----------
    def _std_options(self):
        """标准角色下拉选项: canonical + 全部动作名。"""
        opts = (["canonical(首帧)"] if self.std_canon else []) + list(self.names)
        return opts or [""]

    def _std_frame(self):
        """当前选中的标准角色 -> (rgba, bbox); 不可用返回 None。"""
        name = self.std_choice.get()
        if name == "canonical(首帧)":
            s = self.std_canon
            return (s["rgba"], s["bbox"]) if s else None
        if name in self.frames:
            return (np.asarray(self.frames[name]), self.meta[name]["bbox"])
        return None

    def _rebuild_std_menu(self):
        """源目录切换后重建标准角色下拉项(保留当前选择, 失效则回退)。"""
        if not hasattr(self, "std_menu"):
            return
        menu = self.std_menu["menu"]
        menu.delete(0, "end")
        opts = self._std_options()
        for o in opts:
            menu.add_command(label=o,
                             command=lambda v=o: (self.std_choice.set(v), self.redraw()))
        if self.std_choice.get() not in opts:
            self.std_choice.set(opts[0])

    def _char_geometry(self):
        """当前动作整帧缩放后的显示图 + 画布坐标。"""
        name = self.cur
        p, bb = self.params[name], self.meta[name]["bbox"]
        bw, bh = bb[2] - bb[0] + 1, bb[3] - bb[1] + 1
        sx, sy = p["w"] / bw, p["h"] / bh
        img = self.frames[name].resize((max(1, round(CW * sx)), max(1, round(CH * sy))), Image.LANCZOS)
        disp = img.resize((max(1, round(img.width * DISP)), max(1, round(img.height * DISP))), Image.LANCZOS)
        ox = (p["cx"] - (bb[0] + bb[2]) / 2 * sx) * DISP
        oy = (p["bottom"] - bb[3] * sy) * DISP
        return disp, ox, oy, sx, sy

    def redraw(self):
        self.cv.delete("all")
        # 网格 + 中心线 + 脚底基准线
        for gx in range(0, CW + 1, 160):
            self.cv.create_line(gx * DISP, 0, gx * DISP, VH, fill="#7c7f86")
        for gy in range(0, CH + 1, 180):
            self.cv.create_line(0, gy * DISP, VW, gy * DISP, fill="#7c7f86")
        self.cv.create_line(CW / 2 * DISP, 0, CW / 2 * DISP, VH, fill="#3a6fd8", width=1)
        if self.ghost_name:
            bb = self.meta[self.ghost_name]["bbox"]
            self.cv.create_line(0, bb[3] * DISP, VW, bb[3] * DISP, fill="#d83a3a",
                                width=1, dash=(6, 4))
        # ghost 待机轮廓
        if self.show_ghost and self.ghost_var.get() and self.ghost_name and self.ghost_name != self.cur:
            src = self.frames[self.ghost_name]
            a = np.asarray(src)[..., 3]
            ov = np.zeros((*a.shape, 4), np.uint8)
            m = a > 128
            ov[m] = (255, 70, 70, 110)
            g = Image.fromarray(ov, "RGBA").resize((VW, VH), Image.LANCZOS)
            self._ghost_img = ImageTk.PhotoImage(g)
            self.cv.create_image(0, 0, anchor="nw", image=self._ghost_img)
        # 角色
        disp, ox, oy, sx, sy = self._char_geometry()
        photo = ImageTk.PhotoImage(disp)
        self._photos = [photo]
        self.cv.create_image(ox, oy, anchor="nw", image=photo, tags="char")
        self.cv.tag_bind("char", "<ButtonPress-1>", self._press)
        self.cv.tag_bind("char", "<B1-Motion>", self._motion)
        # 标准角色虚影(最上层): 选中角色 45% 半透明投放到画布(所有动作窗口一致), 附包围盒虚线框
        if self.show_std and self.std_var.get() and self.std_choice.get() != self.cur:
            sf = self._std_frame()
            if sf:
                rgba, bb = sf
                ov = rgba.copy()
                ov[..., 3] = (ov[..., 3].astype(np.float32) * 0.45).astype(np.uint8)
                g = Image.fromarray(ov, "RGBA").resize((VW, VH), Image.LANCZOS)
                self._std_img = ImageTk.PhotoImage(g)
                self.cv.create_image(0, 0, anchor="nw", image=self._std_img,
                                     tags="stdghost")
                x0, y0, x1, y1 = (v * DISP for v in bb)
                self.cv.create_rectangle(x0, y0, x1, y1, outline="#e0a81a",
                                         width=2, dash=(7, 5), tags="stdghost")
        self.sync_scale_lbl(sx, sy)

    def sync_scale_lbl(self, sx, sy):
        self.scale_lbl.config(text=f"缩放: {sx*100:.1f}% × {sy*100:.1f}%")
        self._set_deform_display()

    def _set_deform_display(self):
        """形变框显示当前形变(0.01 精度)并上色: <1% 绿 / 1~3% 黄 / >3% 红。"""
        p = self.params[self.cur]
        d = (p["h"] / p["w"] * self.aspect[self.cur] - 1) * 100
        self.deform_var.set(f"{d:.2f}")
        c = "#1a7d32" if abs(d) < 1 else ("#b8860b" if abs(d) < 3 else "#c02020")
        self.deform_entry.config(fg=c)

    # ---------- 面板 <-> 参数 ----------
    def sync_panel(self):
        p = self.params[self.cur]
        self.cur_lbl.config(text=f"当前: {self.cur}")
        for k, v in self.vars.items():
            if k in self._trace_cb:                 # 摘除 trace, 防止半截输入数字触发重绘
                v.trace_remove("write", self._trace_cb.pop(k))
            v.set(f"{p[k]:.0f}")
            self._trace_cb[k] = v.trace_add("write", lambda *a: self.apply_panel())
        self._set_deform_display()

    def apply_panel(self):
        try:
            p = dict(self.params[self.cur])
            for k in ("cx", "bottom", "w", "h"):
                p[k] = float(self.vars[k].get())
            if p["w"] < 10 or p["h"] < 10:
                return
            if self.lock:
                p["h"] = p["w"] / self.aspect[self.cur]
            self.params[self.cur] = p
            self.redraw()
        except ValueError:
            pass

    def apply_deform(self):
        """形变框 -> 按 sy/sx-1 设高度; 手动输入形变视为主动解锁长宽比。"""
        try:
            d = float(self.deform_var.get())
        except ValueError:
            return
        p = dict(self.params[self.cur])
        p["h"] = p["w"] / self.aspect[self.cur] * (1.0 + d / 100.0)
        self.params[self.cur] = p
        if self.lock:                              # 输入形变 = 主动要形变 -> 解锁
            self.lock = False
            self.lock_var.set(False)
        self.sync_panel()
        self.redraw()

    def _deform_step(self, e):
        """形变框聚焦时: 上下方向键 ±0.01 微调(0.01 分辨率)。"""
        try:
            cur = float(self.deform_var.get())
        except ValueError:
            cur = 0.0
        cur += 0.01 if e.keysym == "Up" else -0.01
        self.deform_var.set(f"{cur:.2f}")
        self.apply_deform()
        return "break"                              # 阻止全局方向键(调脚底)冒泡

    # ---------- 交互 ----------
    def _press(self, e):
        self._drag = (e.x, e.y, self.params[self.cur]["cx"], self.params[self.cur]["bottom"])

    def _motion(self, e):
        if not self._drag:
            return
        x0, y0, cx0, b0 = self._drag
        p = dict(self.params[self.cur])
        p["cx"] = cx0 + (e.x - x0) / DISP
        p["bottom"] = b0 + (e.y - y0) / DISP
        self.params[self.cur] = p
        self.sync_panel()
        disp, ox, oy, sx, sy = self._char_geometry()
        photo = ImageTk.PhotoImage(disp)
        self._photos = [photo]
        self.cv.delete("char")
        self.cv.create_image(ox, oy, anchor="nw", image=photo, tags="char")
        self.cv.tag_raise("stdghost")   # 拖动是增量重画: 重建的角色会盖住虚影, 提回最上层

    def _wheel(self, e, manual=None):
        d = manual if manual is not None else (1 if e.delta > 0 else -1)
        f = 1.06 ** d
        ctrl = bool(e.state & 0x4)          # Ctrl
        alt = bool(e.state & 0x20000)       # Alt (Mod2)
        shift = bool(e.state & 0x1)         # Shift
        p = dict(self.params[self.cur])
        if ctrl or alt:                     # 只调宽
            p["w"] *= f
        elif shift:                         # 只调高
            p["h"] *= f
        else:
            p["w"] *= f
            if self.lock:
                p["h"] = p["w"] / self.aspect[self.cur]
            else:
                p["h"] *= f
        self.params[self.cur] = p
        self.sync_panel()
        self.redraw()

    def _key(self, e):
        step = 10 if (e.state & 0x1) else 1
        p = dict(self.params[self.cur])
        if e.keysym == "Left":
            p["cx"] -= step
        elif e.keysym == "Right":
            p["cx"] += step
        elif e.keysym == "Up":
            p["bottom"] -= step
        elif e.keysym == "Down":
            p["bottom"] += step
        else:
            return
        self.params[self.cur] = p
        self.sync_panel()
        self.redraw()

    # ---------- 工具 ----------
    def copy_idle(self):
        if not self.ghost_name or self.ghost_name == self.cur:
            return
        self.params[self.cur] = dict(self.params[self.ghost_name])
        self.sync_panel()
        self.redraw()

    def copy_std(self):
        """把当前选中的标准角色的 中心/脚底/宽/高 搬到当前动作(再微调)。"""
        sf = self._std_frame()
        if not sf or self.std_choice.get() == self.cur:
            return
        _, bb = sf
        p = dict(cx=(bb[0] + bb[2]) / 2.0, bottom=float(bb[3]),
                 w=float(bb[2] - bb[0] + 1), h=float(bb[3] - bb[1] + 1))
        self.params[self.cur] = p
        self.aspect[self.cur] = p["w"] / p["h"]
        self.sync_panel()
        self.redraw()

    def reset_default(self):
        self.params[self.cur] = identity_params(self.meta[self.cur]["bbox"])
        self.sync_panel()
        self.redraw()

    # ---------- 保存 / 渲染 ----------
    def save(self):
        data = dict(version=1, canvas=[CW, CH], source_dir=str(self.src_dir),
                    ffmpeg=ff().desc(),
                    actions={n: {k: round(v, 2) for k, v in self.params[n].items()}
                             for n in self.names},
                    meta={n: dict(src=self.meta[n]["src"], bbox=self.meta[n]["bbox"])
                          for n in self.names})
        PARAMS_JSON.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
        self.status.config(text=f"已保存 {PARAMS_JSON.name} | {len(self.names)} 个动作")
        return PARAMS_JSON

    def render(self):
        pj = self.save()
        self.status.config(text="渲染中…（大视频约几分钟，完成后弹窗提示）")

        def work():
            try:
                r = subprocess.run([sys.executable, str(HERE / "apply_adjust.py"),
                                    "--params", str(pj)],
                                   capture_output=True, text=True, timeout=3600)
                ok = r.returncode == 0
                msg = (r.stdout[-1500:] or "") + (r.stderr[-800:] or "")
                self.root.after(0, lambda: self._render_done(ok, msg))
            except Exception as e:
                self.root.after(0, lambda: self._render_done(False, str(e)))

        threading.Thread(target=work, daemon=True).start()

    def _render_done(self, ok, msg):
        from tkinter import messagebox
        if ok:
            self.status.config(text="渲染完成 → 豆包/最终输出/ + 全部动作_调优版.webm")
            messagebox.showinfo("完成", "已按参数输出到:\n豆包/最终输出/\n并生成合并版:\n豆包/全部动作_调优版.webm")
        else:
            self.status.config(text="渲染失败")
            messagebox.showerror("渲染失败", msg)


# ---------------- 自检 ----------------

def selftest(src: Path):
    print(f"ffmpeg: {ff().desc()}")
    print(f"源目录: {src}")
    frames, meta = load_frames(src)
    if not frames:
        print("FAIL: 无可用动作")
        sys.exit(1)
    ok = True
    print(f"\n{'动作':<8} {'bbox':>22} {'roundtrip 误差(px)':<18}")
    for name, webm in scan_source(src):
        if name not in meta:
            continue
        bb = meta[name]["bbox"]
        p = identity_params(bb)
        rd = params_to_render(p, bb)
        # 反算: 整帧按 (scale, stretch) 缩放后, 角色 bbox 应落在目标位置
        bw, bh = bb[2] - bb[0] + 1, bb[3] - bb[1] + 1
        sx, sy = rd["scale"], rd["scale"] * rd["stretch"]
        cx2, bottom2 = rd["place"]
        ex = abs((bb[0] + bb[2]) / 2 * sx + (cx2 - (bb[0] + bb[2]) / 2 * sx) - cx2)
        ey = abs(bb[3] * sy + (bottom2 - bb[3] * sy) - bottom2)
        ew = abs(bw * sx - p["w"])
        eh = abs(bh * sy - p["h"])
        err = max(ex, ey, ew, eh)
        flag = "OK" if err < 0.01 else "FAIL"
        ok = ok and err < 0.01
        print(f"{name:<8} {str(bb):>22} {err:>8.4f}  {flag}")
    print("\nSELFTEST", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


def _fit_display(root):
    """窗口最大化铺满屏幕(不遮任务栏); 记录工作区尺寸(AVAIL_W/AVAIL_H)。
    画布最终显示尺寸由 _build() -> _fit_canvas() 在量完 左侧列表/右侧面板/
    顶栏/底栏 的实际占用后回填, 保证整窗请求尺寸不超工作区 ——
    右侧参数面板完整可见, 不会再被挤出屏幕。"""
    global AVAIL_W, AVAIL_H, DISP, VW, VH
    try:
        root.state("zoomed")                 # 最大化(标准行为, 不遮任务栏)
        root.update()
        AVAIL_W = root.winfo_width() or root.winfo_screenwidth()
        AVAIL_H = root.winfo_height() or root.winfo_screenheight()
    except Exception:
        AVAIL_W = root.winfo_screenwidth()
        AVAIL_H = root.winfo_screenheight()
    AVAIL_W = max(640, AVAIL_W - 8)          # 少量安全边距, 防边界取整被裁
    AVAIL_H = max(480, AVAIL_H - 8)
    # 初始估值(偏保守, 保证首次请求就在屏内); 精确值由 _fit_canvas() 实测回填
    DISP = max(0.6, round(min((AVAIL_W - 480) / CW, (AVAIL_H - 80) / CH), 3))
    VW, VH = int(CW * DISP), int(CH * DISP)


def main():
    args = sys.argv[1:]
    src = ROOT / "豆包" / "统一动作集"
    if "--src" in args:
        src = Path(args[args.index("--src") + 1])
    if "--selftest" in args:
        selftest(src)
        return
    if tk is None:
        sys.exit("[ERROR] 当前 Python 没有 tkinter，请改用 Anaconda 的 python 启动: "
                 "D:\\Anaconda3\\python.exe 豆包\\调优工具.bat")
    try:                                    # 高 DPI 清晰化, 失败无所谓
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    root = tk.Tk()
    _fit_display(root)
    AdjustApp(root, src)
    root.mainloop()


if __name__ == "__main__":
    main()
