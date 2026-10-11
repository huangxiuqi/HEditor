# 第三方依赖与授权清点（THIRD PARTY NOTICES）

> 结论摘要：**所有代码依赖均为宽松许可（无 copyleft），可安全用于闭源商业产品。**
> 但**字体是雷区**——现有病历组件里随包分发的两个蒙文字体**都没有商用分发授权**，必须替换。
>
> 本文件基于**实际下载到 `third_party/` 的源码**逐份核对，不是凭印象。
>
> **2026-10-09 更新**：引入 Skia 作为 wasm 侧的光栅后端，**FreeType 由"备选"变成"必需"**。
> 因此 FTL 的致谢义务（见 §1.2）**从"将来要做"变成"现在就有"**。
> 「实际进入可执行文件」的判定见 §1.3 —— 是查符号，不是看依赖清单。

---

## 一、代码依赖

| 依赖 | 版本 | 许可证 | Copyleft | 义务 |
|---|---|---|---|---|
| **HarfBuzz** | 14.6.0 | **Old MIT**（源树 `COPYING`，入库副本 `licenses/MIT-HarfBuzz.txt`） | 否 | 保留版权与许可声明 |
| **Skia** | main @ 2026-10-09 | **BSD-3-Clause**（`licenses/BSD-3-Clause-Skia.txt`） | 否 | 保留版权声明 |
| **FreeType** | 2.14.3 | **FTL** 或 GPLv2（**二选一，必须显式选 FTL**） | 否（选 FTL 时） | **必须在产品文档中明确致谢 FreeType 项目**（FTL 广告条款） |
| **libpng** | 1.6.x（Skia DEPS 钉的 commit） | **libpng License**（`licenses/libpng-LICENSE.txt`） | 否 | 保留版权与许可声明 |
| **zlib** | chromium fork（Skia DEPS 钉的 commit） | **zlib License**（`licenses/Zlib-LICENSE.txt`） | 否 | **不得冒充作者**；保留声明 |
| **Emscripten** | 6.0.12 | **MIT / Expat**（`LICENSE`） | 否 | 保留许可声明（生成的 JS 胶水已带） |
| ICU（备选，尚未引入） | main | Unicode License V3 | 否 | 保留版权声明 |
| SheenBidi（备选，尚未引入） | master | Apache-2.0 | 否 | 保留 NOTICE |

> **没进包、因此没有义务的**：libjpeg-turbo、libwebp。
> Skia 的 `//:skia` 是 `complete_static_lib`，这俩会被编进 `libskia.wasm.a`，
> 但本工程关掉了全部编解码、也没有任何代码引用它们 → 链接期被 GC 掉。
> **判定依据是查符号（见 §1.3），不是"它在依赖清单里所以我们要致谢"。**

### 1.1 HarfBuzz 逐份核对结果

- 顶层 `COPYING`：**Old MIT**。
- 全树只有 1 个子目录有独立许可：`src/ms-use/COPYING` → **MIT License (Microsoft Corporation)**。该目录只含 3 个 `.txt` 数据文件（USE 整形用的分类表），同样是**宽松许可**。
- 其余 `perf/texts/COPYING`、`test/COPYING`、`test/shape/data/**/COPYING` 均属测试数据，**不随产品分发**。
- 抽样验证 `src/hb-ot-font.cc`、`src/hb-draw.cc`、`src/hb-ot-shaper-arabic.cc` 许可头均为 HarfBuzz 自身（Old MIT）。

→ **HarfBuzz 整体可安全静态链接进闭源产品。**

> ★ 注意：Skia 自己**不**带 HarfBuzz。Skia 的 HarfBuzz 只服务于 `modules/skshaper`
> （本工程不编），核心光栅化与 `SkFont` 完全不依赖它。
> 所以 `libskia.wasm.a` 里查不到 harfbuzz 符号是**预期**的，不是漏编。
> 引擎的整形用的是本仓库自带的 `third_party/harfbuzz-14.6.0`（`heditor_harfbuzz`）。

### 1.2 FreeType 的关键提示（现在真的生效了）

`LICENSE.TXT` 原文（本机已下载）明确：

> The FreeType 2 font engine ... we distribute it under **two mutually exclusive open-source licenses**.
> **This means that *you* must choose *one* of the two licenses** described below.

两个选项：

1. **FTL**（`licenses/FTL-FreeType.txt`）—— BSD 风格 + 广告条款，**宽松，可用于闭源商业产品**；
2. **GPLv2** —— 强 copyleft，**会传染**。

> ⚠️ **工程纪律**：FreeType 是"隐含双许可"，如果某天有人"清理许可"时误选了 GPLv2，整个 HIS 就会被 GPL 传染。
> 建议在仓库根放一份 `LICENSE-SELECTION.md` 明确声明"本项目对 FreeType 选择 FTL"。

