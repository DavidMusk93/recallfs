---
doc_id: recallfs-study-kache-build-cache-v1
kind: study
status: active
authority: evidence
applies_to:
  - learning/studies/20260910-kache-build-cache
depends_on:
  - learning/studies/20260910-kache-build-cache/source.md
  - learning/studies/20260910-kache-build-cache/exploration.md
supersedes: []
verified_by:
  - learning/studies/20260910-kache-build-cache/demo/verify.sh
  - learning/studies/20260910-kache-build-cache/evidence/direct-cache-report.json
  - learning/studies/20260910-kache-build-cache/evidence/blade-without-allowlist-report.json
  - learning/studies/20260910-kache-build-cache/evidence/blade-with-allowlist-report.json
---

# Kache 构建缓存原理与 Blade、Rust、Jenkins 适配

> 研究对象：[kunobi-ninja/kache](https://github.com/kunobi-ninja/kache)，
> `v0.19.0`，commit `89b0c738115534156e759f157d0ce681b282d484`。
>
> 本文区分上游能力、当前机器复现实验和对生产 Jenkins 的工程建议。

## 1. 结论先行

Kache 不是“缓存整个构建目录”的工具，也不是远程执行系统。它是一个位于构建
系统与真实编译器之间的 compiler wrapper：

1. 从一次编译调用中找出会影响输出的输入；
2. 归一化工作区、Cargo home、target 等机器本地路径；
3. 计算 BLAKE3 key；
4. 命中时从本地 content-addressed store 恢复产物；
5. 未命中时执行真实编译器、存储产物，并可异步发布到 S3-compatible 或
   filesystem remote。

适配结论如下：

| 场景 | 结论 | 条件 |
| --- | --- | --- |
| Cargo/Rust | 接近开箱即用 | 设置 `RUSTC_WRAPPER=kache` 或执行 `kache init` |
| 普通 GCC/Clang C++ | 接近开箱即用 | 用 PATH shims，或设置 `CC="kache cc"`、`CXX="kache c++"` |
| 开源 Blade C++ | **默认不能产生 cache hit** | Blade 编译规则带 `-H`，kache 0.19.0 默认将其视为未知 flag 并 passthrough |
| 开源 Blade + 项目配置 | 可接入，但需验证 | `.kache.toml` allowlist `-H` 后，普通业务 TU 可跨工作区命中 |
| 内部 Blade + Sailfish/Goma/CAS | 不能声称开箱即用 | 必须先确认真实编译命令、wrapper 顺序、CAS 所有权和 passthrough 原因 |
| Jenkins | 无专用插件，但 shell 集成可行 | 临时 runner 需要 remote 或持久卷；每个 executor 必须使用独立 runtime dir |

因此建议是：**把 Kache 作为 Rust 的直接候选、Blade C++ 的受控实验候选，
不要直接替换现有 Blade CAS/Goma。** 先在一条代表性 Jenkins job 上完成
interception、正确性、缓存覆盖率和 wall-time 四项验证，再决定推广。

## 2. Kache 的核心原理

### 2.1 它拦截的是编译器，不是构建系统

```text
Cargo / Blade / Ninja / Make
             |
             v
      compiler invocation
             |
             v
      Kache wrapper
       |           |
       | hit       | miss
       v           v
 restore output   real compiler
       |           |
       |           v
       |      content-addressed store
       |           |
       +-----------+
             |
             v
        build continues
```

Cargo 通过 `RUSTC_WRAPPER` 进入 Kache。C/C++ 通过 `kache cc`、
`kache c++` 或 `gcc`、`clang++` 等 PATH shim 进入 Kache。Kache 不接管
Cargo、Blade 或 Ninja 的依赖图，也不缓存任意 codegen、归档、链接和测试步骤。

这决定了它的收益上限：

$$
T_{build}=T_{graph}+T_{codegen}+T_{compile}+T_{link}+T_{test}
$$

Kache 主要压缩其中可缓存的 $T_{compile}$，并只在符合策略时缓存部分 Rust
link output。若 Jenkins 时间主要消耗在依赖下载、Blade graph、代码生成、
超长链接或测试，单独引入 Kache 不会解决主瓶颈。

### 2.2 Rust cache key

可把 Rust key 简化为：

$$
K_{rust}=H(V, C, T, A, S, E, L, X)
$$

其中：

- $V$：cache-key schema；
- $C$：rustc identity；
- $T$：target triple 与 target spec；
- $A$：crate type、edition、features、cfg、emit、codegen flags；
- $S$：dep-info 发现的 source/include 文件路径与内容；
- $E$：`--extern` 依赖产物内容；
- $L$：需要链接时的 linker、libc、CRT 或 SDK identity；
- $X$：声明的环境变量、额外输入和 key salt。

Kache 会把已知工作区、Cargo home、Rustup home、临时目录和 target 目录映射到
稳定 sentinel，同时要求编译器在产物中使用对应的 remapped path。因此相同
内容可在不同 worktree 或 Jenkins workspace 复用。

关键边界是 hidden input。proc macro、build script 或外部工具读取了 rustc
没有报告的文件/环境时，应使用 `kache.toml`、`workspace.extra_inputs`、
`cache.key_env_vars` 或 `cache.key_salt` 显式建模。否则不是低命中率问题，
而是潜在错误命中问题。

### 2.3 C/C++ cache key

C/C++ 的负载输入是预处理结果：

$$
K_{cc}=H(V, C, P, T, F, D, I)
$$

- $C$：compiler family、版本和探测出的真实 invocation；
- $P$：`-E -P` 或 `/EP` 得到的预处理输出 hash；
- $T$：目标架构；
- $F$：已建模的 codegen flags；
- $D$：dep-info 输出形态；
- $I$：include shadowing 风险摘要。

预处理输出覆盖源文件、传递头文件和宏展开。Kache 还保存 preprocess memo：
当已记录输入的 metadata/content 未变化时，可以复用上次的预处理 hash，
避免每次 hit 都重新完整预处理。

它采用 fail-closed 策略。单源 `-c` 目标文件编译才是主要缓存形态；未知参数、
response file、PCH、modules、coverage、split DWARF、CUDA/HIP、多源编译和
C/C++ 链接会 passthrough 到真实编译器。passthrough 保证构建继续，但不会
产生加速。

### 2.4 Store、恢复和远端

本地 store 包含：

- SQLite `index.db`，记录 entry 与 blob 的关系；
- `store/blobs/<prefix>/<hash>`，按内容 hash 保存输出；
- `store/<cache-key>/`，保存 entry metadata。

同样的输出 bytes 只保存一份。恢复按安全顺序尝试：

1. CoW clone/reflink；
2. 对有限的 immutable Unix artifact 使用 hardlink；
3. byte copy。

可执行文件和动态库不会使用 hardlink fallback，以防签名、strip 或后续工具
原地修改缓存内容。daemon 负责远端 exact lookup、下载、后台上传、prefetch
和定期 GC；daemon 失败时本地缓存仍可工作。

并发 miss 还会经过 machine-wide flight、memory-weighted permit 和 per-key
lock。等待中的同 key 编译会在获得锁后再次查询 store，从而避免重复编译。

## 3. 本机复现实验

环境：

| 项目 | 值 |
| --- | --- |
| OS | macOS 26.6.2, Apple silicon |
| Kache | 0.19.0 release binary |
| Rust | rustc/cargo 1.97.0 |
| C++ | Apple Clang 21.0.0 |
| Blade reference | public Blade commit `b95bff3e...` |

运行：

```bash
KACHE_BIN=/path/to/kache \
BLADE_BIN=/path/to/blade \
./learning/studies/20260910-kache-build-cache/demo/verify.sh
```

### 3.1 Rust 与直接 C++

同一源码被复制到两个独立 Rust workspace；C++ object 也在删除第一次输出后
重新请求。结果：

| Workload | First | Second | Same key | Real compiler on second |
| --- | ---: | ---: | --- | --- |
| Rust | miss, 551 ms | local hit, 90 ms | yes | no |
| direct C++ | miss, 818 ms | local hit, 19 ms | yes | no |

报告合计 2 misses、2 local hits、0 errors。Rust 恢复约 2.8 MiB 走 APFS
reflink；很小的 C++ object 走 copy。见
[`direct-cache-report.json`](evidence/direct-cache-report.json)。

这是机制 smoke test，不是性能 benchmark。项目太小、样本只有一次，数字不能
外推到生产。

### 3.2 Blade 默认配置

公共 Blade 的 Ninja compile rule 在真实 compiler command 尾部增加 `-H`，
用 stderr 中的 include stack 做依赖检查。仅安装 Kache PATH shims 后：

```text
Blade compile
    |
    +-- -H
    |
    v
Kache 0.19.0 flag classifier
    |
    v
passthrough: unsupported flag(s): -H
```

两份相同 Blade workspace 的结果为 0 cacheable、0 hit、44 passthrough；业务
TU `answer.cpp` 两次都直接编译。见
[`blade-without-allowlist-report.json`](evidence/blade-without-allowlist-report.json)。

所以“Blade 只要把 shim 放到 PATH 就开箱即用”是错误结论。

### 3.3 Blade 项目级修复

实验加入：

```toml
[cc]
extra_allowlist_flags = ["-H"]
```

第二份 workspace 的业务 TU 使用了与第一份相同的 key，并得到
`local_hit`。Blade 两次生成的 `answer.cpp.incstk` byte-identical，说明本样例
中 Kache 对缓存 stderr 的回放保留了 Blade 需要的 include-stack 行为。

仍有三个边界：

- `scm.cc` 是路径相关生成文件，两份 workspace 的 key 不同；
- Blade 的 header-only preprocess、工具链探测和 link 等步骤继续 passthrough；
- 微型样例中业务 TU hit 为 1294 ms，而对应 miss 为 716 ms，不能据此声称
  性能收益。

见
[`blade-with-allowlist-report.json`](evidence/blade-with-allowlist-report.json)。
真实 Blade fork 还必须检查自己的 flags、response files、compiler wrapper 和
CAS 路径，不能直接照搬 `-H` allowlist。

## 4. 是否开箱即用

### 4.1 Rust

对标准 Cargo build，答案是“基本可以”：

```bash
export RUSTC_WRAPPER=kache
cargo build --locked
```

但以下场景需要额外处理：

- ephemeral Jenkins runner 没有持久 local store，需要 filesystem remote 或 S3；
- proc macro/build script 有隐藏输入，需要显式声明；
- 自定义 target、Emscripten、无法固定 runtime inputs 的 host binary 可能
  passthrough；
- 持久 workspace 的原生 incremental build 可能已经很快，应与 Kache 的
  adaptive incremental 策略实测比较；
- `cargo install kache` 要求 Rust 1.95+，CI 更适合使用带 checksum 的固定
  release binary。

### 4.2 普通 C++

对标准 GCC、Clang、Apple Clang、clang-cl 的单源 object compile，答案是
“需要一次 wrapper 配置后可用”：

```bash
kache install-shims
export PATH="$HOME/.local/lib/kache/shims:$PATH"
```

或：

```bash
export CC="kache cc"
export CXX="kache c++"
```

需要先用 `kache doctor` 和 verbose build 证明真实编译器调用经过 Kache。
构建系统如果固定 `/usr/bin/clang++`、使用私有 wrapper、把 compile flags
放进 `@response.rsp`，或主要使用 PCH/modules/CUDA，则不能视为开箱即用。

### 4.3 Blade

对本文固定的公共 Blade revision：

- toolchain 未指定 `prefix` 或绝对 `cc/cxx` 时，会从 PATH 解析 compiler，
  因而 Kache shims 能拦截；
- compile rule 不使用 response file，但固定带 `-H`；
- link rule使用 response file，不过 Kache 本来也不缓存 C/C++ link；
- `.kache.toml` allowlist `-H` 后，普通业务 TU 在 smoke test 中命中。

对内部 Blade，先回答下面四个问题：

1. verbose command 的首个 compiler token 是 shim、绝对路径还是另一层 wrapper？
2. Sailfish/Goma/CAS 是否已经拥有 compile cache 或 remote execution？
3. 有多少 compile 因 unknown flag、response file、coverage、PCH、module 等
   passthrough？
4. 缓存命中后，Blade 依赖的 stderr、depfile、include stack 是否仍正确？

若现有 CAS/Goma 健康，优先修复和度量现有链路。并排叠加两个 cache wrapper
会增加 key 计算、预处理、网络和排障复杂度。只有现有远端链路不可用、Kache
覆盖率可证明、或作为明确 fallback/migration 层时才值得引入。

## 5. Jenkins 落地模型

Jenkins 不是 Kache 自动识别的受信 CI。Kache 0.19.0 只对 GitHub Actions
和 GitLab CI 内建“非 protected branch 强制只读”策略。因此 Jenkins 必须
自行实施 trust boundary：

```text
job starts
    |
    v
pin binary + verify checksum
    |
    v
classify trust (PR / branch / protected branch)
    |
    +--> untrusted: remote read-only
    |
    +--> trusted master: remote read-write
    |
    v
unique runtime dir per executor
    |
    v
install compiler interception
    |
    v
doctor + build + correctness gates
    |
    v
report + metrics + trusted sync push
```

推荐职责划分：

| State | Lifetime | Recommended location |
| --- | --- | --- |
| `KACHE_RUNTIME_DIR` | 单次 executor/build | workspace temp 下带 `BUILD_TAG` 的独立目录 |
| `KACHE_CACHE_DIR` | runner 生命周期 | 本机持久盘；并发 job 可共享 |
| Remote cache | 跨 runner | S3-compatible bucket 或受控共享文件系统 |
| Build output | 单次 checkout | Cargo target 或 Blade build dir，不作为 Kache remote |

最小 shell 结构：

```bash
export RUSTC_WRAPPER=/opt/kache/bin/kache
export KACHE_RUNTIME_DIR="${WORKSPACE_TMP}/kache-${BUILD_TAG}"
export KACHE_CACHE_DIR="/var/cache/kache/${NODE_NAME}"

if [ -n "${CHANGE_ID:-}" ] || [ "${BRANCH_NAME:-}" != "master" ]; then
  export KACHE_REMOTE_READONLY=1
else
  export KACHE_REMOTE_READONLY=0
fi

kache doctor --json > kache-doctor.json
kache sync --pull

# Rust
cargo build --locked

# C++ / Blade, after compatibility qualification
kache install-shims --force "$WORKSPACE_TMP/kache-shims"
export PATH="$WORKSPACE_TMP/kache-shims:$PATH"
blade build //path:target --verbose

kache report --format json --output kache-report.json
if [ "$KACHE_REMOTE_READONLY" = 0 ]; then
  kache sync --push
fi
```

生产配置还应做到：

- 通过 node image 或工具仓固定 Kache 版本和 SHA-256；
- 使用 instance/workload identity，避免长期 S3 key；
- bucket/prefix 按组织、toolchain ABI 和 trust domain 隔离；
- PR 只读，只有受保护 `master` job 能写；
- remote 故障是否 fail-open 必须显式决定并报警；
- 每个 executor 独立 runtime dir，避免 socket/session 相互污染；
- 保存 `kache report --format json` 为 Jenkins artifact；
- 对一次完整 build 使用同一个 `KACHE_EVENT_ROOT`，方便按 job 过滤报告。

## 6. Agent 应如何优化构建

Agent 不应从“安装 Kache”开始，而应执行下面的证据闭环：

1. **定位时间**：拆分 checkout/dependency、graph/codegen、compile、link、
   test、package 阶段 wall time。
2. **证明拦截**：查看 verbose compiler command，并从 report 证明出现
   cacheable miss，而不是只有 passthrough。
3. **正确性资格测试**：固定 revision 和 toolchain，比较无缓存 build 与
   cache-hit build 的测试结果；高风险 rollout 使用 `KACHE_VERIFY=1` 抽样
   重编译比较 Rust hit。
4. **测三种状态**：cold、local hit、remote hit 分开，至少一次 warm-up 后
   比较多次中位数和 p95。
5. **解释 miss**：Rust 用 `kache why-miss`；C++ 看 report 的 passthrough
   reason 和 key trace。
6. **控制信任**：不受信 job 永远只读，受信分支才能发布。
7. **给出决策**：收益、覆盖率、正确性、存储/网络成本和回滚方式必须同时呈现。

本研究配套 Skill：

[`skills/build/build-cache-acceleration/SKILL.md`](../../../skills/build/build-cache-acceleration/SKILL.md)

## 7. 生产验收指标

不能只看 hit count。至少记录：

| Metric | Why |
| --- | --- |
| end-to-end wall time p50/p95 | 最终用户收益 |
| compile-weighted hit rate | 避免大量廉价 TU 掩盖昂贵 miss |
| cacheable coverage | 发现 wrapper 未拦截或大量 passthrough |
| key/lookup/restore/store time | 判断 cache overhead |
| remote bytes、latency、failures | 判断网络是否抵消收益 |
| duplicate outputs | 发现 key 过度具体 |
| correctness gate result | 阻止 stale/poisoned artifact |
| local/remote storage growth | 控制成本与 GC |

建议用同一 revision、toolchain、runner class 和并发度，依次执行：

```text
baseline without cache
        |
        v
cold cache build
        |
        v
fresh output + warm local store
        |
        v
fresh runner/output + warm remote
        |
        v
source-change rebuild
```

只有代表性 workload 的 wall time 有稳定改善，且 correctness 与可靠性门禁不
退化时，才扩大 rollout。

## 8. 风险与边界

- Kache 项目迭代很快，本文只对 `v0.19.0` 负责。
- 本地实验没有覆盖 Linux Jenkins、S3、MinIO、远端并发和断网恢复。
- 本地 Blade 是公共实现，不代表内部 fork。
- Kache 不是 remote execution，miss 仍在当前 runner 编译。
- 它不缓存任意 link/codegen/test；超长 link 仍需 ThinLTO cache、链接器或
  target 拆分等独立优化。
- `extra_allowlist_flags` 是 correctness 承诺，不应由 Agent 为追求 hit rate
  自动扩张。每个 flag 必须证明不会产生未进入 key 的 object 语义。
- remote write credential 可以污染共享缓存。Jenkins 没有内建自动保护，
  必须由 pipeline 实施只读/写入边界。
- 小 TU 的 wrapper、probe、hash 和 restore 成本可能高于直接编译。应考虑
  `KACHE_MIN_STORE_COMPILE_MS`，但门槛也要通过真实 workload 测量。

## 9. 最终建议

1. 先在 Jenkins 选择一条 Rust-heavy job，使用固定 `v0.19.0` binary 和
   filesystem remote 或 S3，运行一周 canary。
2. Blade job 单独试验。先保留现有 CAS/Goma 作为 baseline；若改用 Kache，
   通过 verbose command 与 report 验证 compiler interception。
3. 对带 `-H` 的 Blade，使用项目级 allowlist 之前先跑 cross-workspace
   include-stack、depfile、全量测试和 source-change invalidation。
4. 不把 Kache local store 当成整个 Jenkins workspace cache；Cargo registry、
   downloaded dependencies、codegen 和 linker cache 需要分层治理。
5. 以 wall time、compile-weighted hit rate、passthrough coverage、remote
   reliability 和 correctness 五项共同做 go/no-go。
