# Consumer V2 UDS / 6510 socat eBPF Bottleneck Analysis

## Scope

- Target: `tide_worker` metrics UDS exposed through manual `socat` TCP6 port `6510`.
- Method: UDS metrics plus eBPF/bpftrace only. Do not use `perf` for this procedure.
- Goal: explain why Kafka lag is still growing and define the next validation/action plan.

## Target Mapping

```text
+--------------------------------------------------+
| browser / curl observes http://host:6510/        |
+-------------------------+------------------------+
                          |
                          | manual TCP6 -> UDS bridge
                          v
+--------------------------------------------------+
| socat TCP6-LISTEN:6510                           |
| bind=[::],reuseaddr,fork                         |
| UNIX-CONNECT:/var/run/tide/worker_6510.sock      |
+-------------------------+------------------------+
                          |
                          v
+--------------------------------------------------+
| pid: 319594 tide_worker                          |
| UDS: /var/run/tide/worker_6510.sock              |
+--------------------------------------------------+
```

Discovery commands:

```bash
ss -lxp | grep worker
ss -tlnp | grep 6510
pgrep -a tide_worker
```

## Procedure

### 1. UDS Snapshot

Fetch one snapshot:

```bash
curl -s --unix-socket /var/run/tide/worker_6510.sock http://localhost/json
```

Extract high-level fields:

```bash
curl -s --unix-socket /var/run/tide/worker_6510.sock http://localhost/json | python3 - <<'PY'
import json, sys
data = json.load(sys.stdin)
s = data["summary"]
c = data["consumers"][0]
for k in [
    "productionRecordsPerSec",
    "ackedOffsetRecordsPerSec",
    "brokerCommittedRecordsPerSec",
    "currentThroughputMsgsPerSec",
    "totalBrokerLag",
    "totalAckedLag",
    "totalCommitGap",
    "totalCommitCalls",
    "totalPeriodicCommitCalls",
    "totalCommitFailures",
    "ringLiveCount",
    "ringCapacity",
    "readyPartitionCount",
    "workerQueueDepth",
    "pausedPartitionCount",
    "directDispatchEnabled",
    "workerCount",
]:
    print(k, c.get(k, s.get(k)))
PY
```

Take a delta sample:

```bash
python3 - <<'PY'
import json, subprocess, time
sock = "/var/run/tide/worker_6510.sock"

def sample():
    raw = subprocess.check_output(
        ["curl", "-s", "--unix-socket", sock, "http://localhost/json"]
    )
    return json.loads(raw)

s1 = sample()
time.sleep(10)
s2 = sample()
c1 = s1["consumers"][0]
c2 = s2["consumers"][0]
secs = 10.0

for f in [
    "totalPolledRecords",
    "totalAckedRecords",
    "totalBrokerLag",
    "totalAckedLag",
    "totalCommitGap",
    "totalCommitCalls",
    "totalPeriodicCommitCalls",
    "totalCommitFailures",
    "totalReadRecords",
    "totalDispatchedRecords",
]:
    print(f, c1.get(f), "->", c2.get(f), "rate/s", (c2.get(f, 0) - c1.get(f, 0)) / secs)

p1 = c1.get("directWorkerPushedRecords", [])
p2 = c2.get("directWorkerPushedRecords", [])
r1 = c1.get("directWorkerReadRecords", [])
r2 = c2.get("directWorkerReadRecords", [])
a1 = c1.get("directWorkerAckedRecords", [])
a2 = c2.get("directWorkerAckedRecords", [])
rates = []
for i in range(min(len(p1), len(p2))):
    pr = max(0, p2[i] - p1[i]) / secs
    rr = max(0, r2[i] - r1[i]) / secs
    ar = max(0, a2[i] - a1[i]) / secs
    if pr or rr or ar:
        rates.append((i, pr, rr, ar))

print("active workers", len(rates))
if rates:
    pushed = [x[1] for x in rates]
    avg = sum(pushed) / len(pushed)
    print("total pushed/s", sum(pushed))
    print("avg/min/max pushed/s", avg, min(pushed), max(pushed))
    print("skew pct", (max(pushed) - min(pushed)) / avg * 100 if avg else 0)
    print("top", sorted(rates, key=lambda x: x[1], reverse=True)[:8])
    print("bottom", sorted(rates, key=lambda x: x[1])[:8])
PY
```

### 2. eBPF CPU Thread Distribution

Use comm-level profiling first. It avoids expensive symbol/unwind work on a 1299-thread process.

```bash
timeout 15 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[comm] = count(); }'
```

Avoid starting with full user stacks:

```bash
timeout 20 bpftrace -e 'profile:hz:49 /pid == 319594/ { @[ustack(8)] = count(); }'
```

On this process, user stack unwinding mostly returned `@[]`, which means it is not useful enough for diagnosis. This can happen when frame pointers are unavailable, mappings are huge, or symbol/unwind metadata is not usable. Use comm-level sampling and kernel/syscall probes instead.

### 3. eBPF Scheduling Pressure

Count context switches:

```bash
timeout 15 bpftrace -e 'tracepoint:sched:sched_switch /pid == 319594/ { @[comm] = count(); }'
```

Count uninterruptible-sleep switches:

```bash
timeout 12 bpftrace -e \
'tracepoint:sched:sched_switch /pid == 319594 && (args->prev_state & 2)/ { @[args->prev_comm] = count(); }'
```

### 4. eBPF Syscall Pressure

Futex:

```bash
timeout 12 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /pid == 319594/ { @[comm] = count(); }'
```

Read/write syscall entry counts:

```bash
timeout 12 bpftrace -e \
'tracepoint:syscalls:sys_enter_read /pid == 319594/ { @[comm] = count(); }
 tracepoint:syscalls:sys_enter_write /pid == 319594/ { @[comm] = count(); }
 tracepoint:syscalls:sys_enter_pwrite64 /pid == 319594/ { @[comm] = count(); }'
```

VFS write bytes:

```bash
timeout 8 bpftrace -e \
'kprobe:vfs_write /pid == 319594/ { @bytes[comm] = sum(arg2); @cnt[comm] = count(); }' \
  | grep '@bytes' | tail -80
```

## Current Evidence

### UDS Summary

Original 10s UDS delta before the worker update:

```text
totalPolledRecords:      +4,009,593  => 400,959/s
totalAckedRecords:       +3,866,267  => 386,627/s
totalBrokerLag:          +8,137,166  => 813,717/s
totalAckedLag:           +2,248,315  => 224,832/s
totalCommitGap:          +5,888,851  => 588,885/s
totalCommitCalls:        +0
totalPeriodicCommitCalls:+0
totalCommitFailures:     +0
totalReadRecords:        +3,872,667  => 387,267/s
totalDispatchedRecords:  +3,867,291  => 386,729/s
```

Instant UDS rates at the second sample:

```text
productionRecordsPerSec:       553,850/s
ackedOffsetRecordsPerSec:      400,820/s
brokerCommittedRecordsPerSec:  0/s
currentThroughputMsgsPerSec:   348,886/s
workerCount:                   48
ringLiveCount/ringCapacity:    0 / 262144
pausedPartitionCount:          0
readyPartitionCount:           0
dispatcherQueueDepth:          0
workerQueueDepth:              0
directDispatchEnabled:         true
```

Direct worker distribution over the same 10s window:

```text
active direct workers: 48
pushed/s total:        400,959/s
pushed/s avg:          8,353/s
pushed/s min:          3,196/s
pushed/s max:          14,122/s
pushed/s skew:         130.8%
```

Top pushed workers:

```text
worker 35: 14,122/s
worker 11: 13,711/s
worker 37: 13,015/s
worker 23: 12,900/s
worker 21: 12,859/s
worker 17: 11,820/s
worker 10: 11,687/s
worker 33: 10,978/s
```

Bottom pushed workers:

```text
worker 8:  3,196/s
worker 27: 3,338/s
worker 1:  4,640/s
worker 4:  4,718/s
worker 39: 4,727/s
worker 31: 4,932/s
worker 42: 5,033/s
worker 2:  5,077/s
```

### Post-Update UDS Check

The worker was updated and the live process changed:

```text
pid:    1943394
UDS:    /var/run/tide/worker_6510.sock
HTML:   http://localhost/ over the UDS socket
JSON:   http://localhost/json over the UDS socket
```

Verification commands:

```bash
curl -s --unix-socket /var/run/tide/worker_6510.sock http://localhost/ > /tmp/uds_6510.html
grep -nE 'Direct Worker Load Balance|directWorkerBody|cardThroughput|buildDirectWorkerQPS|direct\\)' /tmp/uds_6510.html

curl -s --unix-socket /var/run/tide/worker_6510.sock http://localhost/json > /tmp/uds_6510.json
python3 - <<'PY'
import json
data = json.load(open("/tmp/uds_6510.json"))
s = data["summary"]
c = data["consumers"][0]
print("directDispatchEnabled", c["directDispatchEnabled"])
print("workerCount", c["workerCount"])
print("direct arrays", len(c["directWorkerPushedRecords"]), len(c["directWorkerReadRecords"]), len(c["directWorkerAckedRecords"]))
print("throughput", c["currentThroughputMsgsPerSec"], c["currentThroughputBytesPerSec"])
print("commit", c["brokerCommittedRecordsPerSec"], c["totalPeriodicCommitCalls"], c["totalCommitGap"])
PY
```

Live HTML result:

```text
Direct Worker Load Balance section exists in /
directWorkerBody exists in /
buildDirectWorkerQPS exists in /
cardThroughput exists in /
```

Important UI note:

```text
The initial server-rendered HTML still says "waiting for direct worker samples".
Worker QPS is browser-side delta data, so it appears only after /json is fetched twice.
If it stays "waiting", the browser did not run JS or could not fetch /json.
```

Current 5s UDS delta after the update:

```text
totalPolledRecords:      +2,375,320 => 475,064/s
totalAckedRecords:       +2,364,651 => 472,930/s
totalReadRecords:        +2,367,688 => 473,538/s
totalDispatchedRecords:  +2,366,604 => 473,321/s
totalBrokerLag:          +2,378,435 => 475,687/s
totalAckedLag:           +13,265    => 2,653/s
totalCommitGap:          +2,365,170 => 473,034/s
totalPeriodicCommitCalls:+1         => 0.2/s
totalCommitCalls:        +1         => 0.2/s
```

Current instant values:

```text
productionRecordsPerSec:       ~470k/s
ackedOffsetRecordsPerSec:      ~470k/s
brokerCommittedRecordsPerSec:  0/s
currentThroughputMsgsPerSec:   ~472k/s
currentThroughputBytesPerSec:  0 B/s
ringLiveCount:                 0
readyPartitionCount:           0
workerQueueDepth:              0
pausedPartitionCount:          0
totalPeriodicCommitCalls:      grows
totalCommitFailures:           2
lastCommitError:               empty
```

