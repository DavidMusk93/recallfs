## Velox Build And Test Notes (Experience)

本文只记录“开发经验/排坑/构建测试方法”，不包含 root cause 与修复原理（根因文档见 `docs/rc/`）。

---

## 1. 跑通核心单测（TypedExprSerDeTest）

推荐用仓库提供的脚本跑：

- `docs/test/run_typed_expr_serde_test.sh`

它会完成：

1. 构建 `velox_core_test`
2. 用 gtest filter 运行 `TypedExprSerDeTest.*`

如果你只想跑某个用例：

```bash
./cmake-build-velox/velox/core/tests/velox_core_test \
  --gtest_filter='TypedExprSerDeTest.constantValueVectorSerializationIsCanonical*'
```

---

## 2. 构建环境常见坑（按出现顺序）

### 2.1 FlexLexer.h 找不到

症状：

- 编译报错：`fatal error: FlexLexer.h: No such file or directory`

解决思路：

- 这是系统依赖缺失类问题，优先安装缺包（例如 `flex` / `libfl-dev`），不要用乱加 include path 掩盖。

### 2.2 不要随意加 `-isystem /usr/include`

症状：

- 为了解决某个头文件找不到，临时加了 `-isystem /usr/include` 后，标准头（如 `stdlib.h`）反而解析异常。

原因：

- 可能改变默认 include 搜索次序/屏蔽规则，制造更大问题。

建议：

- 回到“装依赖/修 FindXXX.cmake/修 CMake 变量”的正路。

### 2.3 Protobuf / gRPC 版本选择漂移

症状：

- CMake 阶段报找不到 Protobuf 或版本不匹配。

建议：

- 在存在统一 `cpp3rdlib` 的仓库里，尽量与其 vendored 版本对齐，避免 CMake 在 SYSTEM/BUNDLED 之间摇摆。

### 2.4 zstd undefined reference（同名 so 选错版本）

症状：

- 链接 `velox_core_test` 时出现 `undefined reference to ZSTD_CCtxParams_setParameter` 等。

原因：

- 依赖 target 的 `INTERFACE_LINK_LIBRARIES` 里可能混入系统 `libzstd.so` 绝对路径，导致链接/运行时选到了旧库。

排查：

- `ldd <binary> | grep -E 'zstd|not found'`

### 2.5 运行时报 ICU 找不到（RUNPATH vs RPATH）

症状：

- `error while loading shared libraries: libicudata.so.65: cannot open shared object file`

原因要点：

- `RUNPATH`（new dtags）对间接依赖解析不生效，导致 `libfolly.so -> libicudata.so` 找不到。

排查：

- `readelf -d <binary> | egrep '(RPATH|RUNPATH)'`
- `ldd <binary> | grep -E 'icu|not found'`

---

## 3. 调试方法清单（最有效的几招）

1. 确认到底在用哪份库：`ldd <binary> | grep -E 'zstd|icu|folly|not found'`
2. 检查 RPATH/RUNPATH：`readelf -d <binary> | egrep '(RPATH|RUNPATH)'`
3. 确认最终 link line：查看 `build.ninja` 或编译输出的链接命令

