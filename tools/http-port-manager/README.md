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

网页 **不能** 给 python3 授予「完全磁盘访问」（TCC 绑的是进程，不是 localhost HTML）。

管理台在新建失败 / `needs_grant` 时提供授权交互：

1. **选择目录并写入镜像**（推荐）：浏览器文件选择器 = 用户手势授权，页面把文件 POST 到 `/api/services/{id}/seed-mirror`，写入 `mirrors/<id>/` 后启动。
2. Terminal 跑 `sync.sh`（可读 Documents）。
3. 可选：给该 python3 开「完全磁盘访问」，并去掉 `force_mirror`，才可直读 Documents。

## API

| method | path | 说明 |
| --- | --- | --- |
| GET | `/api/healthz` | 健康检查 |
| GET | `/api/access` | TCC 探测（Documents 是否可读、python 路径） |
| GET | `/api/services` | 服务列表 + metrics（含 `needs_grant` / `grant`） |
| POST | `/api/services` | 创建（JSON: id,name,port,bind,root,auto_start）。TCC 失败仍 201，服务进入 error + `needs_grant` |
| POST | `/api/services/{id}/start` | 启动 |
| POST | `/api/services/{id}/stop` | 停止 |
| POST | `/api/services/{id}/restart` | 重启 |
| POST | `/api/services/{id}/sync` | 尝试从 source rsync 到 mirror（LaunchAgent 下通常失败） |
| POST | `/api/services/{id}/seed-mirror` | 浏览器授权上传：`{reset,done,files:[{path,data|text}]}` |
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
