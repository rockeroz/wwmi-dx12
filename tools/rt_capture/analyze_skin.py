"""Offline: the UE GPU skin cache sections recorded by the RT observe module (RTSKIN lines + skin_*.bin).

For every (frame, ring) it prints each section: vertex start/count from $Globals (root CBV p3: [?, SkinCacheStart,
InputStreamStart, NumVertices, InputWeightStart, InputWeightStride, ...]), the bone palette view (typed float4
view into the frame's upload buffer: offset, bones = elements / 3, how many look affine), and, across frames, how
many bones moved. A ring VA is reused by the skin cache pool, so a character is recognised by its section layout
(the sequence of vertex counts), not by the VA.

    python tools/rt_capture/analyze_skin.py rt_observe/rt_observe.log [--bins rt_observe/bins] [--layout 5927,5606,...]
"""
import argparse
import collections
import os
import re
import struct

HEAD = re.compile(r'\[f(\d+)\] RTSKIN cs=(\w+) ring=(\w+) mod=(-?\d+) section=(\d+) groups=(\d+)')
GLOB = re.compile(r'cbv p3 va=\w+ size=\d+ read=\d+ words=((?:[0-9a-f]{8} ){8})')
PAL = re.compile(r'tbl p0 s0 va=(\w+) res_bytes=\d+ view type=\d+ fmt=(\d+) raw_off=(\d+) raw_size=(\d+) elem=(\d+) -> off=(\d+) bytes=(\d+).*?bone_run=(\d+)@(\d+)')


def parse(log):
    recs = collections.OrderedDict()
    cur = None
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = HEAD.search(line)
            if m:
                cur = (int(m.group(1)), m.group(3), int(m.group(5)))
                recs[cur] = dict(cs=m.group(2), mod=int(m.group(4)), groups=int(m.group(6)))
                continue
            if cur is None:
                continue
            m = GLOB.search(line)
            if m:
                recs[cur]['glob'] = [int(x, 16) for x in m.group(1).split()]
            m = PAL.search(line)
            if m:
                recs[cur]['pal'] = dict(va=m.group(1), fmt=int(m.group(2)), off=int(m.group(6)), bytes=int(m.group(7)),
                                        bones=int(m.group(4)) // 3, affine=int(m.group(8)))
    return recs


def layouts(recs):
    by = collections.defaultdict(list)
    for (frame, ring, sec), r in recs.items():
        g = r.get('glob')
        if g:
            by[(frame, ring)].append((sec, g[1], g[3], r.get('pal', {}).get('bones', 0), r.get('pal', {}).get('affine', 0)))
    return {k: sorted(v) for k, v in by.items()}


def palettes(bins):
    out = collections.defaultdict(dict)
    if not bins or not os.path.isdir(bins):
        return out
    for f in os.listdir(bins):
        m = re.match(r'skin_f(\d+)_cs\w+_r(\w+)_s(\d+)_tbl0_0\.bin$', f)
        if m:
            with open(os.path.join(bins, f), 'rb') as fh:
                out[(m.group(2), int(m.group(3)))][int(m.group(1))] = fh.read()
    return out


def moved(a, b):
    n = min(len(a), len(b)) // 48
    cnt = 0
    for i in range(n):
        x = struct.unpack_from('12f', a, i * 48)
        y = struct.unpack_from('12f', b, i * 48)
        cnt += max(abs(p - q) for p, q in zip(x, y)) > 1e-4
    return cnt, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--bins')
    ap.add_argument('--layout', help='comma-separated vertex counts of the character to keep (e.g. the body)')
    a = ap.parse_args()
    recs = parse(a.log)
    lay = layouts(recs)
    want = [int(x) for x in a.layout.split(',')] if a.layout else None
    signatures = collections.Counter(tuple(c for _, _, c, _, _ in v) for v in lay.values())
    print('section layouts seen (vertex counts per section) x occurrences:')
    for sig, n in signatures.most_common(12):
        print(f'  {n:4d}  {len(sig)} sections  total {sum(sig)}  {list(sig)}')
    pals = palettes(a.bins)
    for (frame, ring), secs in lay.items():
        counts = [c for _, _, c, _, _ in secs]
        if want and counts != want:
            continue
        print(f'f{frame} ring {ring}: ' + '  '.join(f's{s}[{st}+{c} bones {b}/{af}]' for s, st, c, b, af in secs))
        if want:
            break
    if pals:
        print('palette motion between the first and last recorded frame of each (ring, section):')
        for (ring, sec), fr in sorted(pals.items()):
            fs = sorted(fr)
            if len(fs) >= 2:
                m, n = moved(fr[fs[0]], fr[fs[-1]])
                print(f'  ring {ring} s{sec}: f{fs[0]}..f{fs[-1]} {m}/{n} bones moved')


if __name__ == '__main__':
    main()
