# orca-netd

本机 Orca 桌面端连 tailnet 上的远端 runtime。netd 是用户态组网（`--tun=userspace-networking`），内核没有 `100.64.0.0/10` 路由，直连 CGNAT 地址会超时。

```text
Orca Electron net.Socket
        |  socks-cgnat-preload.js   (must stay JS)
        v
127.0.0.1:16768  forward.js
        |  SOCKS5 127.0.0.1:1055
        v
netd userspace ---- DERP ----> remote Orca :6768
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

## 边界

| 做了 | 没做 |
| --- | --- |
| 把 CGNAT TCP 转到固定的远端 Orca 端口 | 通用的逐目标 CGNAT 代理 |
| Tunnel Manager `auto_start`，`stop_on_exit=false` | 替换 netd，或改成 TUN |
| 离线 self-check（伪造 SOCKS） | 把 tailnet 通断写成健康检查 |

远端不在线时，SOCKS 会失败，转发日志写 `dial fail`。这不是转发器本身的故障。不要用内核 `ping` 判断 netd 对端。