Current direct worker QPS computed from `/json` deltas:

```text
active direct workers: 64
direct pushed/s total: ~475,064/s
avg pushed/s:          ~7,423/s
min pushed/s:          ~3,255/s
max pushed/s:          ~9,429/s
skew:                  ~83.2%
```

Top workers:

```text
worker 27: ~9,429/s
worker 35: ~9,308/s
worker 32: ~8,961/s
worker 63: ~8,812/s
worker 33: ~8,770/s
worker 47: ~8,737/s
worker 44: ~8,706/s
worker 36: ~8,704/s
```

Bottom workers:

```text
worker 55: ~3,255/s
worker 56: ~3,359/s
worker 57: ~3,658/s
worker 5:  ~6,461/s
worker 21: ~6,529/s
worker 58: ~6,592/s
worker 26: ~6,736/s
worker 0:  ~6,744/s
```

### 60s Trace After Worker Update

Command:

```bash
python3 - <<'PY'
import json, subprocess, time
sock = "/var/run/tide/worker_6510.sock"
def sample():
    raw = subprocess.check_output(["curl", "-s", "--unix-socket", sock, "http://localhost/json"])
    return json.loads(raw)
a = sample()
start = time.time()
time.sleep(60)
b = sample()
elapsed = time.time() - start
c1 = a["consumers"][0]
c2 = b["consumers"][0]
for f in ["totalPolledRecords", "totalAckedRecords", "totalReadRecords", "totalDispatchedRecords",
          "totalBrokerLag", "totalAckedLag", "totalCommitGap",
          "totalPeriodicCommitCalls", "totalCommitCalls", "totalCommitFailures"]:
    d = c2.get(f, 0) - c1.get(f, 0)
    print(f, d, d / elapsed)
PY

timeout 60 bpftrace -e 'profile:hz:99 /pid == 1943394/ { @[comm] = count(); }'
timeout 60 bpftrace -e 'kprobe:vfs_write /pid == 1943394/ { @bytes[comm] = sum(arg2); @cnt[comm] = count(); }'
```

UDS 60s result:

```text
elapsed:                  60.040s
totalPolledRecords:       +29,101,328 => 484,698/s
totalAckedRecords:        +29,097,158 => 484,628/s
totalReadRecords:         +29,097,448 => 484,633/s
totalDispatchedRecords:   +29,097,286 => 484,630/s
totalBrokerLag:           +2,025,609,358 => 33,737,566/s
totalAckedLag:            +2,067 => 34/s
totalCommitGap:           +2,025,607,291 => 33,737,532/s
totalPeriodicCommitCalls: +11 => 0.18/s
totalCommitCalls:         +11 => 0.18/s
totalCommitFailures:      +1
```

Instant values at the end:

```text
productionRecordsPerSec:       489,511/s
ackedOffsetRecordsPerSec:      489,514/s
brokerCommittedRecordsPerSec:  0/s
currentThroughputMsgsPerSec:   473,176/s
currentThroughputBytesPerSec:  0 B/s
ringLive / ready / workerQ:    0 / 0 / 0
pausedPartitionCount:          0
```

Direct worker 60s distribution:

```text
active direct workers: 64
direct pushed/s total: 484,698/s
avg pushed/s:          7,573/s
min pushed/s:          3,898/s
max pushed/s:          8,091/s
skew:                  55.4%
```

60s eBPF result:

```text
CPU samples: slot-* and c1620e5b/w-* workers are dominant, with rdk:broker-* and jemalloc_bg_thd visible.
vfs_write: writer/ex-* threads dominate write syscall counts; c1620e5b/w-46 is also high.
```

60s conclusion:

```text
- Direct dispatch throughput is now stable around 485k records/s.
- Production and ack rates are close, so internal ack lag is almost flat.
- consumer_v2 direct queues are still empty, so source/direct queueing is not the bottleneck.
- Broker committed lag/commitGap are still pathological because brokerCommittedRecordsPerSec remains 0 despite periodic commits growing.
- Payload bytes are still 0 in live metrics because the deployed direct path does not yet count pushed direct message bytes.
```

### Metrics/HTML Fix Plan

The live HTML already has the worker-QPS JS, but it needs to be easier to read and better aligned with `/json`:

```text
+-------------------------------+------------------------------------------------------------+
| issue                         | fix                                                        |
+-------------------------------+------------------------------------------------------------+
| direct bytes show 0 B/s       | count message len when direct dispatch pushes to workers   |
| card title says payload only  | rename to Record Throughput and separate bytes hint        |
| per-consumer row lacks direct | add direct records/s into 5m rolling stat cells            |
| worker QPS table is cramped   | add aligned numeric columns, cumulative counter, bar chart |
| first sample looks broken     | label it as collecting second /json sample                 |
+-------------------------------+------------------------------------------------------------+
```

Expected after redeploying this metrics fix:

```text
currentThroughputBytesPerSec > 0 in direct dispatch mode
Record Throughput card shows direct records/s after two /json polls
Direct Worker Load Balance table shows pushed/read/acked QPS plus cumulative pushed counts
5m consumer rolling cells include direct records/s
```

New interpretation after the worker update:

```text
- Direct dispatch throughput is healthy and close to production throughput.
- consumer_v2 internal queues are empty.
- periodic commit scheduling is deployed because totalPeriodicCommitCalls grows.
- brokerCommittedRecordsPerSec is still 0, so the remaining commit problem is commit effectiveness / broker-visible offset advancement.
- The UDS / HTML already contains worker QPS code, but the table depends on browser-side /json polling.
```

If the browser does not show worker QPS:

```text
+-----------------------------+----------------------------------------------+
| symptom                     | likely cause                                  |
+-----------------------------+----------------------------------------------+
| section missing entirely    | old binary or stale page/cache                |
| section exists, says waiting| JS did not run, /json fetch failed, or only   |
|                             | the first sample has been collected           |
| throughput lacks "(direct)" | second /json sample not collected or direct   |
|                             | pushed delta is zero in browser               |
+-----------------------------+----------------------------------------------+
```

Browser-side checks:

```text
1. Hard refresh the UDS dashboard.
2. Wait at least 2 seconds because QPS needs two /json samples.
3. If still waiting, check browser console/network for /json request failure.
4. Compare with curl:
   grep -n 'Direct Worker Load Balance' /tmp/uds_6510.html
   grep -n 'buildDirectWorkerQPS' /tmp/uds_6510.html
```

### eBPF Findings

CPU profile by thread name:

```text
Top CPU samples are spread across:
- slot-* threads
- writer/ex-* threads
- rdk:broker-* threads
- jemalloc_bg_thd
- kc-1-323920
```

This means the process is busy, but not because consumer_v2 direct-dispatch queues are backing up. The source side is feeding the pipeline and worker queues are empty.

Scheduling and futex pressure:

```text
High context-switch and futex counts appear in:
- slot-* threads
- rdk:broker-* threads
- kc-1-323920
- writer/ex-* threads
```

VFS write bytes in 8s:

```text
fringedb-c6:     ~1.10 GB
fringedb-c4:     ~1.04 GB
fringedb-c5:     ~0.96 GB
writer/ex-48:    ~302 MB
writer/ex-47:    ~294 MB
writer/ex-17:    ~280 MB
writer/ex-34:    ~269 MB
writer/ex-21:    ~265 MB
writer/ex-23:    ~259 MB
writer/ex-43:    ~240 MB
```

This is strong downstream write pressure. The bottleneck is not visible as consumer_v2 queue backlog because direct workers read and dispatch quickly, then downstream processing/writing determines end-to-end ack advancement.

## Interpretation

There were two separate issues in the original sample:

1. Original deployed binary did not include the direct periodic commit fix.

```text
brokerCommittedRecordsPerSec = 0/s
totalPeriodicCommitCalls     = 0
totalCommitGap               = 16.3B and growing
```

That explained why broker committed lag did not drop in the first sample. After the worker update, `totalPeriodicCommitCalls` grows, so commit scheduling is no longer missing.

2. In the original sample, even ignoring broker commit lag, end-to-end processing throughput was lower than input rate.

```text
productionRecordsPerSec  ~= 554k/s
ackedOffsetRecordsPerSec ~= 401k/s
consumer direct pushed/s ~= 401k/s
```

That explained why internal acked lag continued to grow in the first sample. In the current post-update sample, production and ack are both near `470k/s`, and internal queues are still empty.

Direct worker assignment is not balanced enough:

```text
48 workers active
worker pushed/s skew ~= 131%
max worker ~= 14.1k/s
min worker ~= 3.2k/s
```

This does not look like the primary capacity bottleneck because all direct queues are empty, but it is a real algorithm quality signal. After the UDS HTML per-worker QPS change is deployed, track it in the dashboard and verify the browser is polling `/json`.

## Current Bottleneck Verdict

```text
+-------------------------+     +-------------------------+     +-------------------------+
| Kafka/source production | --> | consumer_v2 direct path | --> | downstream slot/writer  |
| ~554k records/s         |     | ~401k records/s         |     | write-heavy, bottleneck |
+-------------------------+     +-------------------------+     +-------------------------+
             |                              |                               |
             v                              v                               v
      input faster than              queues are empty               ack advancement
      processing capacity            no ring pressure               below input rate
```

Primary live bottleneck:

```text
downstream slot/writer/fringedb write path capacity
```

Current separate correctness problem:

```text
periodic commit is scheduled, but brokerCommittedRecordsPerSec remains 0 in the latest sample.
```

This means the next commit investigation should focus on broker-visible offset advancement:

```text
commit scheduling -> works
commit call count  -> grows
commit failure     -> not currently growing
broker committed   -> not advancing in latest sample
```

## Jemalloc Configuration Advice

### Current Config

Runtime environment:

```bash
strings /proc/319594/environ | grep MALLOC
```

Current value:

```text
MALLOC_CONF=dirty_decay_ms:1000,muzzy_decay_ms:1000,confirm_conf:true,background_thread:true,prof:true,prof_active:true,prof_prefix:/tmp/jemalloc,oversize_threshold:0,retain:false,narenas:128
```

### Why This Is Heavy

The eBPF/kernel evidence already shows allocator and memory churn:

```text
jemalloc_bg_thd visible in CPU samples
__handle_mm_fault / do_user_addr_fault visible in kstack
madvise_free_pte_range / do_madvise visible in kstack
large writer/fringedb write path active
```

This config can amplify that pressure:

```text
+----------------------+----------------------+---------------------------------------------+
| option               | current              | risk                                        |
+----------------------+----------------------+---------------------------------------------+
| prof                 | true                 | heap profiling metadata and stack overhead  |
| prof_active          | true                 | profiling is always on in hot path          |
| dirty_decay_ms       | 1000                 | aggressive purge, more re-fault/re-zero     |
| muzzy_decay_ms       | 1000                 | aggressive purge, more madvise activity     |
| retain               | false                | more mmap/munmap churn for large extents    |
| narenas              | 128                  | more per-arena fragmentation/metadata       |
| oversize_threshold   | 0                    | disables dedicated oversize arena handling  |
+----------------------+----------------------+---------------------------------------------+
```