**FTL 的广告条款实际义务**（`licenses/FTL-FreeType.txt` 原文）：

> if you use it, or only parts of it, in a program, **you must acknowledge somewhere in your documentation that you have used the FreeType code**.

→ 落地动作：在产品《关于/开源许可》页面加一句
`Portions of this software are copyright © The FreeType Project (https://freetype.org). All rights reserved.`

> ★ **状态变更**：这一段原来写的是"本项目 POC 阶段不使用 FreeType"。
> 自从 `HEDITOR_USE_SKIA=ON` 之后这句话**不再成立** —— Skia 的字体后端
> （`SkFontMgr_New_Custom_Empty` / `SkTypeface_FreeType`）就是 FreeType，
> 且它**进了包**（符号检查见 §1.3）。所以上面那句致谢是**现在就要加**的。
> 旧的自写光栅器（`HEDITOR_USE_SKIA=OFF`）确实不需要 FreeType，但那是**非默认配置**，
> 不能拿它来免除义务。

### 1.3 到底谁进了可执行文件 —— 用符号判定，不用依赖清单

依赖清单只说明"编了什么"，而我们关心的是"分发了什么"。两者会差很远
（`libskia.wasm.a` 有 7.9 MB、791 个 .o，全部来自 Skia 的 `complete_static_lib`）。
本工程的做法是在**最终产物**里找标志性字符串：

```powershell
# 对 build/wasm/web/heditor.wasm 逐条查
#   SkSL / FreeType / libpng / zlib / libjpeg / libwebp ...
```

2026-10-09 实测（`HEDITOR_USE_SKIA=ON`，wasm 3,282,875 B）：

| 依赖 | 在最终 wasm 里 | 结论 |
|---|---|---|
| Skia | ✅ | 要致谢（BSD-3） |
| FreeType | ✅ | **要致谢（FTL 广告条款）** |
| libpng | ✅ | 要保留许可 |
| zlib | ✅ | 要保留许可 |
| libjpeg-turbo | ❌ | 被 GC 掉，无义务 |
| libwebp | ❌ | 被 GC 掉，无义务 |

> 这套判定的价值：以后有人重新打开 `skia_use_libjpeg_turbo_decode` 或加图片解码，
> 表里那一行就会从 ❌ 变 ✅，义务跟着出现。**改动之后请重跑这个检查**，
> 而不是靠"我印象里没开"。

---

## 二、字体（真正的问题所在）

> 以下结论来自**直接解析 TTF/OTF 的 `name` 表与 `OS/2.fsType` 字段**，是机器可读的客观证据。

### 2.1 现有病历组件随包分发的字体 —— ❌ 都不能商用分发

文件位置：`cloud-medical-component/src/styles/fonts/`

#### `monbaiti.ttf`（Mongolian Baiti）

| 项 | 值 |
|---|---|
| Copyright | `© 2018 Microsoft Corporation. All Rights Reserved. Portions © 2015 Beijing Founder Electronics Co. Ltd.` |
| Trademark | `Mongolian Baiti is a trademark of the Microsoft group of companies.` |
| Designer | Founder, Beijing China |
| `OS/2.fsType` | `0x0008` = Editable embedding |
| LicenseDesc | `Microsoft supplied font. You may use this font ... as permitted by the license terms or terms of use, **of the Microsoft product, service, or content in which this font was included**. You may only (i) embed this font in content as permitted by the embedding restrictions included in this font; and (ii) temporarily download this font to a printer ... **Any other use is prohibited.**` |

**判读**：`fsType=0x0008` 只管"嵌入到**文档**里"，不等于"可以作为 Web 字体再分发"。
EULA 把用途限定在"随该字体一起分发的那个 Microsoft 产品"之内。
微软官方字体页把 Mongolian Baiti 的许可指向 *Font redistribution FAQ* / *License Microsoft fonts for enterprises, web developers, for hardware & software redistribution* ——
**即再分发需要单独取得授权**。

→ **结论：不能自托管分发给客户。** 需购买授权，或换成 OFL 字体。

#### `Menk_Qagan_Tig.ttf`（Menksoft 蒙科立）

| 项 | 值 |
|---|---|
| Copyright | `Copyright(c) Menksoft Co.,Ltd.2012` |
| Manufacturer | Menksoft Corporation（呼和浩特） |
| `OS/2.fsType` | `0x0008` = Editable embedding |
| LicenseDesc | `You may use this font to display and print content as permitted by the license terms **for the product in which this font is included** ...` |

蒙科立官方《字库许可协议》（可公开查到的 1.00 版，2012-12-01）原文关键句：

