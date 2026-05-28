# Topicstats HTTP Endpoint Code Path

This document explains the code path for:

```bash
curl -s "http://127.0.0.1:6789/federation-1/topicstats?topic=doubao-trace-TUDP.pb"
```

## 0) ASCII data flow

```text
+----------------------------------------------------------------------------------+
| curl                                                                             |
| GET http://127.0.0.1:6789/federation-1/topicstats?topic=doubao-trace-TUDP.pb    |
+-----------------------------------+----------------------------------------------+
                                    |
                                    v
+----------------------------------------------------------------------------------+
| main.rs                                                                          |
| - picks config/tide_scheduler.{ENV}.yaml or config/tide_scheduler.yaml           |
| - loads Config::from_file()                                                      |
+-----------------------------------+----------------------------------------------+
                                    |
                                    v
+----------------------------------------------------------------------------------+
| config/mod.rs                                                                    |
| - YAML -> replace_from_env() -> finish()                                         |
| - effective HTTP port = service.listen_port                                      |
|   env override: TIDESCHED_LISTEN_PORT / LISTEN_PORT                              |
+-----------------------------------+----------------------------------------------+
                                    |
                                    v
+----------------------------------------------------------------------------------+
| server/mod.rs :: Server::run()                                                   |
| - bind {listen_port}                                                             |
| - install Warp route: path!(String / "topicstats") + query + topicstats::execute |
+-----------------------------------+----------------------------------------------+
                                    |
                                    v
+----------------------------------------------------------------------------------+
| topicstats.rs :: execute(prefix, query_map, schedgroup_manager)                  |
| - prefix = "federation-1"                                                        |
| - query_map["topic"] = "doubao-trace-TUDP.pb"                                    |
| - prefix is currently ignored by lookup logic                                    |
+-----------------------------------+----------------------------------------------+
                                    |
                                    v
+----------------------------------------------------------------------------------+
| schedgroup/mod.rs :: SchedgroupManager::topic_stats(topic)                       |
| - topic_groups.get(topic)                                                        |
| - if found -> group.stats().await                                                |
| - if not found -> None                                                           |
+-----------------------------------+----------------------------------------------+
                                    |
                                    v
+----------------------------------------------------------------------------------+
| JSON response                                                                    |
| - success: { code: 0, message: "success", job_res_tg_stats: ... }                |
| - missing topic param: { code: -1, message: "no topic", job_res_tg_stats: {} }  |
| - unknown topic: success with empty map                                          |
+----------------------------------------------------------------------------------+
```

## 0.1) ASCII source-of-truth flow

`topicstats` is built from two upstream flows:

```text
                 DAG / control topology flow

    Job Manager
         |
         | gRPC: GetExecutorDAG
         v
    dag/mod.rs :: DAGReader::pull_impl()
         |
         v
    schedgroup/mod.rs
    - builds topic -> job/group mapping
    - decides which job IDs belong to a topic


                 Runtime state flow

    Task / worker
         |
         | gRPC: Heartbeat(stream HeartbeatReq)
         | gRPC: Statsreport(StatsreportReq)
         v
    server/mod.rs :: GrpcService
         |
         +--> heartbeat manager
         |    -> distributed.heartbeat(...)
         |
         +--> statsreport manager
              -> distributed.statsreport(...)
         |
         v
    distributed backend (usually Redis)
         |
         | periodic pull
         v
    statemgr/mod.rs :: StateManager
         |
         v
    schedgroup/group/mod.rs :: stats()
         |
         v
    /{prefix}/topicstats
```

## 1) URL to handler mapping

Endpoint shape:

- `/{prefix}/topicstats?topic={topic_name}`

Route registration is in `scheduler/src/server/mod.rs`:

- `warp::path!(String / "topicstats")` captures `{prefix}` (for example `federation-1`)
- `warp::query::<HashMap<String, String>>()` parses query params
- `and_then(topicstats::execute)` calls the handler

Current behavior detail:

- In `scheduler/src/server/topicstats.rs`, handler signature is:
  `execute(_prefix: String, p: HashMap<String, String>, schedgroup_manager: SharedSchedgroupManager)`
- `_prefix` is currently not used in lookup logic. It is only path matching input.
- `topic` query param is required:
  - missing `topic` -> `{ code: -1, message: "no topic", job_res_tg_stats: {} }`
  - has `topic` -> lookup stats and return `{ code: 0, message: "success", ... }`

For your query:

```bash
curl localhost:6789/stable/topicstats?topic=doubao-trace-FrontierX.pb
```

