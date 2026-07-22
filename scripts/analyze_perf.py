#!/usr/bin/env python3
# Copyright 2026 The Ren'Py fork contributors
# SPDX-License-Identifier: MIT

"""
AES 性能损耗分析脚本

对比两份 perf_log.csv（非加密 vs 加密），输出分类别和分文件类型的性能报告。

用法:
    python analyze_perf.py perf_log_noaes.csv perf_log_aes.csv
"""

import csv
import sys
import os
from collections import defaultdict


def load_csv(path):
    """读取 perf_log.csv，返回记录列表。"""
    records = []
    with open(path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            records.append({
                "timestamp": float(row["timestamp"]),
                "category": row["category"],
                "name": row["name"],
                "size_bytes": int(row["size_bytes"]),
                "elapsed_ms": float(row["elapsed_ms"]),
            })
    return records


def percentile(sorted_values, p):
    """计算已排序列表的百分位数。"""
    if not sorted_values:
        return 0.0
    k = (len(sorted_values) - 1) * (p / 100.0)
    f = int(k)
    c = f + 1
    if c >= len(sorted_values):
        return sorted_values[f]
    return sorted_values[f] + (k - f) * (sorted_values[c] - sorted_values[f])


def get_extension(name):
    """从资源名提取扩展名。"""
    _, ext = os.path.splitext(name)
    return ext.lower() if ext else "(no ext)"


def size_bucket(size_bytes):
    """将文件大小归入桶。"""
    if size_bytes < 1024:
        return "<1KB"
    elif size_bytes < 10 * 1024:
        return "1-10KB"
    elif size_bytes < 100 * 1024:
        return "10-100KB"
    elif size_bytes < 1024 * 1024:
        return "100KB-1MB"
    else:
        return ">1MB"


def summarize_category(records, category):
    """汇总某个 category 的统计信息。"""
    items = [r for r in records if r["category"] == category]
    if not items:
        return None
    elapsed = sorted([r["elapsed_ms"] for r in items])
    total_bytes = sum(r["size_bytes"] for r in items)
    return {
        "count": len(items),
        "total_ms": sum(elapsed),
        "mean_ms": sum(elapsed) / len(elapsed),
        "p50_ms": percentile(elapsed, 50),
        "p95_ms": percentile(elapsed, 95),
        "p99_ms": percentile(elapsed, 99),
        "max_ms": elapsed[-1],
        "total_bytes": total_bytes,
    }


def summarize_by_ext(records, category):
    """按文件扩展名分组。"""
    groups = defaultdict(list)
    for r in records:
        if r["category"] == category:
            ext = get_extension(r["name"])
            groups[ext].append(r["elapsed_ms"])
    result = {}
    for ext, times in sorted(groups.items()):
        s = sorted(times)
        result[ext] = {
            "count": len(s),
            "total_ms": sum(s),
            "mean_ms": sum(s) / len(s),
            "p50_ms": percentile(s, 50),
        }
    return result


def summarize_by_size(records, category):
    """按文件大小分桶。"""
    groups = defaultdict(list)
    for r in records:
        if r["category"] == category:
            bucket = size_bucket(r["size_bytes"])
            groups[bucket].append(r["elapsed_ms"])
    result = {}
    bucket_order = ["<1KB", "1-10KB", "10-100KB", "100KB-1MB", ">1MB"]
    for bucket in bucket_order:
        if bucket in groups:
            s = sorted(groups[bucket])
            result[bucket] = {
                "count": len(s),
                "total_ms": sum(s),
                "mean_ms": sum(s) / len(s),
            }
    return result


def fmt_ms(ms):
    """格式化毫秒。"""
    if ms < 0.01:
        return f"{ms * 1000:.2f}μs"
    elif ms < 1.0:
        return f"{ms:.4f}ms"
    elif ms < 1000.0:
        return f"{ms:.2f}ms"
    else:
        return f"{ms / 1000:.3f}s"


def fmt_bytes(b):
    """格式化字节数。"""
    if b < 1024:
        return f"{b}B"
    elif b < 1024 * 1024:
        return f"{b / 1024:.1f}KB"
    elif b < 1024 * 1024 * 1024:
        return f"{b / (1024 * 1024):.1f}MB"
    else:
        return f"{b / (1024 * 1024 * 1024):.2f}GB"


def print_comparison(label, s1, s2):
    """打印两个汇总的对比。"""
    if s1 is None and s2 is None:
        return

    print(f"\n{'=' * 60}")
    print(f"  {label}")
    print(f"{'=' * 60}")

    header = f"{'Metric':<20} {'Non-AES':>15} {'AES':>15} {'Δ':>12}"
    print(header)
    print("-" * 62)

    metrics = ["count", "total_ms", "mean_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms"]
    metric_labels = ["Count", "Total", "Mean", "P50", "P95", "P99", "Max"]

    for metric, ml in zip(metrics, metric_labels):
        v1 = s1[metric] if s1 else 0
        v2 = s2[metric] if s2 else 0

        if metric == "count":
            f1 = str(int(v1))
            f2 = str(int(v2))
            delta = ""
        else:
            f1 = fmt_ms(v1)
            f2 = fmt_ms(v2)
            if v1 > 0:
                pct = ((v2 - v1) / v1) * 100
                delta = f"{pct:+.1f}%"
            elif v2 > 0:
                delta = "+∞"
            else:
                delta = "0%"

        print(f"{ml:<20} {f1:>15} {f2:>15} {delta:>12}")

    if s1 and s1.get("total_bytes"):
        print(f"\n  Total data: {fmt_bytes(s1['total_bytes'])} (non-AES), "
              f"{fmt_bytes(s2['total_bytes'] if s2 else 0)} (AES)")


def print_ext_comparison(label, ext1, ext2):
    """打印按扩展名的对比。"""
    all_exts = sorted(set(list(ext1.keys()) + list(ext2.keys())))
    if not all_exts:
        return

    print(f"\n{'=' * 60}")
    print(f"  {label}")
    print(f"{'=' * 60}")
    print(f"{'Extension':<12} {'Count':>7} {'Non-AES Mean':>14} {'AES Mean':>14} {'Δ':>10}")
    print("-" * 57)

    for ext in all_exts:
        e1 = ext1.get(ext)
        e2 = ext2.get(ext)
        count = (e1 or e2)["count"]
        m1 = e1["mean_ms"] if e1 else 0
        m2 = e2["mean_ms"] if e2 else 0
        if m1 > 0:
            delta = f"{((m2 - m1) / m1) * 100:+.1f}%"
        elif m2 > 0:
            delta = "N/A"
        else:
            delta = "0%"
        print(f"{ext:<12} {count:>7} {fmt_ms(m1):>14} {fmt_ms(m2):>14} {delta:>10}")


def print_size_comparison(label, sz1, sz2):
    """打印按文件大小分桶的对比。"""
    all_buckets = ["<1KB", "1-10KB", "10-100KB", "100KB-1MB", ">1MB"]
    active = [b for b in all_buckets if b in sz1 or b in sz2]
    if not active:
        return

    print(f"\n{'=' * 60}")
    print(f"  {label}")
    print(f"{'=' * 60}")
    print(f"{'Size Bucket':<14} {'Count':>7} {'Non-AES Mean':>14} {'AES Mean':>14} {'Δ':>10}")
    print("-" * 59)

    for bucket in active:
        s1 = sz1.get(bucket)
        s2 = sz2.get(bucket)
        count = (s1 or s2)["count"]
        m1 = s1["mean_ms"] if s1 else 0
        m2 = s2["mean_ms"] if s2 else 0
        if m1 > 0:
            delta = f"{((m2 - m1) / m1) * 100:+.1f}%"
        elif m2 > 0:
            delta = "N/A"
        else:
            delta = "0%"
        print(f"{bucket:<14} {count:>7} {fmt_ms(m1):>14} {fmt_ms(m2):>14} {delta:>10}")


def main():
    if len(sys.argv) != 3:
        print(f"用法: python {sys.argv[0]} <non-aes-csv> <aes-csv>")
        print(f"示例: python {sys.argv[0]} perf_log_noaes.csv perf_log_aes.csv")
        sys.exit(1)

    path_noaes = sys.argv[1]
    path_aes = sys.argv[2]

    print(f"Loading: {path_noaes} (non-AES)")
    recs_noaes = load_csv(path_noaes)
    print(f"  → {len(recs_noaes)} records")

    print(f"Loading: {path_aes} (AES)")
    recs_aes = load_csv(path_aes)
    print(f"  → {len(recs_aes)} records")

    # ── 1. Index Archives ──
    s1 = summarize_category(recs_noaes, "index_archives")
    s2 = summarize_category(recs_aes, "index_archives")
    print_comparison("Index Archives (one-time startup cost)", s1, s2)

    # ── 2. Load Archive (file open) ──
    s1 = summarize_category(recs_noaes, "load_archive")
    s2 = summarize_category(recs_aes, "load_archive")
    print_comparison("File Loading (load_from_archive)", s1, s2)

    # ── 3. By extension ──
    ext1 = summarize_by_ext(recs_noaes, "load_archive")
    ext2 = summarize_by_ext(recs_aes, "load_archive")
    print_ext_comparison("File Loading by Extension", ext1, ext2)

    # ── 4. By size ──
    sz1 = summarize_by_size(recs_noaes, "load_archive")
    sz2 = summarize_by_size(recs_aes, "load_archive")
    print_size_comparison("File Loading by Size Bucket", sz1, sz2)

    s1 = summarize_category(recs_noaes, "readinto")
    s2 = summarize_category(recs_aes, "readinto")
    if s2:
        print_comparison("AES Decrypt Hot Path (readinto)", s1, s2)

    # ── 6. Overall Summary ──
    noaes_total = sum(r["elapsed_ms"] for r in recs_noaes if r["category"] == "load_archive")
    aes_total = sum(r["elapsed_ms"] for r in recs_aes if r["category"] == "load_archive")

    print(f"\n{'=' * 60}")
    print("  Overall AES Overhead Summary")
    print(f"{'=' * 60}")
    print(f"  Non-AES total load time:  {fmt_ms(noaes_total)}")
    print(f"  AES total load time:      {fmt_ms(aes_total)}")
    if noaes_total > 0:
        overhead = ((aes_total - noaes_total) / noaes_total) * 100
        print(f"  AES overhead:             {overhead:+.2f}%")
        print(f"  AES extra time:           {fmt_ms(aes_total - noaes_total)}")
    print()


if __name__ == "__main__":
    main()
