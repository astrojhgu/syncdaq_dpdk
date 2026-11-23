#!/usr/bin/env python3
"""校验 pktloss 的 CSV 末行快照是否等于预期。

用法: check_csv.py <csv> <chan>:<recv>,<lost>,<gaps>,<maxgap>,<rounds>,<dup>,<abn>,<bad> ...
"""

import sys


def main():
    csv_path = sys.argv[1]
    expect = {}

    for spec in sys.argv[2:]:
        chan, nums = spec.split(":")
        expect[int(chan)] = [int(x) for x in nums.split(",")]

    last = {}
    with open(csv_path, encoding="utf-8") as f:
        header = f.readline().strip().split(",")
        idx = {name: i for i, name in enumerate(header)}
        for line in f:
            parts = line.strip().split(",")
            if len(parts) != len(header):
                continue
            chan = int(parts[idx["chan"]])
            last[chan] = [int(parts[idx[k]]) for k in
                          ("recv", "lost", "gaps", "maxgap", "rounds", "dup",
                           "abn", "bad")]

    bad = 0
    for chan, want in expect.items():
        got = last.get(chan)
        if got != want:
            print(f"  FAIL ch{chan}: got {got}, want {want}")
            bad += 1
        else:
            print(f"  OK   ch{chan}: {got}")

    if set(last) != set(expect):
        print(f"  FAIL 通道集合不一致: got {sorted(last)}, want {sorted(expect)}")
        bad += 1

    print("CSV 校验通过" if bad == 0 else "CSV 校验失败")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
