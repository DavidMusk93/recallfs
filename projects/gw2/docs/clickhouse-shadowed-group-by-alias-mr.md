# ClickHouse Shadowed Group-By Alias

## Regression

- Scope: verify the `ResolveClickhouseShadowedGroupByAliasRule` behavior and its parser/session integration only.
- Files:
  - `starry/starry-core/src/main/scala/org/apache/spark/sql/execution/columnar/extension/rule/ResolveClickhouseShadowedGroupByAliasRule.scala`
  - `gateway-catalyst/src/main/scala/org/apache/spark/convert/parser/TideSparkSession.scala`
  - `gateway-thrift-service/src/test/scala/com/bytedance/tide/catalyst/GroupByAliasFunSuite.scala`

### 1. Install dependent modules

```bash
./mvnw -pl starry/starry-core,gateway-catalyst,gateway-thrift-service -am -DskipTests install
```

- Purpose: ensure the renamed ClickHouse shadowing rule and parser/session changes are available to downstream test modules.
- Result: `BUILD SUCCESS`

### 2. Run focused regression suite

```bash
JAVA_TOOL_OPTIONS='--add-exports=java.base/sun.nio.ch=ALL-UNNAMED' \
./mvnw -pl gateway-thrift-service \
  -DfailIfNoTests=false \
  -Dtest=com.bytedance.tide.catalyst.GroupByAliasFunSuite \
  test
```

- Purpose: validate ClickHouse-compatible shadowing behavior for outer `GROUP BY` keys.
- Covered cases:
  - standard `GROUP BY alias` still resolves to the same-level select-list expression
  - unqualified outer `GROUP BY __api_time__` rewrites to the outer select-list alias expression under ClickHouse-compatible shadowing behavior
  - qualified `GROUP BY t2.__api_time__` keeps standard child-reference semantics and is not rewritten
- Result: `Tests run: 3, Failures: 0, Errors: 0, Skipped: 0`