- `stable` is just the captured `{prefix}`
- the current handler does not use `stable` to filter data
- the real lookup key is the query parameter:
  `topic=doubao-trace-FrontierX.pb`

## 2) Lookup data path

Request flow after entering handler:

1. `topicstats::execute` reads `topic` from query map.
2. Calls `schedgroup_manager.read().await.topic_stats(topic).await`.
3. `SchedgroupManager::topic_stats` is in `scheduler/src/schedgroup/mod.rs`:
   - finds `topic_groups[topic]`
   - returns `group.stats().await` if exists
   - returns `None` if missing
4. Handler converts missing topic group to empty map in response.

So response data is from in-memory schedgroup state managed by `SchedgroupManager`, which is built and refreshed by DAG/state manager modules during server runtime.

## 2.1) How to read the sample result

Given a response like:

```json
{
  "job_res_tg_stats": {
    "b0332309-dde4-448c-96ab-7293395d9bc7": {
      "res_tg_states": {
        "groupid:4a43...;resourceid:[2605:...]:6510": {
          "num_available": 0,
          "avalive_queue_length": 0,
          "total_queue_length": 0,
          "rows_per_sec": 0.0
        }
      }
    }
  },
  "code": 0,
  "message": "success"
}
```

Interpretation:

- `code: 0`
  - request was handled successfully
- `message: "success"`
  - handler completed normally
- `job_res_tg_stats`
  - top-level map keyed by `job_id`
  - in your sample there is one matching job:
    `b0332309-dde4-448c-96ab-7293395d9bc7`
- `res_tg_states`
  - per-job map keyed by resource task-group identity
  - key format is:
    `groupid:{task_group_id};resourceid:{resource_address}`
- `groupid:...`
  - the DAG task-group ID
- `resourceid:[2605:...]:6510`
  - the resource address for one execution target
  - brackets are normal IPv6 formatting
  - `6510` / `7510` are part of the resource address string coming from DAG/task metadata
- `num_available`
  - count of alive tasks aggregated into that resource task-group
  - `0` means no currently alive task contributes to this bucket
- `avalive_queue_length`
  - aggregated available queue length from runtime reports
  - spelling follows the existing code field name
- `total_queue_length`
  - aggregated total queue length from runtime reports
- `rows_per_sec`
  - aggregated `num-rows-per-second` parsed from runtime indicators

What your sample specifically means:

- the topic resolves to at least one job
- that job currently has several resource task-group entries
- every shown entry is effectively inactive:
  - `num_available = 0`
  - queue lengths are `0`
  - throughput is `0.0`

Common reasons for an all-zero result:

- workers stopped sending `HeartbeatReq`
- workers never sent `StatsreportReq`
- Redis state expired and was pulled back as empty/default state
- the DAG still contains these resources, but no live runtime is attached right now

## 3) How server starts this HTTP endpoint

Main startup chain:

1. `scheduler/src/main.rs`
   - builds config filename from `ENV`:
     - `ENV=dev` -> `config/tide_scheduler.dev.yaml`
     - unset -> `config/tide_scheduler.yaml`
   - loads config via `Config::from_file(...)`
2. `scheduler/src/config/mod.rs`
   - `from_file` does: YAML load -> `replace_from_env()` -> `finish()`
3. `scheduler/src/server/mod.rs`
   - `Server::run()` binds `service.listen_port`
   - mounts `topic_stats_route`
   - starts Hyper server

Default port:

- `service.listen_port` default is `6789` (`Service::default_listen_port`).

Port env overrides:

- `TIDESCHED_LISTEN_PORT`
- `LISTEN_PORT`

The same listening socket serves both:

- HTTP/1.x requests -> Warp routes such as `/stable/topicstats`
- HTTP/2 requests -> Tonic gRPC methods such as `Heartbeat` and `Statsreport`

So this app receives `curl` traffic and worker gRPC traffic on the same main server port.

## 4) "How to get the server path?"

If you mean the full request URL path used by this endpoint, derive it as:

- `http://{host}:{listen_port}/{prefix}/topicstats?topic={topic}`

How to determine each part:

1. `{listen_port}`
   - first check env: `TIDESCHED_LISTEN_PORT` / `LISTEN_PORT`
   - otherwise check config YAML: `service.listen_port`
   - fallback default: `6789`
2. `{prefix}`
   - route requires any first path segment (`String`)
   - by convention, callers often pass cluster name (like `federation-1`)
   - in current code this segment is not used for filtering in `topicstats::execute`
