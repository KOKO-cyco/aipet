#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""纯 stdlib 的 MP3 规格探针（不需要 ffmpeg / mutagen）。

对每个文件报告：
  - ID3v2 标签大小、是否有 APE/ID3v1 尾巴
  - 首帧 MPEG 头：MPEG 版本 / layer / 采样率 / 声道模式 / 比特率
  - CBR / VBR 判定（Xing / Info / VBRI 头）
  - 帧数、时长、平均每帧字节
  - 是否为 mono / 16kHz（决定能不能直连 app_speaker 而不过重采样）

用法：python mp3_probe.py <dir-or-file> [<dir-or-file> ...]
"""
import os
import struct
import sys

# MPEG 版本 -> (version_id, layer 掩码用)
BITRATE_V1L3 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
BITRATE_V2L3 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]
BITRATE_V1L1 = [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448]
BITRATE_V2L1 = [0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256]
BITRATE_V1L2 = [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384]
BITRATE_V2L2 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]

SAMPLE_RATE = {
    3: [44100, 48000, 32000],   # MPEG v1
    2: [22050, 24000, 16000],   # MPEG v2
    0: [11025, 12000, 8000],    # MPEG v2.5
}

SAMPLES_PER_FRAME = {
    3: {1: 384, 2: 1152, 3: 1152},   # v1: L1/L2/L3
    2: {1: 384, 2: 1152, 3: 576},    # v2
    0: {1: 384, 2: 1152, 3: 576},    # v2.5
}


def bitrate_table(version, layer):
    if layer == 1:
        return BITRATE_V1L1 if version == 3 else BITRATE_V2L1
    if layer == 2:
        return BITRATE_V1L2 if version == 3 else BITRATE_V2L2
    return BITRATE_V1L3 if version == 3 else BITRATE_V2L3


def id3v2_size(buf):
    if len(buf) >= 10 and buf[0:3] == b"ID3":
        # 同步安全整数，每字节 7 位
        s = buf[6:10]
        if all(b < 0x80 for b in s):
            return 10 + ((s[0] << 21) | (s[1] << 14) | (s[2] << 7) | s[3])
    return 0


def find_first_frame(buf, start):
    """返回第一个合法的 MPEG 帧同步位置；找不到返回 -1。"""
    n = len(buf)
    i = start
    limit = n - 4
    while i < limit:
        if buf[i] == 0xFF and (buf[i + 1] & 0xE0) == 0xE0:
            ok, _ = parse_header(buf, i)
            if ok:
                return i
        i += 1
    return -1


def parse_header(buf, off):
    """解析 4 字节 MPEG 头。返回 (是否合法, dict)。"""
    b = buf[off:off + 4]
    if len(b) < 4:
        return False, None
    if b[0] != 0xFF or (b[1] & 0xE0) != 0xE0:
        return False, None
    version_bits = (b[1] >> 3) & 0x03
    layer_bits = (b[1] >> 1) & 0x03
    crc = b[1] & 0x01
    if version_bits == 1 or layer_bits == 0:
        return False, None
    version = {0: 0, 2: 2, 3: 3}[version_bits]
    layer = 4 - layer_bits              # 1/2/3
    br_index = (b[2] >> 4) & 0x0F
    sr_index = (b[2] >> 2) & 0x03
    pad = (b[2] >> 1) & 0x01
    mode = (b[3] >> 6) & 0x03           # 3=mono
    if sr_index == 3 or br_index in (0, 15):
        return False, None
    br = bitrate_table(version, layer)[br_index]
    sr = SAMPLE_RATE[version][sr_index]
    if br == 0 or sr == 0:
        return False, None
    spf = SAMPLES_PER_FRAME[version][layer]
    frame_len = int(spf / 8 * br * 1000 / sr) + pad
    return True, dict(
        version=version, layer=layer, crc=crc, bitrate=br, sample_rate=sr,
        padding=pad, mode=mode, samples_per_frame=spf, frame_len=frame_len,
        mono=(mode == 3),
    )


def find_xing(buf, off, frame_len):
    """在首帧数据区里找 Xing / Info / VBRI。返回 (kind, frames, bytes)。"""
    # Xing/Info 位于侧信息之后：v1 stereo 32B、v1 mono 17B；v2 stereo 17B、v2 mono 9B
    region = buf[off + 4:off + min(frame_len, 200)]
    for tag in (b"Xing", b"Info", b"VBRI"):
        p = region.find(tag)
        if p >= 0:
            abs_p = off + 4 + p
            if tag == b"VBRI":
                if abs_p + 26 > len(buf):
                    continue
                frames = struct.unpack(">I", buf[abs_p + 14:abs_p + 18])[0]
                return tag.decode(), frames, 0
            flags = struct.unpack(">I", buf[abs_p + 4:abs_p + 8])[0]
            q = abs_p + 8
            frames = 0
            if flags & 0x01:
                frames = struct.unpack(">I", buf[q:q + 4])[0]
            return tag.decode(), frames, 0
    return None, 0, 0


def count_frames(buf, start, hdr):
    """CBR 无法靠头推断时逐帧扫描（慢但准）。"""
    n = len(buf)
    off = start
    cnt = 0
    while off + 4 <= n:
        ok, h = parse_header(buf, off)
        if not ok:
            # 同步丢失：线性找下一个 0xFF
            nxt = buf.find(b"\xff", off + 1)
            if nxt < 0:
                break
            off = nxt
            continue
        if off + h["frame_len"] > n:
            break
        cnt += 1
        off += h["frame_len"]
    return cnt


def scan_all(buf, start, hdr):
    """逐帧扫描全文件：返回 (帧数, 各帧比特率集合, 是否所有帧同规格)。
    同时检测采样率/声道是否中途变化。"""
    n = len(buf)
    off = start
    cnt = 0
    brs = {}
    srs = {}
    modes = {}
    while off + 4 <= n:
        ok, h = parse_header(buf, off)
        if not ok:
            nxt = buf.find(b"\xff", off + 1)
            if nxt < 0:
                break
            off = nxt
            continue
        if off + h["frame_len"] > n:
            break
        brs[h["bitrate"]] = brs.get(h["bitrate"], 0) + 1
        srs[h["sample_rate"]] = srs.get(h["sample_rate"], 0) + 1
        modes[h["mode"]] = modes.get(h["mode"], 0) + 1
        cnt += 1
        off += h["frame_len"]
    return cnt, brs, srs, modes


def probe(path):
    try:
        with open(path, "rb") as f:
            buf = f.read()
    except Exception as e:
        return dict(path=path, err="读取失败: %s" % e)
    tag = id3v2_size(buf)
    start = find_first_frame(buf, tag)
    if start < 0:
        return dict(path=path, err="找不到合法 MPEG 帧")
    ok, hdr = parse_header(buf, start)
    if not ok:
        return dict(path=path, err="首帧解析失败")

    xing, frames, _ = find_xing(buf, start, hdr["frame_len"])
    vbr = xing == "Xing"
    audio_bytes = len(buf) - start
    # 尾部 ID3v1 / APE
    tail = 0
    if buf[-128:-125] == b"TAG":
        tail = 128
    ape = buf.rfind(b"APETAGEX")
    if ape >= 0 and len(buf) - ape < 64:
        tail = max(tail, len(buf) - ape)
    audio_bytes -= tail

    # 逐帧扫描拿真实分布（比首帧头可靠得多）
    nframes, brs, srs, modes = scan_all(buf, start, hdr)
    if nframes:
        frames = nframes
    if frames == 0:
        frames = audio_bytes // hdr["frame_len"]
    duration = frames * hdr["samples_per_frame"] / hdr["sample_rate"]
    br_min = min(brs) if brs else hdr["bitrate"]
    br_max = max(brs) if brs else hdr["bitrate"]
    avg_kbps = (audio_bytes * 8 / 1000) / duration if duration > 0 else 0

    return dict(
        path=path, size=len(buf), id3=tag, tail=tail, start=start,
        hdr=hdr, xing=xing or "-", vbr=vbr, frames=frames,
        duration=duration, avg_kbps=avg_kbps,
        br_min=br_min, br_max=br_max, nbr=len(brs),
        srs=sorted(srs.keys()), modes=sorted(modes.keys()),
        scanned=nframes,
    )


def fmt(r):
    if "err" in r:
        return "  %-32s  %s" % (os.path.basename(r["path"]), r["err"])
    h = r["hdr"]
    ver = {3: "MPEG1", 2: "MPEG2", 0: "MPEG2.5"}[h["version"]]
    mode = {0: "stereo", 1: "joint", 2: "dual", 3: "mono"}[h["mode"]]
    mm = int(r["duration"] // 60)
    ss = r["duration"] - mm * 60
    brs = "%d-%d kbps" % (r["br_min"], r["br_max"]) if r["nbr"] > 1 else "%d kbps" % r["br_min"]
    kind = "VBR" if (r["nbr"] > 1 or r["vbr"]) else "CBR"
    flag = ""
    if not h["mono"] or h["sample_rate"] != 16000:
        flag = "  <== 需转码"
    return ("  %-30s %5.1fMB  %s L%d  %5dHz  %-6s  %-12s 平均%4.0fkbps  %-9s "
            "帧%6d  %d:%05.2f%s"
            % (os.path.basename(r["path"]), r["size"] / 1048576, ver, h["layer"],
               h["sample_rate"], mode, brs, r["avg_kbps"],
               kind + "/" + r["xing"],
               r["frames"], mm, ss, flag))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    files = []
    for a in args:
        if os.path.isdir(a):
            for n in sorted(os.listdir(a)):
                if n.lower().endswith(".mp3"):
                    files.append(os.path.join(a, n))
        else:
            files.append(a)
    tot_dur = 0.0
    tot_size = 0
    for p in files:
        r = probe(p)
        print(fmt(r))
        if "err" not in r:
            tot_dur += r["duration"]
            tot_size += r["size"]
    print("\n  合计 %d 个文件，%.1f MB，%.1f 分钟" %
          (len(files), tot_size / 1048576, tot_dur / 60))
    return 0


if __name__ == "__main__":
    sys.exit(main())