The most suspicious option is:

```text
prof:true,prof_active:true
```

For a hot production process moving hundreds of thousands of records/s, always-on heap profiling can add CPU overhead, memory metadata overhead, and stack capture cost. It is useful for short diagnostic windows, not as a normal high-throughput runtime default.

### Low-Risk Production Recommendation

For current lag/bottleneck testing, first remove heap profiling from the steady-state config:

```text
MALLOC_CONF=dirty_decay_ms:10000,muzzy_decay_ms:10000,confirm_conf:true,background_thread:true,prof:false,prof_active:false,oversize_threshold:8388608,retain:true,narenas:64
```

Rationale:

```text
+----------------------+----------------------+---------------------------------------------+
| option               | proposed             | reason                                      |
+----------------------+----------------------+---------------------------------------------+
| prof                 | false                | remove heap profiling overhead              |
| prof_active          | false                | prevent stack sampling/allocation metadata  |
| dirty_decay_ms       | 10000                | reduce purge/re-fault churn                 |
| muzzy_decay_ms       | 10000                | reduce madvise churn                        |
| background_thread    | true                 | keep async purging                          |
| retain               | true                 | reduce mmap/munmap churn and page faults    |
| narenas              | 64                   | reduce arena fragmentation vs 128           |
| oversize_threshold   | 8388608              | restore dedicated handling for huge allocs  |
+----------------------+----------------------+---------------------------------------------+
```

Expected tradeoff:

```text
RSS/VIRT may look higher or decay slower, but CPU, page fault, madvise, and mmap/munmap churn should drop.
```

This is usually the right tradeoff while debugging throughput lag because current evidence points to memory churn and downstream CPU/write path pressure, not immediate host OOM.

### Memory-Pressure Conservative Variant

If the host is close to memory pressure or cgroup OOM, use a more conservative decay/retain profile:

```text
MALLOC_CONF=dirty_decay_ms:5000,muzzy_decay_ms:5000,confirm_conf:true,background_thread:true,prof:false,prof_active:false,oversize_threshold:8388608,retain:false,narenas:64
```

Rationale:

```text
- keeps profiling disabled
- still reduces arena count
- lets jemalloc return address space more aggressively
- may keep RSS lower than retain:true
- may keep more kernel page fault/madvise overhead than the throughput profile
```

Use this only if memory headroom is the top risk. If latency/throughput is the top risk, prefer `retain:true` first.

### Short Heap-Profiling Window

If heap profiles are needed, do not keep `prof_active:true` all the time. Use profiling as a short window:

```text
MALLOC_CONF=dirty_decay_ms:10000,muzzy_decay_ms:10000,confirm_conf:true,background_thread:true,prof:true,prof_active:false,prof_prefix:/tmp/jemalloc,lg_prof_sample:20,oversize_threshold:8388608,retain:true,narenas:64
```

Then activate profiling only during the target window with jemalloc control tooling, signal handler, or application hook if available. The desired flow is:

```text
steady state: prof=true, prof_active=false
diagnose:     prof_active=true for 30-120s
dump:         write profile to /tmp/jemalloc.*
recover:      prof_active=false
```

If there is no safe runtime control path, run a short-lived canary process with profiling enabled instead of keeping production hot path profiled continuously.

### Validation Commands

Before and after changing `MALLOC_CONF`, collect the same 60s window:

```bash
pid=319594

echo "=== environ ==="
strings /proc/$pid/environ | grep MALLOC

echo "=== rss/status ==="
grep -E 'VmRSS|VmSize|RssAnon|RssFile|Threads' /proc/$pid/status

echo "=== numa/maps summary ==="
grep -E 'heap|anon' /proc/$pid/smaps_rollup 2>/dev/null || cat /proc/$pid/smaps_rollup

echo "=== faults ==="
pidstat -r -p $pid 1 10

echo "=== eBPF kernel allocator/page-fault signal ==="
timeout 20 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[kstack(8)] = count(); }'

echo "=== eBPF thread CPU signal ==="
timeout 20 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[comm] = count(); }'
```

UDS throughput/lag check:

```bash
python3 - <<'PY'
import json, subprocess, time
sock = "/var/run/tide/worker_6510.sock"
def s():
    return json.loads(subprocess.check_output(["curl","-s","--unix-socket",sock,"http://localhost/json"]))["consumers"][0]
a = s()
time.sleep(10)
b = s()
for f in ["totalPolledRecords","totalAckedRecords","totalBrokerLag","totalAckedLag","totalCommitGap"]:
    print(f, b[f] - a[f], "rate/s", (b[f] - a[f]) / 10)
print("production/s", b["productionRecordsPerSec"])
print("acked/s", b["ackedOffsetRecordsPerSec"])
print("queues", "ring", b["ringLiveCount"], "ready", b["readyPartitionCount"], "workerQ", b["workerQueueDepth"])
PY
```

Success criteria for jemalloc tuning:

```text
+--------------------------------------+-------------------------------+
| metric                               | expected direction            |
+--------------------------------------+-------------------------------+
| minor faults/s                       | down                          |
| kstack __handle_mm_fault samples     | down                          |
| kstack madvise_free/do_madvise       | down                          |
| jemalloc_bg_thd CPU samples          | down                          |
| ackedOffsetRecordsPerSec             | same or up                    |
| RSS                                  | acceptable, not runaway       |
| p99 latency                          | same or better                |
+--------------------------------------+-------------------------------+
```

## Plan

### Phase 0: Confirm UDS HTML And Browser Polling

1. Confirm `/` contains the worker QPS section and JS:

```bash
curl -s --unix-socket /var/run/tide/worker_6510.sock http://localhost/ > /tmp/uds_6510.html
grep -nE 'Direct Worker Load Balance|directWorkerBody|buildDirectWorkerQPS|cardThroughput' /tmp/uds_6510.html
```

2. Confirm `/json` contains direct worker arrays:

```bash
curl -s --unix-socket /var/run/tide/worker_6510.sock http://localhost/json > /tmp/uds_6510.json
python3 - <<'PY'
import json
c = json.load(open("/tmp/uds_6510.json"))["consumers"][0]
print(c["directDispatchEnabled"], c["workerCount"])
print(len(c["directWorkerPushedRecords"]), sum(c["directWorkerPushedRecords"]))
PY
```

3. In browser, wait for at least two `/json` polls. QPS cannot be calculated from one sample.

4. If browser still shows waiting:

```text
- check browser network panel for /json status
- check browser console for JS error
- hard refresh or disable cache
- compare the HTML curl grep result with browser page source
```

### Phase 1: Confirm Commit Effectiveness

The worker update fixed scheduling, but broker-visible committed offsets still need validation:

```bash
curl -s --unix-socket /var/run/tide/worker_6510.sock http://localhost/json \
  | python3 -m json.tool \
  | grep -E 'totalPeriodicCommitCalls|brokerCommittedRecordsPerSec|totalCommitGap'
```

Expected healthy state:

```text
totalPeriodicCommitCalls grows
brokerCommittedRecordsPerSec > 0
totalCommitGap stops monotonic growth or starts shrinking
```

Current post-update state:

```text
totalPeriodicCommitCalls grows
brokerCommittedRecordsPerSec = 0 in latest sample
totalCommitGap still grows
```

Next check:

```text
- inspect per-partition commitGap to see if all partitions are stuck or only hot partitions
- verify commit payload contains updated acked offsets
- verify librdkafka async commit callback status if available
- check whether lag refresh observes committed offsets after async commit delay
```

### Phase 2: Quantify Source vs Sink Capacity

Run 3 parallel measurements for 60s:

1. UDS 10s deltas for poll/read/dispatch/ack/lag.
2. eBPF `profile:hz:99` by `comm`.
3. eBPF `vfs_write` bytes by `comm`.

Acceptance criteria:

```text
source-side bottleneck:
- ringLiveCount grows
- readyPartitionCount grows
- workerQueueDepth grows
- direct pushed/s < Kafka production/s

downstream-side bottleneck:
- ringLiveCount stays near 0
- readyPartitionCount stays 0
- workerQueueDepth stays 0
- direct pushed/s ~= ackedOffsetRecordsPerSec
- writer/fringedb write bytes dominate eBPF
```

Current process matches downstream-side bottleneck.

### Phase 3: Evaluate Assignment Algorithm

After confirming the UDS HTML per-worker QPS patch is present:

1. Open UDS HTML.
2. Watch "Direct Worker Load Balance (Per Worker QPS)".
3. Record pushed/read/acked skew at 1m, 5m, and 15m windows.
4. If skew remains > 100%, test a better partition-to-worker algorithm:

```text
current likely shape: partition/hash based spread
candidate: partition load aware assignment using recent direct pushed/s
constraint: keep per-partition ordering and avoid moving active partitions too frequently
```

### Phase 4: Downstream Write Bottleneck Isolation

Use eBPF only:

```bash
timeout 30 bpftrace -e 'kprobe:vfs_write /pid == 319594/ { @bytes[comm] = sum(arg2); @cnt[comm] = count(); }'
timeout 30 bpftrace -e 'tracepoint:sched:sched_switch /pid == 319594 && (args->prev_state & 2)/ { @[args->prev_comm] = count(); }'
timeout 30 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /pid == 319594/ { @[comm] = count(); }'
```

Then map hot `writer/ex-*` and `fringedb-*` threads to sink/operator config:

```text
thread class -> operator -> sink target -> batch size -> flush/commit policy -> IO target
```

Likely optimization directions:

```text
- increase writer batching if write syscall count is high and payload bytes per write is small
- reduce sync/flush frequency if D-state write wait dominates
- split sink shards or output partitions if a few writer threads dominate bytes
- reduce decode/copy cost only if eBPF CPU profile shifts from writer/fringedb to slot CPU
```

## Next Check Command Set

Use this minimal loop after redeploy:

```bash
python3 - <<'PY'
import json, subprocess, time
sock = "/var/run/tide/worker_6510.sock"
def s():
    return json.loads(subprocess.check_output(["curl","-s","--unix-socket",sock,"http://localhost/json"]))["consumers"][0]
a=s(); time.sleep(10); b=s()
for f in ["totalPolledRecords","totalAckedRecords","totalBrokerLag","totalAckedLag","totalCommitGap","totalPeriodicCommitCalls"]:
    print(f, b[f]-a[f], "rate/s", (b[f]-a[f])/10)
print("instant production/s", b["productionRecordsPerSec"])
print("instant acked/s", b["ackedOffsetRecordsPerSec"])
print("instant committed/s", b["brokerCommittedRecordsPerSec"])
print("queues", "ring", b["ringLiveCount"], "ready", b["readyPartitionCount"], "workerQ", b["workerQueueDepth"])
PY

timeout 15 bpftrace -e 'profile:hz:99 /pid == 319594/ { @[comm] = count(); }'
timeout 15 bpftrace -e 'kprobe:vfs_write /pid == 319594/ { @bytes[comm] = sum(arg2); @cnt[comm] = count(); }'
```