3. `{topic}`
   - required query param, topic key in scheduler topic groups

Practical checks:

```bash
# 1) check effective port in env
echo "${TIDESCHED_LISTEN_PORT:-$LISTEN_PORT}"

# 2) if env empty, inspect selected config file by ENV
echo "${ENV:-<empty>}"
grep -n "listen_port" scheduler/config/tide_scheduler.${ENV}.yaml 2>/dev/null || \
grep -n "listen_port" scheduler/config/tide_scheduler.yaml

# 3) call endpoint
curl -s "http://127.0.0.1:6789/federation-1/topicstats?topic=doubao-trace-TUDP.pb"
```

## 4.1) How this app gets the control messages

There are two different kinds of upstream input that matter here.

### A. Topology / control data from Job Manager

This is how the scheduler learns:

- which jobs exist
- which task groups belong to each job
- which resource addresses belong to each task
- which topics map to which jobs/groups

Path:

```text
Job Manager
  -> TideJobManagerService.GetExecutorDAG
  -> dag/mod.rs :: DAGReader::pull_impl()
  -> internal job/task cache
  -> schedgroup manager rebuild
```

Important detail:

- this path is polled, not pushed
- `DAGReader::start()` keeps fetching from Job Manager on an interval

### B. Runtime status messages from tasks/workers

This is how the scheduler learns whether a resource is alive and how busy it is.

Path:

```text
worker/task
  -> gRPC Heartbeat(stream HeartbeatReq)
  -> gRPC Statsreport(StatsreportReq)
  -> server/mod.rs :: ServiceImpl
  -> heartbeat/statsreport manager
  -> distributed backend (Redis)
  -> periodic Redis pull
  -> StateManager
  -> topicstats output
```

Meaning of the two message types:

- `HeartbeatReq`
  - says a task is alive
  - eventually updates `alive = true`
  - contributes to `num_available`
- `StatsreportReq`
  - carries runtime metrics:
    - `queueAvailableLen`
    - `queueTotalLen`
    - `kv_indicators`
  - `kv_indicators["num-rows-per-second"]` becomes `rows_per_sec`

Key implementation notes:

- `server/mod.rs`
  - gRPC `heartbeat()` receives a stream of `HeartbeatReq`
  - gRPC `statsreport()` receives `StatsreportReq`
- `server/heartbeat/heartbeat.rs`
  - batches heartbeat updates and flushes them into distributed storage
- `server/statsreport.rs`
  - caches per-task stats briefly, then flushes them into distributed storage every 6 seconds
- `statemgr/distributed/sharedredis/mod.rs`
  - writes:
    - `"alive"` from heartbeat
    - `"runtime"` JSON from statsreport
  - reads them back and converts to internal `State`
- `statemgr/mod.rs`
  - aggregates per-task `State` into per-resource-task-group totals

### C. Important distinction

`topicstats` does **not** read worker messages directly from the gRPC request in real time.

Instead it reads the aggregated in-memory view after this pipeline:

```text
gRPC from workers
  -> distributed store
  -> state manager aggregation
  -> schedgroup topic lookup
  -> HTTP topicstats response
```

This is why you can see:

- a job/group/resource entry exists because DAG data is present
- but all counters are zero because runtime status is missing or expired

## 4.2) How to manually trigger these messages

The workspace already has a small gRPC client in `tests/client/`.

Examples:

```bash
# send heartbeat loop
cd tests/client
TOOL=heartbeat JOBID=x TASKID=y cargo run --bin toolset
```

```bash
# send one statsreport request
cd tests/client
TOOL=statsreport JOBID=x TASKID=y cargo run --bin toolset
```

If those `JOBID` / `TASKID` values do not exist in current DAG data, they will not show up meaningfully in `topicstats`.

## 5) Related files

- `scheduler/src/main.rs`
- `scheduler/src/config/mod.rs`
- `scheduler/src/server/mod.rs`
- `scheduler/src/server/topicstats.rs`
- `scheduler/src/server/statsreport.rs`
- `scheduler/src/server/heartbeat/heartbeat.rs`
- `scheduler/src/schedgroup/mod.rs`
- `scheduler/src/schedgroup/group/mod.rs`
- `scheduler/src/statemgr/mod.rs`
- `scheduler/src/statemgr/distributed/sharedredis/mod.rs`
- `scheduler/src/dag/mod.rs`
- `scheduler/proto/service.proto`
- `scheduler/proto/tide_control.proto`
- `readme.md` (example curl)
