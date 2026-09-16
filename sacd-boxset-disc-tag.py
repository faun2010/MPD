#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sacd-boxset-disc-tag.py — 给一套多碟 SACD ISO 套装（例如 9 张碟的合集）
补写 Master TOC 里的 album_set_size / album_sequence_number 字段，
让 mpd(-sacd) 的 sacdiso 解码器能正确打出 Disc 标签，从而在
Media Library / Songs 视图里按碟正确分组、排序，而不是把每张碟的
第 1 轨、第 2 轨……互相混在一起。

原理（对照本仓库源码 src/lib/sacdiso/sacd_disc.cpp:205 及
src/lib/sacdiso/scarletbook.h 的 master_toc_t 结构体）：

    每个 SACD ISO 文件里，偏移量 510*2048 (LSN 模式) 或
    510*2064+12 (PSN 模式) 处是一段以 "SACDMTOC" 开头的
    Master TOC 结构，其中：
        offset +16, 2 字节, 大端: album_set_size       (套装总碟数)
        offset +18, 2 字节, 大端: album_sequence_number (本碟是第几张)

    厂牌刻录时如果没有把这批碟标记成"一套"，这两个字段就是
    album_set_size=1, album_sequence_number=1（或类似的"各自独立"
    的值），mpd 因为判断条件是 `album_set_size > 1` 才打 Disc 标签，
    所以永远不会给这些碟分组。

    本脚本只改这 4 个字节，不碰任何音频数据、不改文件大小，
    并且写之前会自动备份原始字节，可以用 --restore 完整还原。

本脚本对任何"一套 N 张 SACD ISO 合集"都通用，不止 Kempe 这一套。

========================= 用法 =========================

  1) 预览模式（默认，不写盘，只看当前状态和将要改成什么样）:

       python3 sacd-boxset-disc-tag.py "/path/to/某套9张SACD的文件夹"

  2) 确认没问题后，正式写入:

       python3 sacd-boxset-disc-tag.py "/path/to/文件夹" --apply

  3) 如果排错了 / 想撤销，恢复成原始状态:

       python3 sacd-boxset-disc-tag.py "/path/to/文件夹" --restore

  4) 如果自动按文件名排序的碟序不对（比如文件名里数字对不上实际碟号），
     可以直接按你想要的顺序手动列出文件（而不是传目录）:

       python3 sacd-boxset-disc-tag.py --apply \\
           "/path/CD1.iso" "/path/CD2.iso" "/path/CD3.iso"

  5) 只想看当前 9 个文件现在的 Master TOC 状态，不做任何改动:

       python3 sacd-boxset-disc-tag.py "/path/to/文件夹" --check-only

改完之后记得让 mpd 重新扫库（mpc update），再看 Disc 标签是否出现:

       mpc -p 6600 update
       mpc -p 6600 find album "专辑名" -f "%disc%/%track% - %title%"
