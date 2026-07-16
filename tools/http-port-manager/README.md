# HTTP Port Manager

多端口静态 HTTP 常驻服务的 Web 管理器（macOS launchd + SSE 指标）。

## 架构

```text
launchd com.user.http-port-manager
  -> python3 server.py
       Control UI/API  :9090
       Static workers  :8000, ...
```

## 快速安装

```bash
cd tools/http-port-manager
chmod +x install.sh server.py
./install.sh
```

`install.sh` 会把运行时文件同步到：

```text
~/Library/Application Support/http-port-manager/
```

（避免 launchd 读 `~/Documents` 触发 macOS TCC `Operation not permitted`。）

打开管理台：http://127.0.0.1:9090/

默认已配置 Algorithms Lab：http://127.0.0.1:8000/

### macOS TCC / Documents

LaunchAgent **不能读** `~/Documents`。本工具策略：

1. `install.sh` / `sync.sh` 在 **Terminal** 里把 `root` rsync 到  
   `~/Library/Application Support/http-port-manager/mirrors/<id>/`
2. LaunchAgent 只对外服务 mirror 目录
3. 改了 algorithms HTML 后执行：

```bash
tools/http-port-manager/sync.sh
```

可选：给 `/usr/bin/python3` 开「完全磁盘访问」，并去掉服务的 `force_mirror`，即可直读 `Documents`（无需 mirror）。

## API

| method | path | 说明 |
| --- | --- | --- |
| GET | `/api/healthz` | 健康检查 |
| GET | `/api/services` | 服务列表 + metrics |
| POST | `/api/services` | 创建（JSON: id,name,port,bind,root,auto_start） |
| POST | `/api/services/{id}/start` | 启动 |
| POST | `/api/services/{id}/stop` | 停止 |
| POST | `/api/services/{id}/restart` | 重启 |
| PATCH | `/api/services/{id}` | 更新配置 |
| DELETE | `/api/services/{id}` | 删除 |
| GET | `/api/events` | SSE：`event: update` / `: heartbeat` |
| POST | `/api/lab/events` | Algorithms Lab 埋点上报（CORS） |
| GET | `/api/lab/sessions` | 最近学习会话列表 |
| GET | `/api/lab/session?sessionId=` | 单会话详情 |
| GET | `/api/lab/coach?problemId=` | AI coach brief（interest/confusion/quiz） |

Lab 数据目录：`$INSTALL_DIR/lab_telemetry/`

## 运维

```bash
# 日志
tail -f ~/Library/Logs/http-port-manager/http-port-manager.out.log
tail -f ~/Library/Logs/http-port-manager/http-port-manager.err.log

# 重装 / 重载
./install.sh

# 卸载
launchctl bootout "gui/$(id -u)/com.user.http-port-manager"
rm -f ~/Library/LaunchAgents/com.user.http-port-manager.plist
```

## 配置

`config.json` 在启停/创建时自动写回。也可直接编辑后 `./install.sh` 重载。
