# orca-netd

本机 Orca 桌面端连 tailnet 上的远端 runtime。netd 是用户态组网（`--tun=userspace-networking`），内核没有 `100.64.0.0/10` 路由，直连 CGNAT 地址会超时。

```text
Orca Electron net.Socket
        |  socks-cgnat-preload.js   (must stay JS)
        |  first bytes: ORCA1 <cgnat>:6768
        v
127.0.0.1:16768  forward.js
        |  SOCKS5 127.0.0.1:1055
        v
netd userspace ---- DERP ----> that runtime :6768

没有这行头时，仍拨 `ORCA_NETD_DEST_HOST`（现在是 cc）。头只接受 `100.64.0.0/10` 的 `:6768`，所以同一个监听可以同时到 cc 和 d2。
```

预加载留在 JavaScript：它改的是本进程里 Electron 的 `net.Socket.prototype.connect`，还要解开 Node 24 已经规范化过的参数数组。转发进程由 Tunnel Manager 的 `orca-netd` 服务拉起，不再用 LaunchAgent。

配对用的 `deviceToken`、`publicKey`、本机 `authToken` 不进仓库，也不进 `config.env`。`config.env` 只保存目标主机和端口，由 `install.sh` 从已保存的 Orca environment 里的 websocket URL 写出，权限 `0600`。

## 安装

```bash
node projects/orca-netd/scripts/self-check.js
bash projects/orca-netd/install.sh
curl -sS http://127.0.0.1:9020/api/tunnel/orca-netd
```

运行时副本在 `~/.local/share/orca-netd/`。Tunnel Manager 读不了 `~/Documents` 时仍能启动这份副本。`install.sh` 会卸掉旧的 `orca-netd-forward` LaunchAgent，再 `POST /api/reload` 和 `POST /api/tunnel/orca-netd/start`。

Orca 升级覆盖 `app.asar` 后，先退出 Orca，再跑：

```bash
node projects/orca-netd/scripts/patch-asar.js
```

需要本机已有的 `@electron/asar`（默认 `/tmp/orca-asar-tool`）。脚本会改 `ElectronAsarIntegrity` 并 `codesign --force --sign -`。

## 用 CLI 操作远端 runtime

`orca` CLI 跑在 `ELECTRON_RUN_AS_NODE` 下，且 `Resources/bin/orca` 会 unset `NODE_OPTIONS`。CGNAT 预加载只挂在桌面主进程上，所以 CLI 进程没有到 `100.64.0.0/10` 的路由：

```text
orca --environment cc-orca        plain CLI process
        |  no preload -> no rewrite
        X  connect 100.90.17.5:6768 (no kernel route, userspace netd)
        =  remote_runtime_unavailable

scripts/orca-cli.sh --environment cc-orca
        |  NODE_OPTIONS=--require socks-cgnat-preload.js
        v
        127.0.0.1:16768 -> SOCKS5 127.0.0.1:1055 -> cc-orca:6768   OK
```

用包装脚本代替裸 `orca`：

```bash
projects/orca-netd/scripts/orca-cli.sh host list
projects/orca-netd/scripts/orca-cli.sh repo add --environment cc-orca --path /root/recallfs --json
projects/orca-netd/scripts/orca-cli.sh worktree create --environment cc-orca --repo id:<repo-id> \
  --name <worktree> --agent cursor --prompt "..." --setup skip --json
projects/orca-netd/scripts/orca-cli.sh terminal list --environment cc-orca
```

本机 CLI（不带 `--environment`）只看本机 runtime：`worktree list` 会打印 `scope: none — not covered: local (--host local), runtime:<id> (--environment cc-orca)`。远端 worktree/terminal 要用 `--environment` 或 `--host runtime:<environment-id>` 才看得到。

注意方向：`--environment`/`--host` 让 CLI 直接成为远端 runtime 的客户端，agent 启动参数由**远端** settings 组装（cc 上 `cursor` 默认是 `--yolo`，模型来自 cc 上 cursor-agent 自己的配置）。Mac 客户端在 UI 里发起的会话则由**本机** settings 组装（`agentDefaultArgs.cursor`），两者都要各自正确。

## 客户端状态：向导、默认代理、执行主机

| 项 | 位置 | 说明 |
| --- | --- | --- |
| 向导开关 | `profiles/<id>/profile-state.db` → `onboarding` 文档 `closedAt` | `closedAt === null` 时渲染 1/3 向导（`Dy`）；置成时间戳即关闭 |
| 默认代理 | 同库 `settings` 文档 `defaultTuiAgent` | 向导文案里「Cursor isn't on your PATH」查的是**本机** PATH，不是远端 |
| Yolo | `settings.agentDefaultArgs.cursor` | yolo 只是 `--yolo` 这一类参数；保留 `--model ...` 就不要走向导的 agent 步（会按 yolo 默认重写参数） |
| 执行主机 | 同库 `workspaceSessionsByHostId` → `runtime:<environment-id>` | 远端 tab 的 `ptyId` 是 `remote:<environment-id>@@<term>`，`unifiedTabs[].executionHostId` 也是 `runtime:<environment-id>` |

改 `profile-state.db` 前必须退出 Orca（退出时会用内存覆盖），并且只改 `payload` 不够：`content_hash` 是 `sha256(payload)`，读回会校验（`Profile state document hash mismatch`），`revision` 必须保持一致、不能大于 `profile_state_meta.revision`。

## 边界

| 做了 | 没做 |
| --- | --- |
| 把 CGNAT TCP 转到固定的远端 Orca 端口 | 通用的逐目标 CGNAT 代理 |
| Tunnel Manager `auto_start`，`stop_on_exit=false` | 替换 netd，或改成 TUN |
| 离线 self-check（伪造 SOCKS） | 把 tailnet 通断写成健康检查 |
| CLI 侧同样加载预加载（`scripts/orca-cli.sh`） | 改 CLI 二进制，或让 CLI 走 TUN |

远端不在线时，SOCKS 会失败，转发日志写 `dial fail`。这不是转发器本身的故障。不要用内核 `ping` 判断 netd 对端。

用 `scripts/orca-cli.sh` 之外的方式跑 CLI（比如裸 `orca`）时，到 CGNAT 地址的连接不会经过转发器，报的是 `remote_runtime_unavailable`，不要据此判断远端挂了。
