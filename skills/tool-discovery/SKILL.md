---
name: tool-discovery
description: >
  工具发现。动手探索前先查 recallfs 的 tools/ 和 skills/；可复用的能力收成
  tools/$domain/ 下的 Rust applet，下次直接运行，不再重新摸索。
  触发：工具发现、有没有现成工具、别再重新探索、收成 applet、/tool-discovery。
---

# 工具发现

可复用的做法只探索一次。下次从已有 applet 开始。

## 先查

按这个顺序，找到能完成任务的命令就停：

1. `tools/` 里的 Rust applet（README 里的运行命令）
2. `skills/` 里已经写明的流程
3. nmem 里同一能力的结论
4. 本机 PATH 上已经装好的命令

没有命中，才允许重新探索。探索过程放在 `.tmp/`，不要写进仓库。

目录规则以 `AGENTS.md` 的「工具与记忆」为准：applet 放 `tools/$domain/`，二进制和临时输出放 `.tmp/`。

## 什么时候收成 applet

下次还会碰到同一类输入输出，就收成 applet。一次性探针留在 `.tmp/`。

```text
Need a capability
        |
        v
Search tools/, skills/, nmem, PATH
        |
        +-- hit --> Run the existing command
        |
        `-- miss --> Solve it once under .tmp/
                        |
                        v
                 Same capability next session?
                        |
                        +-- no --> Stop. Do not promote.
                        |
                        `-- yes --> Rust applet in tools/$domain/
                                    README: command, inputs, failure,
                                    and the choice that was expensive
                                    Build the binary into .tmp/
                                    Commit and push
```

## applet 最低内容

- 一个 `rustc` 或 Cargo 入口，加上 `README.md`
- README 写明：解决什么、一条运行命令、失败时长什么样、哪一个选择不能再试
- 密钥、cookie、下载的媒体不要入库
- 能用参数表达的策略写成旗标，不要把某一次的样本 ID 写死在程序里

## 例子

下载一类有多档清晰度的媒体。探索时发现有一个地址是超大原片，另一档才是目标尺寸。applet 把档位和高度上限做成参数，默认停在目标档，并且先读帧尺寸再决定要不要下完整文件。下次运行该二进制。