## 2026-05-21 Follow-Up: brokerCommitted / ackedOffset Rate Instability

### Observation Flow

```text
+--------------------------------------------------+
| User observes metrics                            |
| browser opens http://host:6510/                  |
+-------------------------+------------------------+
                          |
                          | manual socat forwards TCP6 -> UDS
                          v
+--------------------------------------------------+
| socat TCP6-LISTEN:6510                           |
| -> UNIX-CONNECT:/var/run/tide/worker_6510.sock   |
+-------------------------+------------------------+
                          |
                          | HTML refreshes /json through UDS
                          v
+--------------------------------------------------+
| worker metrics endpoint                          |
| UDS: /var/run/tide/worker_6510.sock              |
+-------------------------+------------------------+
                          |
                          | refreshLagSnapshotLocked()
                          | every 5000 ms
                          v
+--------------------------------------------------+
| Build lag snapshot                               |
| local ackedOffset                                |
| cached highWatermark                             |
| committed() -> OffsetFetch, timeout 100 ms       |
+-------------------------+------------------------+
                          |
                          v
+--------------------------------------------------+
| Derived lag/rate metrics                         |
| brokerLag / ackedLag / commitGap                 |
| brokerCommittedRecordsPerSec                     |
| ackedOffsetRecordsPerSec                         |
+--------------------------------------------------+
```

There is no dedicated metrics TCP port owned by `tide_worker`. The browser port used in this
reproduction is a manual `socat` bridge from TCP6 `6510` to the worker UDS:
`socat TCP6-LISTEN:6510,bind=[::],reuseaddr,fork UNIX-CONNECT:/var/run/tide/worker_6510.sock`.
Most users still observe the HTML page at `/`, and that page polls `/json` underneath. Therefore
metric fixes must cover both paths: raw JSON fields and HTML labels/explanations.

### Target Mapping

| Field | Value |
|---|---|
| observation port | `6510`, created manually by `socat`; not a built-in worker metrics port |
| pid | `2245371 tide_worker` |
| UDS | `/var/run/tide/worker_6510.sock` |
| manual TCP bridge | `socat TCP6-LISTEN:6510,bind=[::],reuseaddr,fork UNIX-CONNECT:/var/run/tide/worker_6510.sock` |
| browser HTML after bridge | `http://<host>:6510/` |
| UDS HTML without bridge | `curl --unix-socket /var/run/tide/worker_6510.sock http://localhost/` |
| UDS JSON without bridge | `curl --unix-socket /var/run/tide/worker_6510.sock http://localhost/json` |
| log | `/data00/tmp/container-data-v3/containers/da7ab2e5-53e5-4f44-a5fa-f11076c69f7b/v_0/tide_engine_1.1.0.6267/logs/tide_worker.log` |

### Reproduction Command

```bash
# Terminal A: expose the worker UDS as a temporary local observation port.
socat TCP6-LISTEN:6510,bind=[::],reuseaddr,fork UNIX-CONNECT:/var/run/tide/worker_6510.sock

# Terminal B: browser can open http://<host>:6510/; scripted capture may use UDS directly.
python3 - <<'PY'
import json, subprocess, time, statistics
sock = "/var/run/tide/worker_6510.sock"
def sample():
    data = json.loads(subprocess.check_output(
        ["curl", "-s", "--unix-socket", sock, "http://localhost/json"], timeout=5))
    c = data["consumers"][0]
    pls = c.get("partitionLags", [])
    return {
        "ts": time.time(),
        "ackRate": float(c.get("ackedOffsetRecordsPerSec", 0)),
        "commitRate": float(c.get("brokerCommittedRecordsPerSec", 0)),
        "prodRate": float(c.get("productionRecordsPerSec", 0)),
        "commitCalls": c.get("totalCommitCalls", 0),
        "commitFailures": c.get("totalCommitFailures", 0),
        "sumAcked": sum(max(0, int(p.get("ackedOffset", 0))) for p in pls),
        "sumCommitted": sum(max(0, int(p.get("brokerCommittedOffset", 0))) for p in pls),
        "commitGap": c.get("totalCommitGap", 0),
    }
rows = []
end = time.time() + 70
while time.time() < end:
    rows.append(sample())
    time.sleep(1)
first, last = rows[0], rows[-1]
elapsed = last["ts"] - first["ts"]
print("samples", len(rows), "elapsed", f"{elapsed:.3f}s")
for f in ["prodRate", "ackRate", "commitRate"]:
    vals = [r[f] for r in rows]
    print(f, "min", min(vals), "p50", statistics.median(vals),
          "p95", sorted(vals)[int(len(vals) * 0.95) - 1],
          "max", max(vals), "nonzero", sum(v != 0 for v in vals))
print("commitCalls delta", last["commitCalls"] - first["commitCalls"],
      "commitFailures delta", last["commitFailures"] - first["commitFailures"])
print("sumAcked delta/rate", last["sumAcked"] - first["sumAcked"],
      (last["sumAcked"] - first["sumAcked"]) / elapsed)
print("sumCommitted delta/rate", last["sumCommitted"] - first["sumCommitted"],
      (last["sumCommitted"] - first["sumCommitted"]) / elapsed)
print("commitGap first/last/delta", first["commitGap"], last["commitGap"],
      last["commitGap"] - first["commitGap"])
PY

timeout 70 bpftrace -e '
uprobe:/proc/2245371/exe:rd_kafka_commit { @commit[comm] = count(); }
uprobe:/proc/2245371/exe:rd_kafka_committed { @committed_query[comm] = count(); }
uprobe:/proc/2245371/exe:_ZN7RdKafka17KafkaConsumerImpl10commitSyncERSt6vectorIPNS_14TopicPartitionESaIS3_EE { @commitSync[comm] = count(); }
tracepoint:syscalls:sys_enter_sendto /pid == 2245371/ { @sendto[comm] = count(); }
tracepoint:syscalls:sys_enter_recvfrom /pid == 2245371/ { @recvfrom[comm] = count(); }'
```

### Runtime Evidence

| Window | Key result |
|---|---|
| 64s before broker commit catch-up | `ackedOffsetRecordsPerSec` varied from `239,609/s` to `723,907/s`; `brokerCommittedRecordsPerSec` stayed `0`; `totalCommitCalls +13`; `totalCommitFailures +0`; `sumAckedOffset +29,367,372`; `sumBrokerCommittedOffset +0`. |
| 69s after broker commit catch-up | `brokerCommittedRecordsPerSec` reached `7,401,010,000/s`; `totalCommitCalls +14`; `totalCommitFailures +0`; `sumAckedOffset +31,875,762`; `sumBrokerCommittedOffset +37,850,769,622`; `commitGap 37,819,288,493 -> 394,633`. |
| eBPF, same 70s window | `rd_kafka_commit` and `KafkaConsumerImpl::commitSync(vector)` were hit `14` times per commit thread; `rd_kafka_committed` was hit `14` times; no syscall/CPU bottleneck was visible in the metrics path. |

The spike row is the clearest symptom: at `t=15.3s`, `sumBrokerCommittedOffset` advanced by `36,896,371,390/s` in the local 1s sampling view, while the exported `brokerCommittedRecordsPerSec` was `7,401,010,000/s` for five consecutive HTML/JSON reads. The value stayed visible because the server-side lag snapshot refreshes every 5s while the page can read it every second.

### Why The Message Is Unsteady

This is not evidence of real `7.4B/s` broker commit throughput. It is a metrics semantics problem caused by mixing four time domains:

- `/` HTML and `/json` are served by the worker UDS and may be exposed through a manual `socat` TCP bridge.
- the HTML page can read `/json` at about 1s cadence.
- `refreshLagSnapshotLocked()` only recalculates partition lag/rate every `kLagRefreshIntervalMs = 5000`.
- `KafkaConsumer::committed()` performs an `OffsetFetch` request with `kCommittedOffsetTimeoutMs = 100`, so broker committed visibility can be temporarily stale or timeout-sensitive under load.

When broker committed offsets stay stale for several refreshes, `brokerCommittedRecordsPerSec` can be `0`. When a later `OffsetFetch` sees the new group offsets, the entire stale delta is divided by the latest 5s refresh window. That produces a huge catch-up rate, and HTML repeats the same number until the next refresh. `ackedOffsetRecordsPerSec` has the same refresh/hold behavior, but its source is local ack state, so it usually oscillates around real consume capacity instead of accumulating a multi-billion stale broker delta.

Healthy Kafka consume throughput means the poll/direct-dispatch/ack path can keep up. It does not guarantee that the broker group committed offset observation is smooth, because broker lag is based on `brokerCommittedOffset`, not local `ackedOffset`.

### Current consumer_v2 Code Path

| Step | Code path | Meaning |
|---|---|---|
| Lag refresh cadence | `src/source/kafka/consumer_v2/shared_consumer.cpp:1114` | `refreshLagSnapshotLocked()` skips refreshes until 5s elapsed. |
| Broker committed read | `src/source/kafka/consumer_v2/shared_consumer.cpp:1155` | Calls `KafkaConsumer::committed(partitions, 100ms)`. |
| High watermark read | `src/source/kafka/consumer_v2/shared_consumer.cpp:1165` | Uses `get_watermark_offsets()`, which is a cached watermark API. |
| Commit execution | `src/source/kafka/consumer_v2/shared_consumer.cpp:3741` | Calls `commitSync()` and counts global/per-partition failures. |

### librdkafka API Research

`librdkafka` does not expose a special API that makes broker committed offsets non-stale or forces the consumer to invalidate an internal committed-offset cache. The relevant APIs have different meanings:

- `KafkaConsumer::committed(partitions, timeout_ms)` maps to `rd_kafka_committed()`, which enqueues an `RD_KAFKA_OP_OFFSET_FETCH` request and waits up to the caller timeout. Source: `inf/librdkafka/src-cpp/KafkaConsumerImpl.cpp:266`, `inf/librdkafka/src/rdkafka.c:3582`.
- `rd_kafka_committed()` resets requested offsets to invalid, copies the partition list, sends an offset-fetch op to the consumer group coordinator, and retries only transport / wait-coordinator cases within the same timeout budget. Source: `inf/librdkafka/src/rdkafka.c:3597`, `inf/librdkafka/src/rdkafka.c:3607`, `inf/librdkafka/src/rdkafka.c:3642`.
- The broker response handler fills offsets from `OffsetFetchResponse`; `UNSTABLE_OFFSET_COMMIT` can be retried when the request allows retry, but this is about transactional unstable offsets, not a general smoothing mechanism for monitoring. Source: `inf/librdkafka/src/rdkafka_request.c:649`, `inf/librdkafka/src/rdkafka_request.c:760`, `inf/librdkafka/src/rdkafka_request.c:813`.
- `KafkaConsumer::position()` / `rd_kafka_position()` returns the current instance's local consumed position, not the broker group committed offset. It can explain local progress but cannot replace Kafka UI lag semantics. Source: `inf/librdkafka/includes/librdkafka/rdkafka.h:4064`.
- `get_watermark_offsets()` is explicitly cached; `query_watermark_offsets()` queries the broker. These APIs are for log-end watermarks, not group committed offsets. Source: `inf/librdkafka/includes/librdkafka/rdkafka.h:3053`, `inf/librdkafka/includes/librdkafka/rdkafka.h:3067`.
- `commit_queue()` / `offset_commit_cb` can improve commit-result observability, especially duration and per-partition result tracking, but they do not solve stale `committed()` observation by themselves. Source: `inf/librdkafka/includes/librdkafka/rdkafka.h:3968`, `inf/librdkafka/includes/librdkafka/rdkafka.h:4007`.

Practical conclusion: the fix should not be “find another librdkafka API that returns always-fresh broker committed offsets.” The fix should make metrics honest about freshness, sampling window, and catch-up behavior, while optionally increasing the `committed()` timeout or splitting large OffsetFetch requests if evidence shows timeout pressure.

### Metrics / HTML Fix Recommendation

The fix must be visible from the normal HTML path, not only from raw JSON. Users usually open `/`
through a manually-created `socat` port, so the HTML should explain whether a rate is a real
throughput signal or a lag-observation/catch-up signal.

| Area | Recommendation |
|---|---|
| `/json` raw fields | Add `lagRateRefreshMs`, `lagRateWindowMs`, `lagRateSampleAgeMs`, `brokerCommittedOffsetSum`, `ackedOffsetSum`, `highWatermarkOffsetSum`, `brokerCommittedDelta`, and `brokerCommittedCatchup`. |
| `/` HTML labels | Rename or annotate `brokerCommittedRecordsPerSec` as “broker committed observed rate (5s snapshot, may catch up)”; do not present it as real Kafka commit throughput. |
| `/` HTML panels | Show `commitGap`, `ackedLag`, `brokerLag`, `totalCommitFailures`, and `lastCommitError` before the derived broker committed rate. |
| UDS access hint | Show the UDS path and the exact `socat TCP6-LISTEN:6510,bind=[::],reuseaddr,fork UNIX-CONNECT:...` command in the doc/runbook, because there is no dedicated metrics TCP port. |
| Catch-up display | If broker committed offset was unchanged for N refreshes and then jumps, mark the row as `catch-up` and exclude it from capacity/throughput conclusions. |
| Alerting | Alert on `commitGap` trend plus `totalCommitFailures` growth, not on one `brokerCommittedRecordsPerSec` spike. |
| API usage | Keep `committed()` for broker group visibility; consider a larger timeout or partition batching only if `partitionLagInfo.error` proves `OffsetFetch` timeout or coordinator wait. |

### Commit Execution Design Note

The target architecture in `docs/consumer_v2/design/single_client_spsc_dispatch_architecture.md`
requires Kafka API ownership to stay inside `KafkaClientOwner`: `commit / pause / resume` must not be
called by dispatch shards or worker threads. Therefore the safe optimization is not "spawn any
background thread and let it call the same `KafkaConsumer`". The safe optimization is an owner-side
non-blocking commit loop:

```text
DispatchShard
  -> SPSC commit lane
  -> CommitToken(partition, committableOffset, generation)

KafkaClientOwner loop
  -> consume(timeout=1-5ms)
  -> drain commit lanes
  -> coalesce latest offsets per partition
  -> issue commitAsync / commit_queue
  -> serve offset_commit_cb through consume/poll events
  -> update commit-result metrics
```

This can reduce poll disruption versus synchronous `commitSync()` because the owner does not block on
broker commit completion in the normal periodic path. It still preserves the single Kafka API owner
rule.

Required safeguards:

- keep per-partition committed candidates monotonic and generation-checked, so stale callbacks or
  revoked partitions cannot move metrics backward;
- cap in-flight async commits and coalesce new candidates while one request is in flight;
- run final `commitSync()` on revoke/shutdown/checkpoint if the recovery point must be durably visible
  before leaving the group;
- configure `OffsetCommitCb` or use `commit_queue()` so every async commit has observable global and
  per-partition result;
- expose callback latency, callback success/failure, attempted offset sum, succeeded offset sum,
  partition error count, and last callback age in both `/json` and `/` HTML.

Without commit callback metrics, `commitAsync()` would make the UI less accurate: the API return value
only proves the request was accepted/scheduled locally, not that the broker committed the offsets.
For this investigation the priority is accurate UI and metrics first; inaccurate metrics hide commit
bugs and make later performance changes unsafe.

### Commit Bug Assessment

Current evidence still does not prove a commit logic bug. `commitSync()` was called periodically, `totalCommitFailures` stayed flat, and broker committed offsets eventually caught up. The unsteady broker lag message is best explained as stale broker committed observations plus a misleading instantaneous derived rate.

The condition for treating it as a real commit-path bug should be stricter: if `sumAckedOffset` keeps growing for multiple 60s windows, `sumBrokerCommittedOffset` does not grow, and `totalCommitFailures` / per-partition commit errors remain empty, then inspect commit payload ownership and partition grouping. Until that condition is reproduced, the immediate fix belongs in metrics semantics and HTML presentation.

## 2026-05-22 Partition Lag Growth Recheck

### Observation Entry

Use the manual `socat` listener on port `6510` as the user-facing metrics entry.

```bash
socat TCP6-LISTEN:6510,bind=[::],reuseaddr,fork UNIX-CONNECT:/var/run/tide/worker_6510.sock
```

Runtime discovery:

```text
+------------------------------+-----------------------------------------------+
| item                         | observed value                                |
+------------------------------+-----------------------------------------------+
| metrics TCP entry            | 127.0.0.1:6510 via socat                      |
| UDS                          | /var/run/tide/worker_6510.sock                |
| worker pid for UDS           | 3081744                                       |
| direct worker count          | 64                                            |
| consumer group               | tlb_mirror_large                              |
| topic                        | dwd_tlb_observe_otel_tlb_span_fix_sample...   |
| assignment mode              | group_subscribe                               |
| enable.auto.commit           | false                                         |
| commit interval              | 5000 ms                                       |
| pollDrainBatchSize           | 2048                                          |
+------------------------------+-----------------------------------------------+
```

Metrics path:

```text
+---------------------+        +-----------------------------+        +-----------------------+
| curl / browser      | -----> | socat :6510                 | -----> | worker_6510.sock     |
| http://127.0.0.1    |        | TCP6 -> UDS bridge          |        | tide_worker pid 3081744 |
+---------------------+        +-----------------------------+        +-----------------------+
```

Do not treat `6511` as the normal metrics observation entry for this analysis. The live process also
had a `6511` listener owned by `tide_worker`, but the reproducible UI/JSON path used here is `6510`
through `socat`.

### 60s Metrics Evidence

Sampling command:

```bash
python3 - <<'PY'
import json, time, urllib.request, pathlib
url = "http://127.0.0.1:6510/json"
out = pathlib.Path(".dbg/partition-lag-growth-6510-60s.jsonl")
out.parent.mkdir(exist_ok=True)
with out.open("w") as f:
    for i in range(13):
        ts = time.time()
        with urllib.request.urlopen(url, timeout=5) as r:
            data = json.load(r)
        c = data["consumers"][0]
        row = {
            "ts": ts,
            "i": i,
            "summary": {k: c.get(k) for k in [
                "totalBrokerLag", "totalAckedLag", "totalCommitGap",
                "totalPolledRecords", "totalAckedRecords",
                "totalCommitCalls", "totalPeriodicCommitCalls",
                "totalCommitCallbacks", "totalCommitCallbackFailures",
                "pausedPartitionCount", "readyPartitionCount",
                "workerQueueDepth", "ringLiveCount", "lagRateSampleAgeMs"
            ]},
            "partitionLags": c.get("partitionLags", []),
        }
        f.write(json.dumps(row, sort_keys=True) + "\n")
        f.flush()
        if i != 12:
            time.sleep(5)
PY
```

The actual local capture was written as `.dbg/partition-lag-growth-6510-60s.jsonl`.

Aggregate result over `60.2s`:

```text
+-----------------------------+---------------+---------------+-------------+
| metric                      | first         | last          | delta       |
+-----------------------------+---------------+---------------+-------------+
| totalPolledRecords          | 3,271,330,170 | 3,301,624,852 | +30,294,682 |
| totalAckedRecords           | 3,271,304,106 | 3,301,601,599 | +30,297,493 |
| totalBrokerLag              | 2,784,096,938 | 2,809,971,438 | +25,874,500 |
| totalAckedLag               | 57,608        | 39,659        | -17,949     |
| totalCommitGap              | 2,784,039,330 | 2,809,931,779 | +25,892,449 |
| totalCommitCalls            | 1,325         | 1,337         | +12         |
| totalPeriodicCommitCalls    | 1,324         | 1,336         | +12         |
| totalCommitCallbacks        | 1,325         | 1,337         | +12         |
| totalCommitCallbackFailures | 3             | 3             | 0           |
+-----------------------------+---------------+---------------+-------------+
```

Per-sample totals:

```text
+--------+---------------+-------------+---------------+
| sample | totalBrokerLag| totalAckedLag| totalCommitGap|
+--------+---------------+-------------+---------------+
| 0      | 2,784,096,938 | 57,608      | 2,784,039,330 |
| 1      | 2,786,275,212 | 45,720      | 2,786,229,492 |
| 2      | 2,788,482,327 | 46,419      | 2,788,435,908 |
| 3      | 2,790,724,992 | 86,803      | 2,790,638,189 |
| 4      | 2,792,938,272 | 184,787     | 2,792,753,485 |
| 5      | 2,795,197,669 | 1,807,508   | 2,793,390,161 |
| 6      | 3,971,270     | 3,390,815   | 580,455       |
| 7      | 2,798,833,772 | 3,220,971   | 2,795,612,801 |
| 8      | 2,800,321,866 | 1,035,366   | 2,799,286,500 |
| 9      | 2,803,165,037 | 866,998     | 2,802,298,039 |
| 10     | 2,805,819,069 | 213,438     | 2,805,605,631 |
| 11     | 2,807,947,793 | 48,432      | 2,807,899,361 |
| 12     | 2,809,971,438 | 39,659      | 2,809,931,779 |
+--------+---------------+-------------+---------------+
```

