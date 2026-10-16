#!/bin/bash
# 生成音画标定素材：30 秒，第 5/15/25 秒整屏闪白（约 125ms）且同时响一声 1kHz。
# 画面和声音的三个标记严格同相，所以"听到嘀"与"看到闪白"之间的间隔就是
# 设备侧的真实音画偏差。用无损容器转一手，避免编码器 priming 干扰测量。
#
# 注意包络必须用 between(t,5,...) 这种**离散**条件：用 mod(t,1) 会变成每秒
# 都响，用户就没法判断哪一声对应闪白。
set -e
TMP=/tmp/heikesong_marks
TOOLS=/mnt/c/Users/23594/TuyaOpenIDE/projects/heikesong/tools
OUT="$TOOLS/sync_test"

rm -rf "$TMP"
mkdir -p "$TMP"
rm -rf "$OUT"

MARK="between(t,5,5.125)+between(t,15,15.125)+between(t,25,25.125)"

ffmpeg -y -hide_banner -loglevel error \
  -f lavfi -i "color=c=black:s=384x224:r=24:d=30" \
  -f lavfi -i "aevalsrc='0.7*sin(2*PI*1000*t)*($MARK)':s=16000:d=30" \
  -vf "drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='$MARK'" \
  -c:v ffv1 -c:a pcm_s16le -ar 16000 -ac 1 -shortest "$TMP/marks.mkv"

echo "--- 源片时间戳 ---"
ffprobe -v error -show_entries stream=codec_type,start_time -of default=noprint_wrappers=1 "$TMP/marks.mkv"

cd "$TOOLS"
python3 pack_video.py -i "$TMP/marks.mkv" -t 30 -n synctest -o sync_test --fps 24
