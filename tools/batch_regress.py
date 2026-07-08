"""Batch regression harness: unpack every .pac in the game, and parse every
contained .model/.material/.skl.pxd. Proves the parsers are robust across the
whole game, not just amy. Writes a summary + failures list.
"""
import os, sys, traceback, time
from collections import Counter
import pac as pacmod
import model as modelmod
import material as matmod
import skeleton as skelmod

ROOT = r"E:\Games\steamapps\common\SonicFrontiers\image\x64\raw"


def main(limit=None, only=None):
    t0 = time.time()
    pacs = []
    for dp, _, fs in os.walk(ROOT):
        for f in fs:
            if f.lower().endswith(".pac"):
                pacs.append(os.path.join(dp, f))
    pacs.sort()
    if only:
        pacs = [p for p in pacs if only in p]
    if limit:
        pacs = pacs[:limit]

    stat = Counter()
    fails = []
    ext_counts = Counter()
    n_models = n_mats = n_skels = 0

    for pi, p in enumerate(pacs):
        rel = os.path.relpath(p, ROOT)
        try:
            ents = pacmod.unpack(p)
            stat["pac_ok"] += 1
        except Exception as e:
            stat["pac_fail"] += 1
            fails.append((rel, "PAC", repr(e)))
            continue
        for e in ents:
            ext_counts[e.ext] += 1
            try:
                if e.ext == "model" or e.ext == "terrain-model":
                    mdl = modelmod.parse_model(e.data)
                    iss = modelmod.sanity(mdl)
                    n_models += 1
                    if iss:
                        stat["model_sanity"] += 1
                        fails.append((rel + "/" + e.name, "MODEL_SANITY", str(iss[:2])))
                    elif not mdl.meshes:
                        stat["model_empty"] += 1
                    else:
                        stat["model_ok"] += 1
                elif e.ext == "material":
                    matmod.parse_material(e.data)
                    n_mats += 1
                    stat["mat_ok"] += 1
                elif e.ext == "skl.pxd":
                    skelmod.parse_skeleton(e.data)
                    n_skels += 1
                    stat["skel_ok"] += 1
            except Exception as ex:
                stat[e.ext + "_fail"] += 1
                if len(fails) < 400:
                    fails.append((rel + "/" + e.name, e.ext.upper(), repr(ex)))
        if pi % 50 == 0:
            print(f"[{pi}/{len(pacs)}] {rel}  ok_models={stat['model_ok']} fails={len(fails)}",
                  flush=True)

    dt = time.time() - t0
    print("\n===== SUMMARY =====")
    print(f"pacs: {len(pacs)}  time: {dt:.1f}s")
    print("stats:", dict(stat))
    print(f"parsed: models={n_models} materials={n_mats} skeletons={n_skels}")
    print("top exts:", dict(ext_counts.most_common(15)))
    print(f"\nFAILURES ({len(fails)}):")
    fk = Counter(f[1] for f in fails)
    print("  by kind:", dict(fk))
    for f in fails[:40]:
        print(f"  {f[1]:14} {f[0][:70]:72} {f[2][:80]}")
    # write full failures
    outp = os.path.join(os.path.dirname(__file__), "..", "docs", "regression_report.txt")
    with open(outp, "w", encoding="utf-8") as fh:
        fh.write(f"pacs={len(pacs)} time={dt:.1f}s\nstats={dict(stat)}\n")
        fh.write(f"models={n_models} mats={n_mats} skels={n_skels}\n")
        fh.write(f"exts={dict(ext_counts.most_common(40))}\n\nFAILURES ({len(fails)}):\n")
        for f in fails:
            fh.write(f"{f[1]}\t{f[0]}\t{f[2]}\n")
    print(f"\nwrote {outp}")


if __name__ == "__main__":
    limit = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1] != "all" else None
    only = sys.argv[2] if len(sys.argv) > 2 else None
    main(limit, only)