Interpretation:

- Local consume and ack progress are healthy in this window: `totalAckedRecords` slightly exceeded
  `totalPolledRecords` delta because previous in-flight work was also acked.
- `totalAckedLag` stayed small relative to `totalBrokerLag`, so the growing lag is not local worker
  backlog.
- `totalBrokerLag` is almost exactly `totalCommitGap`, so the visible growth is broker committed
  offset observation lag.
- Sample `6` is decisive: broker lag dropped from billions to `3.97M`, then the next sample jumped
  back to billions. A real Kafka group committed offset should not normally move forward for all
  partitions and then move backward to old values in the next 5s sample.

### Partition Evidence

Top final broker-lag partitions all had normal local progress but no broker committed progress from
the first to last sample:

```text
+-----------+-------------+-------------+-------------+------------+-------------+
| partition | brokerLag   | dBrokerLag  | ackedLag    | dAckedOff  | dCommitted |
+-----------+-------------+-------------+-------------+------------+-------------+
| 315       | 26,537,555  | +250,501    | 0           | +250,501   | 0           |
| 267       | 26,497,417  | +229,869    | 3,962       | +227,166   | 0           |
| 266       | 26,485,801  | +241,979    | 0           | +241,979   | 0           |
| 327       | 26,480,918  | +244,716    | 0           | +245,528   | 0           |
| 321       | 26,478,365  | +235,635    | 0           | +235,635   | 0           |
+-----------+-------------+-------------+-------------+------------+-------------+
```

All `125` observed partitions had the same pattern during sample `6`: `brokerCommittedOffset`
temporarily jumped near the local acked offset and then reverted on the next sample. Example:

```text
+-----------+-----------------+-----------------+-----------------+
| partition | sample 0        | sample 6        | sample 12       |
+-----------+-----------------+-----------------+-----------------+
| 315       | 146128425150    | 146154802567    | 146128425150    |
| 267       | 146209415054    | 146235764860    | 146209415054    |
| 266       | 146146034470    | 146172368729    | 146146034470    |
| 321       | 145990499620    | 146016828916    | 145990499620    |
| 327       | 146054410192    | 146080738879    | 146054410192    |
+-----------+-----------------+-----------------+-----------------+
```

This is the strongest evidence in this run. It falsifies a pure throughput/backpressure explanation:
the local `ackedOffset` and high watermark move forward, while the broker committed observation is
non-monotonic.

### Log Evidence

Log files were discovered from `/proc/3081744/fd`:

```text
+----------------------+---------------------------------------------------------------+
| log                  | path                                                          |
+----------------------+---------------------------------------------------------------+
| tide worker log      | .../logs/tide_worker.log                                      |
| error log            | .../logs/tide_worker.errorlog                                 |
| backpressure log     | .../logs/backpress.log                                        |
| task metrics         | .../logs/taskmanager/task_metrics.log                         |
+----------------------+---------------------------------------------------------------+
```

Relevant error log findings:

```text
+---------------------------+--------------------------------------------------------------+
| time                      | event                                                        |
+---------------------------+--------------------------------------------------------------+
| 2026-05-22 12:43:43 +0800 | partition 167 commit callback failed: Group rebalance        |
| 2026-05-22 12:52:33 +0800 | partition 167 commit callback failed: Broker transport       |
| 2026-05-22 13:23:18 +0800 | partition 167 commit callback failed: Broker transport       |
+---------------------------+--------------------------------------------------------------+
```

The 60s metrics window did not show new commit callback failures:

```text
totalCommitCallbackFailures = 3 -> 3
totalCommitFailures         = 3 -> 3
```

Therefore the observed multi-billion broker lag in this capture is not explained by new commit
callback failures. The historical partition `167` failures are real and should remain visible, but
they do not explain synchronized non-monotonic broker committed offsets for all 125 observed
partitions.

### eBPF Evidence

Thread distribution:

```text
+---------------------+---------------------------------------------------------------+
| signal              | observation                                                   |
+---------------------+---------------------------------------------------------------+
| thread count        | ~1400 threads in pid 3081744                                  |
| CPU profile         | dominant samples in slot-* workers and rdk:broker-* threads   |
| Kafka owner         | kc-1-3091318 visible but not the main CPU hotspot             |
| futex/sched         | high counts are mostly normal worker/writer/broker waits      |
| syscall read/write  | writer/ex-*, slot-*, rdk:broker-* dominate; no source queue   |
|                     | backpressure signature matched the lag jump                   |
+---------------------+---------------------------------------------------------------+
```

eBPF conclusion:

- There is no evidence that partition lag growth is caused by source-side queue buildup.
- `pausedPartitionCount`, `readyPartitionCount`, `workerQueueDepth`, and `ringLiveCount` were not
  growing.
- eBPF supports the metrics conclusion: local pipeline throughput is active; the suspicious part is
  broker committed offset observation / commit visibility.

### Source-Level Finding

Current metrics build path:

```text
debug socket / metrics thread
  -> SharedConsumerState::buildRuntimeInfo()
     -> lock(mutex_)
     -> syncDirectCommittedOffsetsLocked()
     -> refreshLagSnapshotLocked(nowMs())
        -> KafkaConsumer::committed(partitions, timeout)
        -> KafkaConsumer::get_watermark_offsets(...)
```

Relevant code path:

```text
src/source/kafka/consumer_v2/shared_consumer.cpp
  buildRuntimeInfo()
  refreshLagSnapshotLocked()
  entry.first->committed(partitions, kCommittedOffsetTimeoutMs)
  entry.first->get_watermark_offsets(...)
```

This violates the ownership direction in
`docs/consumer_v2/design/single_client_spsc_dispatch_architecture.md`: Kafka APIs should be owned by
the Kafka owner thread. Even if librdkafka's C APIs are broadly thread-safe, this design makes the
metrics path a second Kafka API caller and allows a monitoring request to interleave with owner
consume/commit/poll callback handling.

librdkafka source confirms `committed()` is not a passive field read:

```text
rd_kafka_committed()
  -> creates RD_KAFKA_OP_OFFSET_FETCH
  -> enqueues it to the consumer group ops queue
  -> waits on a reply queue
  -> updates the caller's partition list from OffsetFetchResponse
```

So every `/json` lag refresh can inject a blocking OffsetFetch operation from the metrics thread.

### Hypothesis Verdict

```text
+------------+-------------------------------+----------+------------------------------+
| hypothesis | description                   | verdict  | evidence                     |
+------------+-------------------------------+----------+------------------------------+
| H1         | partition local backpressure  | rejected | ackedLag low; queues empty   |
| H2         | worker lane skew              | secondary| skew exists, not root cause  |
| H3         | stale/non-owner metrics query | confirmed| committed jumps then reverts |
| H4         | librdkafka fetch imbalance    | rejected | poll/ack progress healthy    |
| H5         | owner loop starvation         | unlikely | commits/callbacks continue   |
+------------+-------------------------------+----------+------------------------------+
```

### Root Cause

The reported "some partitions lag steadily grows" is primarily a broker committed lag / commit-gap
observation problem, not a Consumer V2 local consume lag problem.

The immediate root cause is:

```text
metrics thread calls Kafka committed-offset APIs directly
  -> OffsetFetch interleaves with owner poll/commit/callback work
  -> brokerCommittedOffset snapshot becomes non-monotonic
  -> brokerLag and commitGap appear to grow steadily for partitions
  -> sample occasionally catches up, then can fall back to stale values
```

There is still a separate correctness guard to keep: Kafka allows committing a smaller offset, so a
true external stale committer can also cause broker committed offset regression. The current evidence
does not prove an external committer because `6510` and `7510` are normal group members with `125`
assigned partitions each, and no new commit failures were seen in this window. The fix should still
make regressions explicit in metrics instead of silently presenting them as capacity lag.

### Fix Plan

Phase 1: move lag refresh to Kafka owner.

```text
+-----------------------+        +--------------------------+        +----------------------+
| debug socket thread   | -----> | cached lag snapshot      | <----- | Kafka owner thread   |
| /json and / HTML      |        | atomic / mutex protected |        | only Kafka API caller |
+-----------------------+        +--------------------------+        +----------------------+
                                                                  |
                                                                  v
                                                    committed() / watermark query
```

Rules:

- `/json` must not call `KafkaConsumer::committed()` directly.
- `/json` should read the last owner-published lag snapshot.
- The Kafka owner should refresh broker committed offsets on a bounded cadence, e.g. every `5s`.
- The owner should split OffsetFetch if needed, e.g. batches of `64` partitions, only if timeout or
  coordinator pressure appears in `PartitionLagInfo.error`.
- The owner must continue draining consume, control, commit-progress, and commit lanes between
  blocking/bounded maintenance tasks.

Phase 2: make lag metrics monotonic and explicit.

Expose separate fields:

```text
+-------------------------------+---------------------------------------------------------+
| field                         | meaning                                                 |
+-------------------------------+---------------------------------------------------------+
| brokerCommittedOffsetRaw      | latest raw OffsetFetch result                           |
| brokerCommittedOffsetStable   | per-partition max observed broker committed offset      |
| brokerCommittedRegression     | raw result < previous stable result                     |
| brokerCommittedStaleRefreshes | refreshes where raw offset did not advance              |
| ackedLag                      | high watermark - local acked offset                     |
| brokerLagRaw                  | high watermark - brokerCommittedOffsetRaw               |
| brokerLagStable               | high watermark - brokerCommittedOffsetStable            |
| commitGapStable               | local acked offset - brokerCommittedOffsetStable        |
+-------------------------------+---------------------------------------------------------+
```

UI rule:

- Use `ackedLag` as the primary local consume health indicator.
- Show `brokerLagRaw` as broker observation, not as Consumer V2 capacity lag.
- If `brokerCommittedRegression=true`, show a warning: "broker committed observation regressed;
  possible stale OffsetFetch or external stale committer".
- Do not use `brokerCommittedRecordsPerSec` as throughput. Keep it labeled as observed snapshot
  catch-up.

Phase 3: detect true stale external committers.

Add diagnostics:

- per-commit attempted offset sum and succeeded offset sum already exist globally; extend to
  per-partition last attempted/succeeded offset for top lag partitions;
- count `commitLowerThanLocalAck` if broker raw committed offset is lower than this process's last
  successful commit callback offset;
- if `brokerCommittedRegression` persists after owner-only metrics refresh, inspect consumer group
  membership and commit writers outside Consumer V2.

Phase 4: validation.

Use the same `6510` entry:

```bash
python3 - <<'PY'
import json, subprocess, time
def s():
    return json.loads(subprocess.check_output(
        ["curl", "-s", "http://127.0.0.1:6510/json"]
    ))["consumers"][0]
a = s()
time.sleep(60)
b = s()
for f in [
    "totalPolledRecords", "totalAckedRecords",
    "totalBrokerLag", "totalAckedLag", "totalCommitGap",
    "totalCommitCalls", "totalCommitCallbacks", "totalCommitCallbackFailures"
]:
    print(f, a.get(f), "->", b.get(f), "delta", b.get(f, 0) - a.get(f, 0))
PY
```

