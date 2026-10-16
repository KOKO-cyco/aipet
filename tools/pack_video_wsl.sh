#!/usr/bin/env bash
# 在 WSL 里跑 pack_video.py —— Windows 本机没有 ffmpeg，WSL 的 Ubuntu 里有。
#
#   ./pack_video_wsl.sh -i 测试视频.mp4 -t 60
#
# 参数原样传给 python 脚本。路径写 Windows 形式即可，这里负责转换。
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"

# pwd -W 给出 Windows 形式（C:/xxx）。git bash 的 pwd 是 /c/xxx，wslpath 会把它
# 当成 Unix 绝对路径、再拼上 WSL 的 cwd，拼成 /mnt/c/c/xxx —— 必须先用 -W。
HERE_WIN="$(pwd -W 2>/dev/null)"
[ -n "$HERE_WIN" ] || HERE_WIN="$HERE"

# 注意：不要用 `wsl bash -lc "..."` 内联整条命令 —— 变量会在某一层被吞掉。
# 先只让 wsl 做一次路径转换，再单独 exec python。
WSL_DIR="$(wsl wslpath -a "$HERE_WIN")"

exec wsl python3 "$WSL_DIR/pack_video.py" "$@"
