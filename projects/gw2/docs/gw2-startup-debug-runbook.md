# GW2 Gateway Thrift Service: Startup & Debug Runbook

This doc is the single source of truth for local compile/start/debug of this project. Always validate new features/bugfixes by running this flow end-to-end.

## 0. Conventions

- `$project_dir` means the repo root, e.g. `/path/to/gw2`.
- Export variables in your current shell, do not rely on IDE-internal executors.
- Record the service PID as `$gw_pid` once started.

## 1. Compile

### 1.1 Check whether port 8080 is occupied

```bash
lsof -i:8080
```

If a process exists, kill it and wait until it exits:

```bash
kill -TERM <pid>
sleep 1
lsof -i:8080
```

If it does not quit, force kill:

```bash
kill -KILL <pid>
sleep 1
lsof -i:8080
```

### 1.2 Build

```bash
cd "$project_dir"
bash build.sh
```

Expected:

- Output contains `BUILD SUCCESS`.

## 2. Run

### 2.1 Start gateway-thrift-service

```bash
export profile=dev
cd "$project_dir/output/gateway-thrift-service"
bash bootstrap.sh
```

Expected (startup success signal):

- Log contains: `Jet Server succeeds in binding to address:[0.0.0.0:8594]`
- Record the PID printed by the service as `$gw_pid`

If the startup runs in foreground and you need PID:

```bash
# In another shell:
ps -ef | grep -i gateway-thrift-service | grep -v grep
```

## 3. Verify HTTP Service (8080)

Check whether the HTTP service exists and the process matches `$gw_pid`:

```bash
lsof -i:8080
```

Expected:

- The process name/command matches the `$gw_pid` process.

## 4. Logs

The service log file is under the process working directory:

- `logs/gw2.log`

You can locate it by PID:

```bash
cat "/proc/$gw_pid/cwd/logs/gw2.log"
```

## 5. Debug SQL via HTTP

### 5.1 Create curl format file (timing metrics)

```bash
cat > curl-format.txt <<'EOF'
{
  "dns": %{time_namelookup},
  "connect": %{time_connect},
  "tls": %{time_appconnect},
  "ttfb": %{time_starttransfer},
  "total": %{time_total}
}
EOF
```

### 5.2 Query

```bash
curl -w "\n---METRICS---\n" -w @curl-format.txt -X POST http://localhost:8080/query_text \
  -H "Content-Type: text/plain" \
  --data-binary @- <<'EOF'
select
  stream as `stream_name`,
  sum(
    case
      when (is_sla_tag = 'true')
        and (session_type = 'sink')
        and (is_relay = 'false')
        and (local_node_type = 'pull')
        AND remote_type = 'user'
      then "count"
    end
  ) as `nvqos_metrics_pull_req_count`,
  toStartOfInterval(toDateTime(ts), interval 1 minute, 'Asia/Shanghai') as nvqos_timestamp
from ti.vqos_nodict
where ts >= '2026-04-08 08:00:00 +08:00'
  and ts < '2026-04-09 08:00:00 +08:00'
  and session_type = 'sink'
  and local_node_type = 'pull'
  and is_relay = 'false'
  and remote_type = 'user'
  and format not in ('webrtc','webrtc-rs')
  and response_status_code = '404'
  and volcano_account_id = '2100003528'
group by stream_name, nvqos_timestamp
order by `nvqos_metrics_pull_req_count` DESC
limit 2
EOF
```

Expected:

- HTTP 200 response (or expected error details for invalid SQL)
- `---METRICS---` section printed with timing numbers

## 6. Stop Service

```bash
kill -TERM "$gw_pid"
```

If it does not exit:

```bash
kill -KILL "$gw_pid"
```

