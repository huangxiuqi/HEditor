# 会话记录导出

这是 WorkBuddy 里那次「编辑器从零自研」的完整对话轨迹，从原始会话文件（append-only JSONL）转出来的可读副本。

| 项 | 值 |
|---|---|
| 会话 ID | `907be7c9-107c-45ff-a1b7-91fe0946d4f7` |
| 标题 | 调整控件光标规则及修复嵌套控件边界问题（标题随会话演进，这是最后一次生成的） |
| 会话所在工作目录 | `d:\Work\CloudMedical\micro-services\cloud-medical-component`（实际动手的工程是 `D:\Develop\editor`） |
| 时间范围 | 2026-10-09 09:59:39 ~ 2026-10-10 19:34:47（本地时间） |
| 规模 | 96 轮用户提问 · 983 条助手正文 · 4349 次工具调用 · 2136 段思考 |
| 原始文件 | `C:\Users\huang\.workbuddy\projects\d-Work-CloudMedical-micro-services-cloud-medical-component\907be7c9-107c-45ff-a1b7-91fe0946d4f7.jsonl` |

## 文件

| 文件 | 内容 |
|---|---|
| `会话记录.md` | **对话正文版**。用户提问与助手回复全文照录，每轮末尾是一个折叠块，列出该轮调过的工具（名称 + 一行摘要，不含结果）。0.9 MB。 |
| `会话记录.html` | **完整轨迹版**。在正文之外再带思考过程、工具入参、工具结果，全部可折叠；左侧固定目录，Esc 一键收起。深色主题，双击即看，无外部依赖。8 MB。 |
| `907be7c9-....raw.jsonl` | **原始数据逐字节副本**。77 MB，一行一条记录，含全部字段（包括下表的内部记录）。想拿去做二次处理就用这份。 |
| `images/` | 会话里贴过的 7 张截图，从 `~/.workbuddy/blobs/` 复制而来，文件名取 blob 哈希前 16 位。 |
| `_export_session.py` | 导出脚本本体，可重跑（见下）。 |

## 原始 JSONL 的记录类型

| type | 条数 | 说明 |
|---|---|---|
| `message` (role=user) | 150 | 其中 96 条是真实提问（内含 `<user_query>`），其余 54 条是系统注入的上下文 / 模式切换。 |
| `message` (role=assistant) | 983 | 助手正文，每轮可能多条（工具调用之间会夹短句）。 |
| `reasoning` | 2136 | 思考过程，`rawContent[].text`。 |
| `function_call` | 4345 | 工具调用，`name` + `arguments`（JSON 字符串）。 |
| `function_call_result` | 4335 | 工具结果，按 `callId` 与调用配对。 |
| `file-history-snapshot` | 1218 | 内部文件快照簿记，**可读视图里已剔除**（9 MB，纯内部状态）。 |
| `session-meta` / `ai-title` | 16 / 5 | 会话元信息与自动标题。 |

## 截断规则（只影响 HTML 版）

原始数据太大（77 MB，单条工具结果最长接近 1 MB），HTML 里做了截断，截断处都有显式标记：

- 思考段：800 字符
- 工具入参：400 字符
- 工具结果：保留头 700 + 尾 200 字符（错误信息通常在尾部，所以留尾）
- 正文与用户提问：**不截断**

要无截断的内容，去 `raw.jsonl` 里按 `timestamp` / `callId` 检索。

## 重跑

```bash
"C:/Users/huang/.workbuddy/binaries/python/versions/3.13.12/python.exe" \
  docs/session-export/_export_session.py \
  "C:/Users/huang/.workbuddy/projects/d-Work-CloudMedical-micro-services-cloud-medical-component/907be7c9-107c-45ff-a1b7-91fe0946d4f7.jsonl" \
  docs/session-export
```

脚本零依赖，只用标准库。会话还在继续，重跑一遍就是最新快照（会覆盖同名文件）。

要导别的时间段的会话，换第一个参数即可；其它会话文件在
`C:\Users\huang\.workbuddy\projects\<按路径转义的工程名>\<会话 ID>.jsonl`。
