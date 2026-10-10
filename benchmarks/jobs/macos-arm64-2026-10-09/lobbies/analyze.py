import csv, json, os, statistics as st, sys
def pct(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(len(v) * p / 100))]
rows = []
for n in sys.argv[2:] and list(map(int,sys.argv[2:])) or [8,12,16,24,32]:
    for m in ["off", "parallel"]:
        d = f"/tmp/jobsbench/lobby_{sys.argv[1] if len(sys.argv)>1 else "p"}{n}_{m}"
        if not os.path.exists(f"{d}/frames.csv"): continue
        r = [tuple(map(int, (x['time_ms'], x['wall_us'], x['thread_cpu_us'], x['process_cpu_us'])))
             for x in csv.DictReader(open(f"{d}/frames.csv"))]
        end = r[-1][0]; w = [x for x in r if x[0] >= end - 30000]
        secs = (w[-1][0] - w[0][0]) / 1000
        wall = [x[1] for x in w]; main = [x[2] for x in w]; proc = [x[3] for x in w]
        joined = sum(1 for l in open(f"{d}/game.log") if "joined" in l)
        ph = "-"
        if m == "parallel" and os.path.exists(f"{d}/jobs.json"):
            p = json.load(open(f"{d}/jobs.json"))['phases'][0]
            ph = f"{p['wall_p50_ns']/1e3:.0f}/{p['wall_p99_ns']/1e3:.0f}"
        rows.append((n, m, joined + 1, len(w) / secs, st.median(wall), pct(wall, 99), st.median(main), pct(main, 99),
                     st.median(proc), sum(main) / sum(wall) * 100, ph))
print("| players | jobs | in game | FPS (mean) | frame p50 µs | frame p99 µs | main CPU p50 µs | main CPU p99 µs | process CPU p50 µs | main busy % | phase p50/p99 µs |")
print("|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---|")
for r in rows:
    print("| %d | %s | %d | %.0f | %d | %d | %d | %d | %d | %.0f%% | %s |" % r)