> 本许可协议将本字库软件的使用权授予客户，**客户仅可应用于非商业用途**，并保证不损害蒙科立公司的商业利益，不与蒙科立公司形成竞争。

字体市场（fontke 等）对蒙科立系列字体的标注一致：**授权方式 = 商用须授权**。

→ **结论：非商业免费，商用需购买授权。** HIS 是商业产品，**不能直接随包分发**。

> **两者共同风险**：`Mongolian Baiti` 是 Windows/Office 自带字体，蒙科立字体是商业字体。
> 把 `.ttf` 直接提交进 `src/styles/fonts/` 并随产品分发，属于**超授权再分发**。
> 这是法务级问题，优先级高于任何技术选型（与之前 DCWriterLite 的 GPL-3.0 同类）。

### 2.2 建议的替代字体 —— ✅ 可自由分发

| 字体 | 许可证 | 用途 | 本机路径 |
|---|---|---|---|
| **Noto Sans Mongolian** | SIL OFL 1.1 | 蒙文竖排（`vertical-lr`，侧倒 90°） | `assets/fonts/NotoSansMongolian-Regular.ttf` |
| **Noto Sans SC** | SIL OFL 1.1 | 中文（主字体） | `assets/fonts/NotoSansSC-VF.ttf` |
| **Noto Sans**（拉丁） | SIL OFL 1.1 | **字体回退**：兜住中文子集没覆盖的码点 | `assets/fonts/NotoSans-Latin-VF.ttf` |

### 2.2.1 为什么必须有第三个（拉丁）字体 —— 实测证据

| 码点 | NotoSansSC-VF | NotoSans（拉丁） |
|---|---|---|
| U+00B9 ¹ / U+00B2 ² / U+00B3 ³ | 有 | 有 |
| **U+2070 ⁰** | **无**（gid 0） | 有 |
| **U+2079 ⁹** | **无**（gid 0） | 有（gid 1555） |

`NotoSansSC-VF` 的 cmap 只覆盖 Latin-1 的上标三兄弟，**不含 U+2070–U+209F 上/下标区**。
而"WBC 11.2×10⁹/L"是检验报告的标准写法——没有回退链，用户看到的就是豆腐块 ☒。

→ 引擎侧已实现**字体回退链**（`src/core/text/fallback.cpp`），
`BlockSpec::fallback_font_indices` 指定回退顺序；缺字数由 `LayoutStats::missing_glyph_count` 量化。

`NotoSans-Latin-VF.ttf` 的 `OS/2.fsType = 0x0000`（Installable Embedding，**无嵌入限制**），
cmap 条目 3094，覆盖 U+2070–U+207A、希腊字母、常用数学符号。

### 2.2.2 现有两个字体

许可原文已存档：`licenses/OFL-1.1-NotoSansMongolian.txt`、`licenses/OFL-1.1-NotoSansSC.txt`。

**OFL 1.1 的义务（宽松，但要知道）**：

1. 字体**不得单独售卖**（可以随产品分发）；
2. 若**修改**字体文件（含子集化、改字形），**不得继续使用 Reserved Font Name**（须改名）；
   → 对 CJK 子集化很关键：子集后必须去掉 `Noto` 前缀或按 OFL 要求处理命名；
3. 字体文件内嵌的版权与许可声明不得删除。

> **子集化**：`NotoSansSC-VF.ttf` 未子集化有 **17.7 MB**。生产环境应按实际用字子集（EMR 常用字约 3000–7000 字），
> 可压到 ~1–2 MB。注意这会触发 OFL 的改名要求。

### 2.3 需要法务确认的两点

1. 现有 `cloud-medical-component/src/styles/fonts/` 下两个字体的历史分发是否已产生风险，以及是否需要取得蒙科立/微软授权。
2. 若业务上确实必须使用 `Mongolian Baiti` 的字形风格（它与国标 GB18030-2000 的"方正蒙白体"一致），
   需向微软或方正取得再分发授权——**这是采购决策，不是技术决策。**

---

## 三、与 DCWriterLite 的对比

| 方案 | 许可证 | 私有化交付（分发）是否可行 |
|---|---|---|
| **本项目（HarfBuzz + Skia + 自研引擎）** | Old MIT + BSD-3 + FTL + zlib/libpng | ✅ 可行（需在产品《关于》页加 FreeType 致谢） |
| DCWriterLite（开源版） | **GPL-3.0** | ❌ 触发 copyleft，须开源整个作品 |
| DCWriter 商业版 | 商业授权 | ✅ 需付费 |

> DCWriterLite 的 GPL 结论需法务核对"你们实际要用的仓库 + commit"的许可证原文——
> 网上存在标 Apache-2.0 的 fork，官方仓库也有过一次 `Update LICENSE` 提交。