"""

import argparse
import json
import os
import re
import struct
import sys
import time

SACD_LSN_SIZE = 2048
SACD_PSN_SIZE = 2064
START_OF_MASTER_TOC = 510
MAGIC = b"SACDMTOC"

# Master TOC 结构里我们关心/备份的这一段：
#   id[8] + version(2) + reserved01[6] + album_set_size(2) + album_sequence_number(2)
HEADER_LEN = 20
SET_SIZE_OFF = 16   # 相对 Master TOC 起始的偏移
SEQ_NUM_OFF = 18

BACKUP_DIRNAME = ".sacd-disctag-backup"


def natural_sort_key(path):
    """按文件名里出现的数字做自然排序，而不是纯字典序（避免 10 排到 2 前面）。"""
    name = os.path.basename(path)
    parts = re.split(r"(\d+)", name)
    return [int(p) if p.isdigit() else p.lower() for p in parts]


def find_iso_files(directory):
    files = [
        os.path.join(directory, f)
        for f in os.listdir(directory)
        if f.lower().endswith(".iso")
    ]
    files.sort(key=natural_sort_key)
    return files


def locate_master_toc(fh, filesize):
    """返回 (offset, mode) 或 None。mode 是 'lsn' 或 'psn'，仅用于展示。"""
    lsn_off = START_OF_MASTER_TOC * SACD_LSN_SIZE
    if lsn_off + HEADER_LEN <= filesize:
        fh.seek(lsn_off)
        if fh.read(8) == MAGIC:
            return lsn_off, "lsn"

    psn_off = START_OF_MASTER_TOC * SACD_PSN_SIZE + 12
    if psn_off + HEADER_LEN <= filesize:
        fh.seek(psn_off)
        if fh.read(8) == MAGIC:
            return psn_off, "psn"

    return None


def read_header(fh, offset):
    fh.seek(offset)
    blk = fh.read(HEADER_LEN)
    set_size, seq_num = struct.unpack(">HH", blk[SET_SIZE_OFF:SET_SIZE_OFF + 4])
    return blk, set_size, seq_num


def backup_path_for(iso_path):
    d = os.path.join(os.path.dirname(iso_path), BACKUP_DIRNAME)
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, os.path.basename(iso_path) + ".json")


def do_check_or_apply(files, apply, total_override, start_override, verbose):
    total = total_override or len(files)
    start = start_override if start_override is not None else 1

    print(f"共 {len(files)} 个 ISO 文件，按以下顺序处理（目标 album_set_size={total}）：\n")

    problems = 0
    changed = 0
    for idx, path in enumerate(files, start=start):
        base = os.path.basename(path)
        try:
            filesize = os.path.getsize(path)
            with open(path, "r+b" if apply else "rb") as fh:
                loc = locate_master_toc(fh, filesize)
                if loc is None:
                    print(f"  [跳过/异常] {base}\n"
                          f"      在预期偏移量找不到 'SACDMTOC' 标记，这个文件可能不是"
                          f"标准 SACD ISO，或者格式跟本脚本假设的不一样。已跳过，未做任何修改。")
                    problems += 1
                    continue

                offset, mode = loc
                _, cur_set_size, cur_seq = read_header(fh, offset)

                target_set_size = total
                target_seq = idx

                status = "无需改动" if (cur_set_size == target_set_size and cur_seq == target_seq) else "将会改动"
                print(f"  [{status}] {base}")
                if verbose:
                    print(f"      模式={mode} 偏移量={offset}")
                print(f"      album_set_size:       {cur_set_size} -> {target_set_size}")
                print(f"      album_sequence_number: {cur_seq} -> {target_seq}")

                if cur_set_size == target_set_size and cur_seq == target_seq:
                    continue

                if not apply:
                    continue

                # 写之前先备份原始 20 字节
                fh.seek(offset)
                original_header = fh.read(HEADER_LEN)
                bpath = backup_path_for(path)
                if not os.path.exists(bpath):
                    with open(bpath, "w", encoding="utf-8") as bf:
                        json.dump({
                            "iso_file": os.path.abspath(path),
                            "offset": offset,
                            "mode": mode,
                            "original_header_hex": original_header.hex(),
                            "backed_up_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                        }, bf, ensure_ascii=False, indent=2)
                else:
                    print(f"      （已存在备份 {bpath}，不覆盖，沿用第一次的原始值）")

                new_bytes = struct.pack(">HH", target_set_size, target_seq)
                fh.seek(offset + SET_SIZE_OFF)
                fh.write(new_bytes)
                fh.flush()

                # 回读校验
                fh.seek(offset)
                _, verify_set_size, verify_seq = read_header(fh, offset)
                if verify_set_size == target_set_size and verify_seq == target_seq:
                    print(f"      已写入并校验通过。备份: {bpath}")
                    changed += 1
                else:
                    print(f"      [警告] 写入后回读的值对不上，请手动检查该文件！")
                    problems += 1

        except OSError as e:
            print(f"  [错误] {base}: 打开/读写文件失败: {e}")
            problems += 1
        print()

    print("——" * 20)
    if not apply:
        print("这是预览模式，以上文件都没有被修改。确认没问题后加 --apply 正式写入。")
    else:
        print(f"完成。实际修改 {changed} 个文件，异常/跳过 {problems} 个。")
    return problems


def do_restore(files, verbose):
    restored = 0
    problems = 0
    for path in files:
        base = os.path.basename(path)
        bpath = backup_path_for(path)
        if not os.path.exists(bpath):
            print(f"  [跳过] {base}: 没有找到备份文件（{bpath}），可能从未被本脚本改过。")
            continue
        with open(bpath, "r", encoding="utf-8") as bf:
            meta = json.load(bf)
        offset = meta["offset"]
        original_header = bytes.fromhex(meta["original_header_hex"])
        try:
            with open(path, "r+b") as fh:
                fh.seek(offset)
                fh.write(original_header)
                fh.flush()
                fh.seek(offset)
                if fh.read(HEADER_LEN) == original_header:
                    print(f"  [已恢复] {base} -> 恢复到备份于 {meta.get('backed_up_at', '?')} 的原始值")
                    restored += 1
                else:
                    print(f"  [警告] {base}: 恢复后回读校验不一致，请手动检查！")
                    problems += 1
        except OSError as e:
            print(f"  [错误] {base}: {e}")
            problems += 1
    print("——" * 20)
    print(f"恢复完成。成功 {restored} 个，异常 {problems} 个。备份文件默认保留，不会自动删除。")
    return problems


def main():
    parser = argparse.ArgumentParser(
        description="给一套多碟 SACD ISO 合集补写 Disc（碟号）标签信息，"
                     "解决 mpd 把多张碟的音轨混在一起排序/分组的问题。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "targets", nargs="+",
        help="一个目录（自动按文件名数字顺序找出所有 .iso 按碟序处理），"
             "或者直接按你想要的顺序列出多个 .iso 文件路径。",
    )
    parser.add_argument(
        "--apply", action="store_true",
        help="真正写入修改（不加这个参数只是预览，不会改任何文件）。",
    )
    parser.add_argument(
        "--restore", action="store_true",
        help="撤销之前本脚本做的修改，恢复成原始字节（需要之前 --apply 时生成的备份文件还在）。",
    )
    parser.add_argument(
        "--check-only", action="store_true",
        help="只读取并显示当前 Master TOC 状态，等同于不加 --apply 的预览模式（别名，便于记忆）。",
    )
    parser.add_argument(
        "--total", type=int, default=None,
        help="手动指定套装总碟数（album_set_size）。默认等于识别到的文件数量。",
    )
    parser.add_argument(
        "--start", type=int, default=None,
        help="第一个文件对应的碟号（album_sequence_number）。默认从 1 开始。",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="显示更详细的信息（比如每个文件用的是 LSN 还是 PSN 模式、具体偏移量）。",
    )
    args = parser.parse_args()

    if args.apply and args.restore:
        parser.error("--apply 和 --restore 不能同时使用。")

    # 展开 targets：如果只给了一个目录，就在目录里按文件名数字顺序找 .iso；
    # 如果给的是多个文件路径，就按你给的顺序原样使用。
    if len(args.targets) == 1 and os.path.isdir(args.targets[0]):
        directory = args.targets[0]
        files = find_iso_files(directory)
        if not files:
            print(f"目录里没有找到 .iso 文件: {directory}")
            sys.exit(1)
    else:
        files = args.targets
        for f in files:
            if not os.path.isfile(f):
                print(f"文件不存在: {f}")
                sys.exit(1)

    if args.restore:
        problems = do_restore(files, args.verbose)
    else:
        apply = args.apply and not args.check_only
        problems = do_check_or_apply(files, apply, args.total, args.start, args.verbose)

    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