Expected after fix:

```text
+--------------------------------------+----------------------------------------------+
| signal                               | expected                                     |
+--------------------------------------+----------------------------------------------+
| brokerCommittedOffsetStable          | monotonic per partition                      |
| brokerCommittedRegression            | 0 in normal runs; warning if external issue  |
| totalAckedLag                        | remains low if consume path is healthy       |
| totalCommitCallbackFailures          | does not grow in healthy run                 |
| /json latency                        | no long blocking OffsetFetch on HTTP thread  |
| Kafka owner CPU/eBPF                 | no new owner starvation                      |
+--------------------------------------+----------------------------------------------+
```

### Immediate Operational Guidance

For current live diagnosis, judge source health with this priority:

```text
1. totalAckedLag and ackedOffsetRecordsPerSec
2. ringLiveCount / readyPartitionCount / workerQueueDepth
3. commit callback failures and lastCommitError
4. brokerLagRaw / totalBrokerLag only as broker committed observation
```

If `ackedLag` is low and queues are empty, do not conclude Consumer V2 partition consumption is
falling behind only because `brokerLagRaw` grows. Treat it as a commit visibility / metrics owner
problem until owner-only lag refresh proves otherwise.

## 2026-05-22 Follow-Up: 60s Evidence-Driven Dispatch Assessment

### Method

Dispatch assessment must be made from runtime evidence, not from code shape. A direct SPSC path can
still accumulate backlog, show worker skew, or be blocked indirectly by downstream writer pressure.

Minimum evidence for any dispatch verdict:

- Metrics deltas over a full 60s window: poll, read, dispatch, ack, queue depth, lag, commit.
- eBPF samples over the same 60s window: CPU thread classes plus write or wait pressure.
- At least three consecutive 60s windows before calling the system stable.

Metrics command used for the 60s windows:

```bash
python3 - <<'PY'
import json, subprocess, time
sock = "/var/run/tide/worker_6510.sock"
def sample():
    raw = subprocess.check_output(
        ["curl", "-s", "--unix-socket", sock, "http://localhost/json"], timeout=5
    )
    return json.loads(raw)["consumers"][0]
rows = []
for i in range(13):
    rows.append((time.time(), sample()))
    if i != 12:
        time.sleep(5)
first_ts, first = rows[0]
last_ts, last = rows[-1]
elapsed = last_ts - first_ts
for f in [
    "totalPolledRecords", "totalAckedRecords", "totalReadRecords", "totalDispatchedRecords",
    "totalBrokerLag", "totalAckedLag", "totalCommitGap", "totalCommitCalls",
    "totalCommitCallbacks", "totalCommitCallbackFailures",
    "ringLiveCount", "readyPartitionCount", "workerQueueDepth", "pausedPartitionCount",
]:
    print(f, first.get(f), "->", last.get(f),
          "rate/s", (last.get(f, 0) - first.get(f, 0)) / elapsed)
PY
```

eBPF commands used for each matching 60s window:

```bash
timeout 60 bpftrace -e 'profile:hz:99 /pid == 4051649/ { @cpu[comm] = count(); }'
timeout 60 bpftrace -e 'kprobe:vfs_write /pid == 4051649/ { @write_bytes[comm] = sum(arg2); @write_cnt[comm] = count(); }'
timeout 60 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /pid == 4051649/ { @futex[comm] = count(); } tracepoint:sched:sched_switch /pid == 4051649 && (args->prev_state & 2)/ { @dstate[args->prev_comm] = count(); }'
```

### One-Window Recheck

Live target:

| Item | Value |
|---|---:|
| pid | 4051649 |
| UDS | `/var/run/tide/worker_6510.sock` |
| worker count | 64 |
| sample window | 60.285s |
| metrics samples | 13 |

60s metrics:

| Metric | First | Last | Rate |
|---|---:|---:|---:|
| totalPolledRecords | 3,902,359,032 | 3,932,044,116 | 492,415/s |
| totalAckedRecords | 3,898,113,189 | 3,930,344,222 | 534,647/s |
| totalReadRecords | 3,898,256,578 | 3,930,398,855 | 533,175/s |
| totalDispatchedRecords | 3,898,188,147 | 3,930,345,246 | 533,421/s |
| totalBrokerLag | 206,832,838 | 210,541,616 | +61,521/s |
| totalAckedLag | 206,168,450 | 209,915,466 | +62,155/s |
| totalCommitGap | 664,388 | 626,150 | -634/s |
| totalCommitCalls | 1,419 | 1,430 | +0.18/s |
| totalCommitCallbacks | 1,419 | 1,430 | +0.18/s |
| totalCommitCallbackFailures | 6 | 6 | 0/s |
| ringLiveCount | 4,111,549 | 1,637,519 | -41,039/s |
| readyPartitionCount | 125 | 125 | 0/s |
| workerQueueDepth | 4,058,972 | 1,599,764 | -40,793/s |
| pausedPartitionCount | 0 | 0 | 0/s |

Direct worker distribution:

| Metric | Active | Total/s | Avg/s | Min/s | Max/s | Skew |
|---|---:|---:|---:|---:|---:|---:|
| directWorkerPushedRecords | 64 | 492,382 | 7,693 | 3,074 | 9,866 | 88.3% |
| directWorkerReadRecords | 64 | 533,175 | 8,331 | 3,074 | 10,243 | 86.0% |
| directWorkerAckedRecords | 64 | 533,421 | 8,335 | 3,074 | 10,260 | 86.2% |

One-window interpretation:

- The queue was not empty. `ringLiveCount` and `workerQueueDepth` were both in the millions.
- The queue was shrinking during this one window, so workers were draining historical backlog.
- `read/ack` exceeded `poll/push`, which argues against immediate direct-dispatch collapse.
- `totalAckedLag` still grew at about `62k/s`, so smooth consumption was not reached.
- `commitGap` shrank and callback failures did not grow, so this window was not primarily a commit failure window.

### Three-Window Recheck

The one-window drain did not hold across three consecutive windows.

| Window | poll/s | ack/s | push/s | read/s | ackLag/s | commitGap/s | ringLive/s | workerQ/s |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 451,306 | 457,429 | 451,306 | 457,356 | +132,005 | -15,931 | -6,124 | -6,052 |
| 2 | 481,473 | 489,852 | 481,497 | 491,185 | +55,016 | -21,091 | -10,108 | -9,688 |
| 3 | 499,261 | 489,484 | 499,263 | 491,906 | +28,995 | -7,915 | +7,869 | +7,357 |

Direct worker skew across the same three windows:

| Window | Pushed Skew | Pushed Min/s | Pushed Max/s |
|---:|---:|---:|---:|
| 1 | 91.0% | 2,870 | 9,287 |
| 2 | 97.4% | 3,000 | 10,331 |
| 3 | 76.6% | 3,908 | 9,884 |

Three-window interpretation:

- Windows 1 and 2 drained backlog, but slowly.
- Window 3 reversed direction: both `ringLiveCount` and `workerQueueDepth` grew.
- `totalAckedLag` growth improved from `+132k/s` to `+29k/s`, but stayed positive in every window.
- `totalCommitGap` decreased in all windows and commit callback failures did not grow.
- Direct worker skew stayed high, but queue behavior was oscillating rather than proving stable dispatch collapse.

### eBPF Evidence

CPU sample classes:

| Window | #1 | #2 | #3 | #4 | kc-owner |
|---:|---:|---:|---:|---:|---:|
| 1 | slot-* 100,177 | rdk:broker-* 40,506 | writer/ex-* 29,220 | consumer-w-* 24,160 | 696 |
| 2 | slot-* 110,547 | rdk:broker-* 44,129 | writer/ex-* 29,093 | consumer-w-* 24,228 | 740 |
| 3 | slot-* 112,884 | rdk:broker-* 44,872 | writer/ex-* 23,193 | consumer-w-* 18,481 | 728 |

VFS write bytes:

| Window | #1 | #2 | #3 |
|---:|---:|---:|---:|
| 1 | writer/ex-* 55.7 GB | fringedb-c* 26.3 GB | consumer-w-* 13.6 GB |
| 2 | writer/ex-* 55.2 GB | fringedb-c* 31.5 GB | consumer-w-* 13.1 GB |
| 3 | writer/ex-* 44.8 GB | fringedb-c* 36.0 GB | consumer-w-* 10.5 GB |

D-state switches:

| Window | #1 | #2 | #3 | #4 |
|---:|---:|---:|---:|---:|
| 1 | rdk:broker-* 749,300 | writer/ex-* 500,145 | slot-* 333,419 | consumer-w-* 106,336 |
| 2 | rdk:broker-* 688,533 | writer/ex-* 454,065 | slot-* 361,318 | jemalloc 84,762 |
| 3 | rdk:broker-* 757,760 | slot-* 513,568 | writer/ex-* 377,434 | consumer-w-* 72,352 |

Futex counts:

| Window | #1 | #2 | #3 | #4 |
|---:|---:|---:|---:|---:|
| 1 | slot-* 1,514,101 | rdk:broker-* 1,513,226 | kc-owner 622,566 | writer/ex-* 281,575 |
| 2 | slot-* 1,801,952 | rdk:broker-* 1,448,808 | kc-owner 627,526 | writer/ex-* 264,468 |
| 3 | rdk:broker-* 1,264,062 | slot-* 1,226,654 | kc-owner 537,013 | writer/ex-* 241,516 |

eBPF interpretation:

- CPU was dominated by `slot-*`, then `rdk:broker-*`, then `writer/ex-*`.
- VFS write bytes were dominated by `writer/ex-*` and `fringedb-c*` in all windows.
- D-state pressure was dominated by `rdk:broker-*`, `writer/ex-*`, and `slot-*`.
- `consumer-w-*` was visible, but not the dominant class.
- `kc-owner` futex count was high, but commit callbacks still progressed and callback failures did not grow.

### Hypothesis Status

| Hypothesis | Status | Evidence |
|---|---|---|
| Backlog steadily converges for 3 x 60s | Rejected | Window 3 queue depth grew |
| Backlog oscillates or regrows after temporary drain | Supported | Window 1/2 down, window 3 up |
| Hotspots remain downstream / slot / broker oriented | Supported | CPU, write, and D-state maps |
| Dispatch is primary only if queue grows and eBPF shifts there | Unproven | `consumer-w-*` visible but not dominant |

### Current Open Verdict

The system is not yet in smooth-consumption steady state.

Evidence:

