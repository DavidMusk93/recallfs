# Flow Scheduler Rust 环境手册

这份手册只回答一件事：

- 怎么把这个仓库需要的 Rust 环境装起来
- 怎么拉依赖、编译、运行、测试
- 遇到代理、registry、nightly、PATH 问题时怎么排查

## 1. 这个仓库对 Rust 的要求

仓库不是一个“随便装个 cargo 就能编”的普通 Rust 项目。

关键要求来自：

- [Cargo.toml](file:///root/Documents/flow-scheduler/scheduler/Cargo.toml)
- [.cargo/config.toml](file:///root/Documents/flow-scheduler/.cargo/config.toml)
- [build.sh](file:///root/Documents/flow-scheduler/build.sh)

核心约束：

- `scheduler/Cargo.toml` 使用 `edition = "2024"`
- `build.sh` 会切到 `nightly`
- `build.sh` 使用 `-Z unstable-options`
- 仓库依赖内部 registry：`rust-preonline.byted.org`

结论：

- 日常开发建议安装 `rustup`
- 至少准备一套 `stable`
- 实际编译这个仓库时，优先使用 `nightly`

## 2. 总体流程

```text
+-------------------+
| install rustup    |
+-------------------+
          |
          v
+-------------------+
| install nightly   |
+-------------------+
          |
          v
+-------------------+
| verify cargo/env  |
+-------------------+
          |
          v
+-------------------+
| cargo fetch       |
| internal registry |
+-------------------+
          |
          v
+-------------------+
| cargo build/test  |
| or ./build.sh     |
+-------------------+
          |
          v
+-------------------+
| run scheduler     |
+-------------------+
```

## 3. 代理准备

如果下载 `rustup`、toolchain 或内部 registry 失败，可以先设置代理。

你给出的代理信息可以整理成下面这种可直接执行的形式：

```bash
export http_proxy='http://sys-proxy-rd-relay.byted.org:8118'
export https_proxy='http://sys-proxy-rd-relay.byted.org:8118'
export no_proxy='localhost,.byted.org,byted.org,.bytedance.net,bytedance.net,127.0.0.1,127.0.0.0/8,169.254.0.0/16,100.64.0.0/10,172.16.0.0/12,192.168.0.0/16,10.0.0.0/8,::1,fe80::/10,fd00::/8'
```

如果你喜欢函数方式，也可以放到 shell 配置里：

```bash
_proxy() {
  export http_proxy='http://sys-proxy-rd-relay.byted.org:8118'
  export https_proxy='http://sys-proxy-rd-relay.byted.org:8118'
  export no_proxy='localhost,.byted.org,byted.org,.bytedance.net,bytedance.net,127.0.0.1,127.0.0.0/8,169.254.0.0/16,100.64.0.0/10,172.16.0.0/12,192.168.0.0/16,10.0.0.0/8,::1,fe80::/10,fd00::/8'
}
```

## 4. 安装 rustup 和 Rust

推荐方式是 `rustup`。

### 4.1 安装 rustup

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/rustup-init.sh
sh /tmp/rustup-init.sh -y --profile minimal
```

如果你希望把安装目录固定到某个位置，可以显式指定：

```bash
export CARGO_HOME="$HOME/.cargo"
export RUSTUP_HOME="$HOME/.rustup"
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/rustup-init.sh
sh /tmp/rustup-init.sh -y --profile minimal
```

### 4.2 设置 PATH

安装完成后，确保：

```bash
export PATH="$HOME/.cargo/bin:$PATH"
```

验证：

```bash
cargo --version
rustc --version
rustup --version
```

### 4.3 安装 nightly

```bash
rustup toolchain install nightly --profile minimal
```

验证：

```bash
cargo +nightly --version
rustc +nightly --version
rustup toolchain list
```

## 5. 这个仓库的 cargo registry 配置

仓库已经自带 registry 配置：

- [.cargo/config.toml](file:///root/Documents/flow-scheduler/.cargo/config.toml)

关键点：

- `crates-io` 被替换为内部源 `byted`
- `crates-byted` 也被映射到内部 registry
- `git-fetch-with-cli = true`

所以通常不需要你手动改 Cargo registry。

但前提是：

- 当前机器能访问这些内部地址
- 代理或网络策略没有拦住它们

## 6. 先拉依赖

第一次建议先执行：

```bash
cd /root/Documents/flow-scheduler
cargo +nightly fetch --manifest-path scheduler/Cargo.toml
```

这样可以尽早暴露问题：

- registry 访问失败
- 代理没配好
- 凭证/网络问题
- nightly 没装好

如果只想确认依赖已经下载到本地，也可以看：

```bash
ls ~/.cargo/registry/src
```

## 7. 编译方式

这个仓库有两套常用编译方式。

### 7.1 直接 cargo 编译

在 workspace 根目录：

```bash
cd /root/Documents/flow-scheduler
cargo +nightly build
```

只编 `scheduler`：

```bash
cd /root/Documents/flow-scheduler
cargo +nightly build -p scheduler
```

如果你想尽量贴近 `build.sh` 的编译参数：

```bash
cd /root/Documents/flow-scheduler
RUSTFLAGS="--cfg tokio_unstable -Z unstable-options -C split-debuginfo=off" \
  cargo +nightly build --features redirect --no-default-features --release
```

### 7.2 使用仓库自带脚本

最贴近打包流程的是：

```bash
cd /root/Documents/flow-scheduler
./build.sh
```

这个脚本会做这些事情：

- `rustup default nightly`
- 设置 `RUSTFLAGS="--cfg tokio_unstable -Z unstable-options -C split-debuginfo=off"`
- release 编译
- 复制二进制到 `output/`
- 复制运行脚本、配置、依赖库、`version.txt`

对应脚本：

- [build.sh](file:///root/Documents/flow-scheduler/build.sh)

## 8. 测试命令

workspace 测试：

```bash
cd /root/Documents/flow-scheduler
cargo +nightly test
```

只测 `scheduler`：

```bash
cd /root/Documents/flow-scheduler
cargo +nightly test -p scheduler
```

只测 `toolset`：

```bash
cd /root/Documents/flow-scheduler
cargo +nightly test -p toolset
```

如果只是做最小健康检查，建议优先：

```bash
cd /root/Documents/flow-scheduler
cargo +nightly test -p scheduler
```

## 9. 本地运行

### 9.1 直接运行开发态

服务配置从当前工作目录下的 `config/` 读。

开发态通常这样起：

```bash
cd /root/Documents/flow-scheduler/scheduler
ENV=dev cargo +nightly run
```

如果只想显式和 `build.sh` 保持一致，可以带 feature：

```bash
cd /root/Documents/flow-scheduler/scheduler
ENV=dev cargo +nightly run --features redirect --no-default-features
```

### 9.2 从打包目录运行

先打包：

```bash
cd /root/Documents/flow-scheduler
./build.sh
```

再进入 `output/`：

```bash
cd /root/Documents/flow-scheduler/output
./run.sh
```

`run.sh` 会做：

- 设置 `LD_LIBRARY_PATH`
- 运行 `./scheduler`

对应脚本：

- [run.sh](file:///root/Documents/flow-scheduler/run.sh)

## 10. toolset 的使用

workspace 里还有一个调试工具二进制 `toolset`。

打包后会被复制到：

- `output/toolset`

源码成员在：

- `tests/client/`

典型用法是通过环境变量 `TOOL` 指定行为，例如：

- `version`
- `heartbeat`
- `statsreport`
- `pull_key_from_redis`

## 11. Tokio Console

如果你需要 Tokio Console：

```bash
cd /root/Documents/flow-scheduler
TOKIO_DEBUG=1 ./build.sh
```

脚本会：

- 打开 `tokioconsole` feature
- 安装 `tokio-console`

相关脚本：

- [build.sh](file:///root/Documents/flow-scheduler/build.sh)
- [run_tokio_console.sh](file:///root/Documents/flow-scheduler/scripts/tokio/run_tokio_console.sh)

## 12. 常见问题

### 12.1 `cargo: command not found`

通常是 `PATH` 没带上 `~/.cargo/bin`。

修复：

```bash
export PATH="$HOME/.cargo/bin:$PATH"
```

### 12.2 `rustup: command not found`

说明 `rustup` 没装好，或者 PATH 没生效。

先检查：

```bash
ls -la ~/.cargo/bin
```

### 12.3 下载 `rustup` / toolchain 很慢或失败

先开代理，然后重试：

```bash
export http_proxy='http://sys-proxy-rd-relay.byted.org:8118'
export https_proxy='http://sys-proxy-rd-relay.byted.org:8118'
```

### 12.4 `cargo fetch` 卡在内部 registry

优先检查：

- 能否访问 `rust-preonline.byted.org`
- 代理是否生效
- `.cargo/config.toml` 是否被本地别的配置覆盖

最小验证：

```bash
cd /root/Documents/flow-scheduler
cargo +nightly fetch --manifest-path scheduler/Cargo.toml
```

### 12.5 `build.sh` 失败但普通 cargo build 成功

优先看这几个点：

- 你是不是用了 `stable` 而不是 `nightly`
- `RUSTFLAGS` 里的 `-Z unstable-options` 是否被支持
- `target/release` 里是否真的生成了目标文件

因为 `build.sh` 比普通 `cargo build` 更依赖：

- `nightly`
- release 目录结构
- 打包复制路径

### 12.6 `run.sh` 能跑但开发态 `cargo run` 不行

优先检查：

- 当前目录是不是 `scheduler/`
- `ENV=dev` 是否设置
- 当前目录下的 `config/` 是否存在

因为开发态读取配置是相对路径行为。

### 12.7 shell 环境有包装，导致安装脚本行为异常

如果你的终端环境对 `sh`、`bash`、`curl` 做了函数包装，可以改用绝对路径：

```bash
/usr/bin/curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/rustup-init.sh
/usr/bin/sh /tmp/rustup-init.sh -y --profile minimal
```

必要时可用隔离环境执行：

```bash
/usr/bin/env -i HOME="$HOME" PATH="/usr/bin:/bin:$HOME/.cargo/bin" /usr/bin/bash --noprofile --norc
```

## 13. 推荐操作顺序

第一次在新机器上建议直接照这个顺序做：

```bash
export http_proxy='http://sys-proxy-rd-relay.byted.org:8118'
export https_proxy='http://sys-proxy-rd-relay.byted.org:8118'
export no_proxy='localhost,.byted.org,byted.org,.bytedance.net,bytedance.net,127.0.0.1,127.0.0.0/8,169.254.0.0/16,100.64.0.0/10,172.16.0.0/12,192.168.0.0/16,10.0.0.0/8,::1,fe80::/10,fd00::/8'

curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/rustup-init.sh
sh /tmp/rustup-init.sh -y --profile minimal

export PATH="$HOME/.cargo/bin:$PATH"
rustup toolchain install nightly --profile minimal

cd /root/Documents/flow-scheduler
cargo +nightly fetch --manifest-path scheduler/Cargo.toml
cargo +nightly build -p scheduler
cargo +nightly test -p scheduler

cd /root/Documents/flow-scheduler/scheduler
ENV=dev cargo +nightly run
```

如果你要做完整打包验证，再执行：

```bash
cd /root/Documents/flow-scheduler
./build.sh
cd output
./run.sh
```
