# douyin

用本机 Chrome 的抖音登录 Cookie，把一条公开视频下到 `~/Downloads`。

默认只下高度不超过 1080 的那一档。播放接口的 `ratio=default` 会给出原片（这条样本是 3840x2160，约 1.2GB）。`ratio=1080p` 才是 1920x1080，约 57MB。程序先用 Range 读帧尺寸，高度超限就不下完整文件。

```text
page URL or aweme id
        |
        v
Chrome Cookies  -- sessionid / ttwid
        |
        v
aweme detail --> video id
        |
        v
play?ratio=1080p   (skip ratio=default unless --max-height > 1080)
        |
        v
Range 2MB --> tkhd width/height
        |
        +-- height > cap --> try the next lower ratio
        |
        `-- height ok --> save mp4
```

## 运行

```bash
mkdir -p .tmp
rustc --edition 2021 -D warnings -O tools/media/douyin/src/main.rs -o .tmp/douyin-dl
.tmp/douyin-dl --self-test
.tmp/douyin-dl 'https://www.douyin.com/video/<aweme_id>'
```

Cookie 只留在临时文件里，权限 `0600`，进程结束就删。不要把 Cookie 或视频提交进仓库。

## 旗标

| 旗标 | 默认 | 作用 |
| --- | --- | --- |
| `-o` | `~/Downloads/<标题>-<id>-1080p.mp4` | 输出路径 |
| `--max-height` | `1080` | 允许的最大画面高度。大于 1080 才会去请求 `default` 原片 |
| `--check-cookies` | | 只检查 Chrome 里有没有抖音登录态，不打印 Cookie 值 |
| `--self-test` | | 跑内置向量，不访问网络 |

Chrome 里没有 `douyin.com` 的 `sessionid` 时直接退出。先在 Chrome 登录抖音再跑。

## 失败

| 现象 | 含义 |
| --- | --- |
| 没有 sessionid | Chrome 未登录抖音，或 Cookie 库还锁着旧副本 |
| 画面高度超限 | 这一档是原片。不要改去下 `ratio=default`，除非明确提高 `--max-height` |
| 三档都没有合格帧 | 接口没给 1080/720/540，或返回的不是 mp4 |