- `totalAckedLag` grew in all three 60s windows.
- `workerQueueDepth` and `ringLiveCount` drained first, then grew again.
- eBPF consistently showed `slot-*`, `rdk:broker-*`, `writer/ex-*`, and `fringedb-c*` as dominant classes.
- Direct worker skew is real and should remain a dispatch optimization candidate.
- Current evidence does not justify treating dispatch assignment as the sole or primary root cause.

Next evidence to collect:

- Continue another `3 x 60s` run after any downstream/write or broker-side change.
- Add per-worker direct ring depth and inflight metrics if they are not already visible in `/json`.
- Correlate hot `writer/ex-*` and `slot-*` thread IDs with operator/sink shards.
- Promote dispatch load-aware binding to a P0 experiment only if queue growth persists and eBPF shifts toward `consumer-w-*` or `kc-owner`.

### kc-owner Futex Follow-Up

Question: why is `kc-owner` futex count high if Kafka owner should be a single thread?

First, the thread name is not unique. The process had three Linux TIDs with comm
`kc-1-4064365`, but only one was the active owner thread:

| TID | comm | State | Voluntary Context Switches | Nonvoluntary Context Switches |
|---:|---|---|---:|---:|
| 4064365 | kc-1-4064365 | sleeping | 10,058,338 | 465,010 |
| 4064486 | kc-1-4064365 | sleeping | 883 | 2 |
| 4064490 | kc-1-4064365 | sleeping | 11 | 0 |

Command used:

```bash
python3 - <<'PY'
import os
pid = "4051649"
for tid in sorted(os.listdir(f"/proc/{pid}/task")):
    comm = open(f"/proc/{pid}/task/{tid}/comm").read().strip()
    if comm.startswith("kc-"):
        status = open(f"/proc/{pid}/task/{tid}/status").read().splitlines()
        print("tid", tid, "comm", comm)
        for line in status:
            if line.startswith(("State:", "voluntary_ctxt_switches:", "nonvoluntary_ctxt_switches:")):
                print(" ", line)
PY
```

The active TID was then sampled directly. In 20s it entered futex about `106,984` times, or roughly
`5.3k/s`:

| Futex op | Meaning | Count in 20s | Main source |
|---:|---|---:|---|
| 0 | `FUTEX_WAIT` | 26,746 | librdkafka internal queue mutex waits |
| 1 | `FUTEX_WAKE` | 48,854 | librdkafka internal queue mutex unlock wakeups |
| 9 | `FUTEX_WAIT_BITSET` | 20,123 | `LaneSignal::wait()` / folly Baton timed waits |
| 10 | `FUTEX_WAKE_BITSET` | 11,261 | `LaneSignal::notify()` / folly Baton wakeups |

Command used:

```bash
timeout 20 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /tid == 4064365/ { @op[args->op & 127] = count(); @addr[args->uaddr, args->op & 127] = count(); }'
```

User-stack evidence:

| Futex op | Count | Stack summary |
|---:|---:|---|
| 1 | 45,920 | `rd_kafka_q_serve_rkmessages -> rd_kafka_consume_batch_queue -> pollLoop` |
| 0 | 25,461 | `mtx_lock -> rd_kafka_q_serve_rkmessages -> rd_kafka_consume_batch_queue -> pollLoop` |
| 9 | 18,596 | `LaneSignal::wait -> handleConsumedMessagesDirect -> pollLoop` |
| 10 | 4,819 | `syscall -> pollLoop` |
| 10 | 4,530 | `syscall -> pollLoop` |
| 10 | 1,887 | `syscall -> handleConsumedMessagesDirect -> pollLoop` |
| 9 | 1,524 | `__pthread_cond_timedwait` |
| 1 | 1,143 | `rd_kafka_q_serve_rkmessages -> rd_kafka_consume_batch_queue -> pollLoop` |
| 1 | 553 | `rd_kafka_q_serve_rkmessages -> rd_kafka_consume_batch_queue -> pollLoop` |
| 0 | 505 | `rd_kafka_op_offset_store -> rd_kafka_q_serve_rkmessages -> rd_kafka_consume_batch_queue -> pollLoop` |

Command used:

```bash
timeout 20 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /tid == 4064365/ { @[args->op & 127, ustack(12)] = count(); }'
```

Scheduler evidence from the same active TID:

| Event | Count in 20s | Meaning |
|---|---:|---|
| `prev_state[0]` | 1,886 | switched out while runnable |
| `prev_state[1]` | 20,770 | switched out sleeping |
| `wakeins` | 22,656 | scheduled back in |

Command used:

```bash
timeout 20 bpftrace -e 'tracepoint:sched:sched_switch /args->prev_pid == 4064365/ { @prev_state[args->prev_state] = count(); } tracepoint:sched:sched_switch /args->next_pid == 4064365/ { @wakeins = count(); }'
```

Interpretation:

- `kc-owner` is logically one active Kafka owner thread, but the thread name appears on three TIDs.
- High futex count is not caused by many owner threads. It is one active owner repeatedly entering
  futex.
- The largest source is librdkafka queue serving: `rd_kafka_consume_batch_queue()` calls into
  `rd_kafka_q_serve_rkmessages`, which locks/unlocks internal librdkafka queues.
- The second important source is direct dispatch backpressure: `handleConsumedMessagesDirect()` calls
  `LaneSignal::wait()` when the preferred worker ring is full.
- The direct-dispatch wait path matches the earlier metrics evidence: `ringLiveCount` and
  `workerQueueDepth` were nonzero and even regrew in the third 60s window.
- Therefore `kc-owner` futex pressure is a symptom of two things happening together: librdkafka queue
  mutex churn plus owner-side waiting for worker ring drain under backlog.

Current verdict:

- `kc-owner` futex count is real and meaningful.
- It does not prove multiple Kafka owners.
- It does prove the owner thread is frequently sleeping/waking while serving librdkafka queues and
  waiting for direct worker lane drain.
- If this remains high when `workerQueueDepth` is low, focus on librdkafka queue mutex/commit/lag
  refresh behavior.
- If this remains high while `workerQueueDepth` grows, focus on direct worker drain latency and
  downstream slot/writer pressure.

## 2026-05-22 Appendix: kc-owner Futex Evidence And Commands

### Conclusion

`kc-owner` futex pressure is real, but it does not mean there are multiple active Kafka owner
threads. In the checked process, three Linux TIDs used the same `kc-1-4064365` comm name, while only
TID `4064365` was active.

The high futex count came from two hot paths:

- librdkafka queue serving inside `rd_kafka_consume_batch_queue()`.
- direct-dispatch backpressure where `handleConsumedMessagesDirect()` waits on `LaneSignal::wait()`
  for worker ring drain.

This means `kc-owner` futex is a mixed signal:

- If `workerQueueDepth` is low, investigate librdkafka queue mutex churn, commit, and lag refresh.
- If `workerQueueDepth` is high or growing, investigate direct worker drain latency and downstream
  `slot-*` / `writer/ex-*` / `fringedb-c*` pressure.

### Thread Identity Check

Command:

```bash
python3 - <<'PY'
import os
pid = "4051649"
for tid in sorted(os.listdir(f"/proc/{pid}/task")):
    comm = open(f"/proc/{pid}/task/{tid}/comm").read().strip()
    if comm.startswith("kc-"):
        status = open(f"/proc/{pid}/task/{tid}/status").read().splitlines()
        print("tid", tid, "comm", comm)
        for line in status:
            if line.startswith(("State:", "voluntary_ctxt_switches:", "nonvoluntary_ctxt_switches:")):
                print(" ", line)
PY
```

Observed result:

| TID | comm | State | Voluntary Context Switches | Nonvoluntary Context Switches |
|---:|---|---|---:|---:|
| 4064365 | kc-1-4064365 | sleeping | 10,058,338 | 465,010 |
| 4064486 | kc-1-4064365 | sleeping | 883 | 2 |
| 4064490 | kc-1-4064365 | sleeping | 11 | 0 |

### Futex Operation Breakdown

Command:

```bash
timeout 20 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /tid == 4064365/ { @op[args->op & 127] = count(); @addr[args->uaddr, args->op & 127] = count(); }'
```

Observed result:

| Futex op | Meaning | Count in 20s | Interpretation |
|---:|---|---:|---|
| 0 | `FUTEX_WAIT` | 26,746 | Mostly librdkafka internal queue mutex waits |
| 1 | `FUTEX_WAKE` | 48,854 | Mostly librdkafka internal queue mutex unlock wakeups |
| 9 | `FUTEX_WAIT_BITSET` | 20,123 | Mostly `LaneSignal::wait()` / folly Baton timed waits |
| 10 | `FUTEX_WAKE_BITSET` | 11,261 | Mostly `LaneSignal::notify()` / folly Baton wakeups |

### Futex User-Stack Attribution

Command:

```bash
timeout 20 bpftrace -e 'tracepoint:syscalls:sys_enter_futex /tid == 4064365/ { @[args->op & 127, ustack(12)] = count(); }'
```

Observed top stacks:

| Futex op | Count | Stack Summary |
|---:|---:|---|
| 1 | 45,920 | `rd_kafka_q_serve_rkmessages -> rd_kafka_consume_batch_queue -> pollLoop` |
| 0 | 25,461 | `mtx_lock -> rd_kafka_q_serve_rkmessages -> rd_kafka_consume_batch_queue -> pollLoop` |
| 9 | 18,596 | `LaneSignal::wait -> handleConsumedMessagesDirect -> pollLoop` |
| 10 | 4,819 | `syscall -> pollLoop` |
| 10 | 4,530 | `syscall -> pollLoop` |
| 10 | 1,887 | `syscall -> handleConsumedMessagesDirect -> pollLoop` |
| 9 | 1,524 | `__pthread_cond_timedwait` |
| 0 | 505 | `rd_kafka_op_offset_store -> rd_kafka_q_serve_rkmessages -> rd_kafka_consume_batch_queue -> pollLoop` |

### Scheduler Check

Command:

```bash
timeout 20 bpftrace -e 'tracepoint:sched:sched_switch /args->prev_pid == 4064365/ { @prev_state[args->prev_state] = count(); } tracepoint:sched:sched_switch /args->next_pid == 4064365/ { @wakeins = count(); }'
```

Observed result:

| Event | Count in 20s | Meaning |
|---|---:|---|
| `prev_state[0]` | 1,886 | Switched out while runnable |
| `prev_state[1]` | 20,770 | Switched out sleeping |
| `wakeins` | 22,656 | Scheduled back in |

### Operational Reading

- `kc-owner` is one active owner TID with frequent sleep/wake cycles.
- The large futex count is not caused by multiple owners.
- The largest futex source is librdkafka queue lock traffic during `rd_kafka_consume_batch_queue()`.
- The direct-dispatch source is owner-side waiting for worker ring drain.
- A high `kc-owner` futex count should always be interpreted together with `workerQueueDepth`,
  `ringLiveCount`, and eBPF hot classes.
