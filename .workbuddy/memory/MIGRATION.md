# 迁移台账（模型层 0.x ~ 1.10 与阶段 1 六刀）

> 2026-10-12 从 `MEMORY.md` 迁出：这里是索引与判据式结论，**权威仍是** `docs/文档模型设计.md`
> 第十五节与 `memory/YYYY-MM-DD.md`（逐段得失在 daily log）。`MEMORY.md` 只留一行索引。
> 模型文档被写回旧版时的恢复判据在 `MEMORY.md` 开头。

## 模型层

- **0.x 地基（全绿）**：`node_id`/`node`/`flow`/`para`/`invariants`/`compile`/`units`/`length`。流位置
  **码点级**、根不含边界、容器槽位 = `children.size()+1`、同一流位置**取最内层**；分段"标记属于它结束的
  那一段"；改结构三路 `insertChild`/`eraseChild`/**`attachChild`**。`layout.h::ParaIndex` 是排版结果，
  doc 层那个才叫 **`ParagraphIndex`**。
- **1.1~1.3**：`char_format`/`normalize`（只并同标记邻居、清空 run）、`range`/`fragment`/`steps`/`subtree`
  （内容级与节点级步骤，**不用 `MoveNode`**）、`transaction`（撤销按逆序应用逆操作、**不用快照**）。
- **1.4~1.6**：`edit_mode`/`edit_capability`（可编辑集合取交集、**落点不受限（只有编辑受约束）**、
  Tab 是唯一按"可编辑"选目标的移动键、structureLock）、
  `control_spec`/`control_index`/`control_api`/`validation`（`ChangeGate` 四态、悲观持有）、
  `style`（三层解析 + `StyleCache`，元素只存差异、样式引用是内容）。
- **1.7 表格（结构一半）**：`table_spec`/`table`（跨度唯一真相、`buildGrid` 派生、`at(r,c)` 返回**原点格**、
  四个行列操作返回一批步骤）+ `SetTableCellSpan` + `pushMappedStep`。★ "每行的跨度之和相等"指"这一行
  **被覆盖**的格位数"。**待落地**：跨页片段网格、动态表格、表格样式。
- **1.8 域与节**：`field_spec`（`FieldSpec{kind,name,cache}`/`FieldContext`/`fieldValue`：只写 cache、
  幂等、不产生步骤）+ `section_spec`（`Paper`/`SectionMargins`/`Gutter`/`SectionBreak`/`VerticalAlign`/
  `WritingMode`/`PageNumbering`/`HeaderFooter[3]`×2/`extra` 属性袋/`sameAs`/`paperSize`）。`Node` 加
  `field`/`section`，`NodeKind` 加 **`HeaderFooter`**（容器、不在正文流、走 `compileFragment`）；
  `NodeTree::sectionCount/sectionAt`；分节强制分页落成 `page_break_before`；`NodeSpec` 带 `field`。
  **待落地**：按节生效的页度量（引擎侧）、页眉页脚渲染、页码格式与重编求值。
- **1.9 修订**：`RevisionLog`（`reset` 记基线、`record` **只收内容级五步**、`accept`/`reject` 只改状态、
  `expand(NodeTree&)` 从基线按序应用未被拒的步骤并规范化一次）+ `NodeTree::assignFrom`/`clone` + 把一条
  步骤坐标穿过另一条映射的 `mapStepPositions`（只覆盖内容级五步）。★ 拒绝一条早的修订时，后面修订的
  坐标要穿过它的**逆步骤**倒着搬（最后被跳过的先搬），位置正好落在被恢复内容插入点上时留左边
  （`Assoc::Before`）。展开失败**不交半成品**（还原成基线）并写明是哪一条。
  **待落地**：批注、与 Word 的 `w:ins`/`w:del` 互导。
- **1.10 序列化**：`serialize.h/.cpp`（**nlohmann/json 3.12.0**，只在 .cpp 里 include，PRIVATE 包含目录）。
  格式 `{v, sym{k,s}, roots[], nodes[], next}`；nodes 按**深度优先序**拍平、每项只记孩子个数；sym 收节点
  种类名与名字字符串（样式/控件类型与 key/校验规则名/语言标签）；默认值不写；枚举**按名字写**（配
  `xxxFromName`，拿 `xxxName` 当唯一真相）；浮点写十进制字符串（9 位有效数字）；落盘无空白；**写出前自查
  UTF-8**（wasm 关了异常，库的失败路径是 abort）。`isAttached` 改成**沿父链上溯到根**。★ 反序列化
  **先在临时树上装好、成功才 `assignFrom` 给目标树** —— 读坏了目标树一个字节都不改。**待落地**：样式表/
  资源表落盘、二进制资源外置的包容器、增量落盘；不带摘下的子树（水位仍带过去）。

## 阶段 1 引擎元素流（2026-10-12，六刀已落地）

① **`FormatRun` 两把尺子求交**：`BlockSpec::runs` / `effectiveRuns()` / `wholeRun()` / `runsTileText()` /
`runCovering()`；排版先按格式 run 切、再在段内按字体回退切，整形段 = 边界**并集**；指纹混 run 属性
（**颜色刻意不进指纹**）；`setText` 清 `runs`。

② **三层解析进 run**：文本段**缓存到段落结束才解析**（段落样式挂在结束标记上）、段落样式链的字符部分
**一段解析一次**（`paragraphRunBase` + `resolveRunFormatFrom`）、run 以**段落默认字符格式**为底；相邻同
格式段并成一个 run；粗体/斜体/下划线引擎还不认 ⇒ 设了就记诊断（不静默丢）。

③ **行度量按行**：横排取本行所有 run 的最大 ascent/descent/字号，竖排取**整段**最大；推进用**行顶**
而非基线（基线 = 行顶 + 本行 ascent）；"本行放不下"必须在本行度量已知后再判。

④ **行内对象进块流**：`InlineObject{cp,kind,owner,read_only}` + `BlockSpec::objects` + `objectsOrdered()`；
排版入口收窄成 `layoutDocument` / `layoutDocumentRange`（**不再收 `ControlSet` 与括号名单**）；
`withBracketSlots` 改名 `withInlineObjects`（一次线性归并）；`syncInlineObjects()` 在每次 `relayout()` 前
写表，**边界先后（bucket 0 闭按 depth 降序 / 1 空字段成对 / 2 开按 depth 升序）从 layout.cpp 原样搬到
引擎**；`trySuffixReuse` 的后缀判据改成"看后缀段对象表是否为空"。

⑤ **产出端搬进编译层**：`ContentControl` 从"记诊断 + 下钻"改成**边界进内容**（进写 `ControlStart`、出写
`ControlEnd`，锚点 = `cpCount()`）。★ 三个结论：**跨段控件不需特殊规则**；**排序规则消失**（次序是遍历
的自然产物）；**owner 是节点 id 不是序号**（序号会随插入平移）。`run()` 落段条件加 `|| !obj_.empty()`。

⑥ **引擎持树**：`setDocument(tree, sheet)`（接 const 引用走 `assignFrom`，树不可拷贝）；`blocks_` 变成
`compileDocument` 的产物；控件集合从产物边界导出（`exportControlsFromBlocks`，业务标识与种类从节点取、
标签不填 —— 标签是内容）。分工三变：**产边界在编译层、决定显示哪些留引擎**（持树时 `syncInlineObjects`
退化成"按名单筛对象表"，只改 objects）；**owner 要翻译一次**（`controlIndexOfOwner`：编译给节点 id、
对外一律下标）；**可见性随模式重编译**（Document=Design、Form/ReadOnly=Display ⇒ `setEditMode` 持树时
必须重编译 + 整篇标脏）。★ 本刀**不碰编辑**：`leaveTreeModeForEdit()` 在落笔前摘掉"树是真相"标记并把
`runs` 拉回旧口径，有损与否由 `droppedCharFormats()` 报出。

**已收口**（2026-10-18）：树上编辑与旧装载路径退役（第七刀 B/C + 演示页切树上）、`cp_text` 降为
派生视图（引擎侧没有写入口，只由 `compile.cpp` 从树产出）、**竖排逐列变宽**（列宽 = 该列字号 ×
行距，列位由前缀和推进；判据 `shaping_test [7]`，注入旧行为当场两红）。★ `line_factor` 与段后距
**按段落不按 run** —— 模型层在 `ParaFormat`、OOXML 的 `w:spacing` 也在 `w:pPr`，不是缺口。

## 第七刀 树上编辑（2026-10-12，分三段）

让整条编辑路径一次落地有两个问题：中间态没有东西可判，且三件事的证伪方式完全不同（坐标换算 / 产出等价 /
策略语义）⇒ 拆三段，A→B→C。

### A 段 段落身份与流位置桥接（已落地）

引擎对外说 `(段, 段内码点偏移)`，模型说 `FlowPos`，中间隔着**控件边界**（开闭括号各占一位）与**节边界**
（起止各占一位）。换算只认一个权威：编译的来源身份 + `FlowIndex`。

- `CompileResult` 多 `sources`，每块一条 `BlockSource{first, mark}`（`first` = 本块吃到的第一个在流里占位的
  元素；`mark` = 结束标记，`kInvalidNodeId` = 末尾那段还没被标记结束）。记**节点 id** 而非流位置，因为编译层
  不认识 `FlowIndex`。
- 引擎据此建 `block_flow_`（每块内容起点，取自 `intervalOf(first)`）与 `block_flow_span_`（文本码点数 + 边界数），
  与流索引**一起**在 `rebuildFromTree()` 重编译（分两次建会"块按新树、位置按旧树"，只表现为光标落偏一个字）。
- 对外两个互逆入口：`flowPosAt(段, 码点偏移, 归属提示)` 与 `flowToTextPos(流位置)`。★ 同处边界上"控件之前 /
  进入控件"是两个流位置 —— 无提示取最外侧、有提示贴内侧；**不持树时桥不存在**（返回 -1 / false，不猜）。

判据（edit_test [26] 五组）：块起点与模型侧独立算出的流位置逐项一致；两段起点之差 = 首段内容位数 + 标记一位；
压在开括号上的归属分侧、闭括号即内容末尾、段末正落在标记那一位；**正向换出去再反向换回来逐项相同**；
落在段间的位置归相邻段端点；不持树时桥消失。

### B 段 编辑改在树上落笔（已落地）

口径：树上编辑与旧路径**逐字段逐字节相同** + 增量页数不退化 + 撤销往返回帧。引擎持 `UndoStack`
（`setDocument` 按树建，`setBlocks`/`setControls` 丢弃）；五个内容级编辑入口（插入/退格/前删/拆分/合并）与
缩进改成**先建步骤、再经 `commitOnTree` 提交**：commit → 规范化 → `rebuildFromTree()` 重编译 → 只标脏受影响段
（段数变了才置 `para_count_changed_`）。撤销/重做按事务逆序应用逆操作，commit 后重编译并**整篇标脏**（脏区不可
定位），光标按事务记的编辑前选区经桥换回 `TextPos`，置 `frame_needs_full_refresh_`。

三条落笔语义：**字符格式继承**（新字沿用落点左侧文本节点的 `marks`/`styleRef`，`marksAtFlow`）；**段落格式归属
显式搬**（段落属性挂结束标记上，回车新标记结束前半段 ⇒ 强制分页等落到原标记；合并删前段标记、留下的是后段标记
⇒ "保留前段格式"搬到后段标记）——新增步骤 `SetParaMarkFormat` 承载，位置映射恒等；**末尾段无结束标记**（合并进
末段要补插一枚带上被删标记格式的标记，坐标按"删完之后"给）。★ 本段第一个真地雷：**结构口径 ≠ 编辑口径**
（`makeDeleteRange` 会外扩到单位边界）⇒ 另立 `makeDeleteTextSteps`（按文本节点切分、从后往前发步）。

删除：`leaveTreeModeForEdit` 退让、`dropped_char_formats_`。判据（edit_test [27] 五组）：装载逐字段一致；
控件内退格（区间 `1,1-1,2`）；分段（`ab|cd|efgh|ij`，pbb 留前半段）；合并（含并进末段 `abcdefghij` 仍 20 磅、
区间 `0,5-0,7`）；24 段 6 页文档敲字脏页数 `1 / 1` 不退化 + 撤销回帧。另有 `SetParaMarkFormat` 往返与守卫、
撤销重做栈状态、"按码点不按字节"的断言。

### C 段 策略改走 edit_capability（已落地）

口径：持树路径的编辑判定与旧路径**同一份结论**，持树路上不再问旧 `EditPolicy`。引擎内只留一个分叉点 ——
`policyAt(TextPos, 归属提示)` / `policyErase(TextPos, TextPos)`：持树先过桥换 `FlowPos` 再问
`canEditAt`/`canEditRange`，不持树才问 `EditPolicy::canInsert`/`canErase`。六个编辑入口（插入/退格/前删/
分段/合并/反缩进）全部改调它俩。

- **安全阀镜像**：表单模式下"一个可编辑控件都没有"→ 旧路 `EditPolicy` 退化放行；持树路必须同款
  （`formDegraded()` + `hasEditableControl()` 两路共用），否则同文档换路装载行为就变。
- **归属提示两条路都留**：共享边界上"前一控件末尾 / 本控件开头"是同一码点偏移。无提示且持树时才回落
  "**最内层含端点**"（否则桥取最外侧＝"控件之前"⇒ 字段起点编辑被拒，表象"光标停住却打不进字"）。
- **Tab 带归属**：新增 `tabCaret` 走 `tabFrom`（到最后一个可编辑控件就放行、不绕回；文档/只读模式不消费
  Tab）。`TabStop` 增 `owner`（`NodeId`）—— 落点是控件内容起点，共享边界上重推归属会推成前一控件
  （表象"Tab 跳过去却打不进字"）。web 端加 `tabCaret` 包装，默认键位不绑。
- ★ 硬边界：**光标位置不受控件约束**（字段外/只读字段里都能停），受约束的只有**编辑**与一次 Tab 跳到哪。
  C 段只换编辑判定，**不碰光标移动** —— 第一轮照"编辑与移动一起受限"写，27 条判据当场红，全回滚。
  （原计划里的"`moveCaretBy` 走 `stepEditable`"由此**取消**：它与这条已定规则冲突。）

判据（edit_test [28] 四组）：落点 0..9 允许位两路**逐位一致**（实测 `0000011100`）、被拒次数相同（7/7）；
带点击归属 `setCaretAt(0,4,1)` 两路都放行、可编辑控件长成 `[4,8)`；安全阀（只有只读控件时表单退化，
控件外也能打字）；Tab 落点 `4`、`caretFieldHint()==1`、跳过后字真的进那个控件、最后一个可编辑控件上再按
返回 false、文档/只读模式不消费。model_test `checkTabStops` 补 `owner` 两条。

### 演示页切到树上（已落地）

web 演示页从"拼块流"（`setBlocks` + `setControls`）改走 `setDocument`：`demo/demo_documents.cpp`
的 `demoTree(blocks, fields)` 把场景块流建成模型树（块级字段搬到段落结束标记、字段区间换成内容
控件）。宿主只留 `g_blocks`（底稿）+ `g_fields`（字段），换场景 / 换字段 / 增删控件都是"改这两份
再重编一次"。`clearControls` / `addControl` 在持树时也走这条路（不再是旧 `setControls`）。

三条与老路径对齐的次序规则：
- **块级字段逐字段搬**（不"只搬与默认不同的"）：少搬一个就静默换成默认值。
- **空字段不参与嵌套**：开闭同处一列，老路径把它那一对排在"到此为止"的闭括号之后、后一个字段的
  开括号之前 —— 只有与包住它的字段**并列**才画得出同一个次序（`][][]`）。
- **归属提示过 `slotOwnerOf`**：占位簇记的是产生端句柄（持树时是控件 id），光标侧别/命中测试/宿主
  报的都是下标。

★ 切过去才露出来的三处真缺陷（都已修）：
1. `policyErase` 求端点流位置**没带归属提示** ⇒ 桥取最外侧（控件外）⇒ 字段删不空（"删到只剩一个字
   就删不动了"）。修：两端都传 `editOwnerHintAt`。
2. `caretRectFor` 拿控件**下标**去和占位簇的 `owner_control`（节点 id）比 ⇒ 共享边界上光标画到
   另一侧。修：新增 `slotOwnerOf(下标→句柄)`，比之前先翻。
3. 宿主 `g_blocks` 在树上编辑后**过期** ⇒ "改控件再重建"把用户改过的内容倒回装载时的样子。修：
   `syncBlocksFromEngine()`（重建前先取回引擎里那一份）。

判据：edit_test **[29]** 十组（两条路帧逐字节相同、零宽+重合只建三枚、删空可删、偏移 7 上
`][][]`/`0 1 1 2 2`、零宽字段打字放行、句柄往返）；浏览器探针 **192 项全绿** —— 用户报的
**三条边界现象各自都有专门断言且全过**：①点三层各自的第一个字归属 `0 1 2`；②删空之后光标仍停在
那个零宽字段上、字进它自己（`[[5,7],[7,8],[8,8]]`）；③删空的内层仍画成一对相邻 `[]` 且嵌在中层
边界之内（`[[5,9],[6,8],[7,7]]`）。

**已收掉**（2026-10-13 旧路径退役）：`ControlSet` 的四路手工维护（`adjustControlsOn*`）、
`syncInlineObjects` 的产边界分支、`EditPolicy`、旧 `setBlocks`/`setControls`/`clearControls`/`addControl`
与 `tree_valid_` 分叉 —— 详见下文"## 旧路径退役"。`ControlSet` 本身保留，地位变了：它是**编译产物的
扁平投影**（只读），供宿主与几何用。

### 括号边界上的两个位置（点选侧别，已落地）

用户报的第二批现象："反字和后面的左括号之间""天和左边的右括号之间"光标都进不去。根因不在归属，
在**渲染**：同一处边界上"控件之前"与"进入控件"是两个流位置（flow.h），而 `inlineOffsetFor` 无归属时
一律贴括号内侧，所以"括号之前 / 括号之后"这两个位置**画不出来**，点下去光标被贴回内侧。

修法（三处，缺一不可）：
- **渲染**：新增 `LineBox::outerOffsetAt(cp)` = 该偏移上"所有控件之外"那个位置（所有闭括号之后、
  所有开括号之前）；`inlineOffsetFor` 认 `kInlineOffsetOuter`（= -2）走它，另外"归属控件在这一处
  没有自己的边界"（落点在它内容的某个嵌套边界上）也走它 —— 于是点"天"不再被拉回 `]` 之内。
- **命中测试**：括号占位簇按**左右半边**分（与真实字簇同一条中点规则）。开括号左半边、闭括号右半边
  都是"控件之外"，归属给 `kInlineOffsetOuter`；另两种半边照旧按括号主人算。
- **归属链**：`caretFieldHint()` 认这个哨兵，`slotOwnerOf` 把负数原样传给排版，`setCaretAt` 收下它。
  无归属（键盘移动、`setCaret`）**不变** —— 字段起点仍画在开括号之内（那条是被老判据钉住的）。

判据：edit_test [29](g) 六条（外侧落在括号左缘、内侧落在它之后那个字、两侧差 5.55pt；
`hitTestPt` 在开括号左半边 / 闭括号右半边给出 -2）+ 浏览器探针 [14.7] 三条。

### 光标侧别：位置 = 偏移 + 侧别（已落地）

用户报的第三批现象：**方向键在括号边界上一下跳好几格**（"反"前按左键跳到"冒号"前、"天"前按左键跳到
"悸"后面），外加"三个控件挨着、把内容删掉之后两个控件之间点不进光标"。根因是结构性的：光标只存
(偏移, 归属)，而**同一个偏移上可能压着好几枚括号** —— "控件之前 / 进入控件 / 内容末尾 / 控件之后"
是互不相同的插入点，一个偏移 + 一个控件下标表达不了。用户的原话是"控件边界都当成一个普通字符了，
怎么还这么特殊" —— 括号在排版侧本来就是一枚簇，缺的只是让光标也按"簇"来数。

修法（一个概念，四处落点）：
- **状态**：`caret_side_` = 在这个偏移上跨过了几枚括号（0 = 所有括号之前），与 `caret_` 成对；
  `caretFieldHint()` 由它推出来（不再是独立状态），`caretSide()` 公开给宿主与探针。
- **几何**：`LineBox::offsetAtSide(cp, side)` 取代 `inlineOffsetFor` / `outerOffsetAt`
  （后者有真 bug：同一偏移上多枚括号时它返回**第一枚开括号**的位置，而不是"所有闭括号之后"）。
- **桥**：`flowPosAtSide(para, cp, side)` 是底层，`flowPosAt(para, cp, hint)` 退化成"提示 → 侧别"。
  `relAt(cp, side) = cp + 排在它前面的边界数 + side` —— 段内相对流位置与"簇边界序号"是同一个数。
- **移动**：`moveCaretBy` 在侧别上逐格走（同偏移上 `side±1`，走完了才跨到下一个簇）。
- **点击**：`hitTestPt` 多给一个 `out_side`（`side` 是落点的完整身份，`field` 只是它的投影）；
  宿主与 `app.js` 搬 `side`，`setCaretAtSide` 是那条入口。
- **默认侧别**：`defaultSideAt` = "最内层含端点"（`controls_.innermostAt(p, true)` 再翻成侧别）——
  与编辑归属同源，相邻空字段于是停在它自己那一对括号之间（"字打不进去"那条老毛病）。

顺手收掉的两个真缺陷：
- **陈旧侧别会复活**：位置变过又变回来（编辑把光标挪走、又落回同一偏移）会骗过"位置相等"这一条。
  修：所有不带侧别的写入走 `setCaretRaw`（24 处赋值 + 排版收口一处）。
- **聚焦档死结**：`focusedControlIndex()` 若只认推出的归属，就会变成
  `bracketedControls → focusedControlIndex → 边界对象表 → bracketedControls` 的循环
  （首次排版前一个括号都排不出来）。修：保留"最内层含端点"回落。

**旧路径的口径没动**：`caretFieldHint()` 只在持树时由位置推；旧路径仍按区间归一次（`innermostAt`），
`policyAt`/`policyErase` 的旧分支一个字没改。对象表也从 `blocks_full_` 改走 `objectsFor(para)`
（持树取产物、旧路取 `blocks_`），否则旧路径读到空表。

判据：edit_test 从 672 涨到 **678**（方向键走动改成断言 (偏移, 侧别) 序列、零宽字段的默认落点、
`flowPosAt` 无提示 = 最内层）；浏览器探针从 195 涨到 **199**（新增 **[14.8]** 四条：反字前 / 天字前
各按一下左键只退一格，像素位移 6.00pt / 7.00pt ≈ 一枚括号宽）。另有 [14.2]/[14.3] 的走动判据改成
"一次跨一枚簇"的口径。

### 编辑落点由流位置给出（不猜）

用户报的第四批现象：**嵌套删「天」之后光标落到 ]中 之前**、**三连删「悸」之后光标跑进第三个
控件** —— 都是"删除之后落点猜错了侧别"。上一刀的"侧别"只解决了点击与几何两处，编辑落点与
删除区间仍在用"最内层含端点"猜。

评估结论：**模型对，实现差一层** —— 模型里括号是流里的普通元素，"一次一个字符"就是流位置 ±1；
而引擎的光标存的是 (段, 码点偏移)，这个坐标不含括号，每次要问"流里哪个位置"都得猜。

修法：`setCaretAtFlow(FlowPos)`（位置 + 侧别一次算准）与 `textSpanAt(para,a,b,&from,&to)`
（文本码点区间在流里占的那一段；判定与真删共用）。六个入口改用它：删除、插入、分段、合并、
Tab、撤销/重做；`moveCaretBy` 改成流位置 ±1。复现脚本 `scripts/diag_del_jump.mjs`。

顺带修掉两个真缺陷：`cpOffsetInBlock` 直接下标 `blocks_full_`（旧路径那张表是空的 ⇒ 崩溃）；
`relayout()` 开头多了一次簇吸附（用上一轮簇表把光标拉回旧段长 —— 探针 [2b] 帧差 186 像素）。

判据：edit_test 678 → **681**（[29](h) 三条）；浏览器探针 199 → **202**（[14.9] 三条）。

### 光标唯一真源：`caret_` = {段, 段内流位置}（已落地）

用户报的第五批现象：**三层嵌套字段全删空后、在最后一枚右括号之前输入，字与光标都跑到第一枚
左括号之后**；**三连字段后两个删空、在第二个字段里回车，光标跑进第三个字段**。用户问到了根上：
"为什么 `caret_` 只数字符、不数括号？"

评估结论：**模型对，光标存了两套坐标。** 模型里括号是流里的普通元素，"一个字符一个字符地移动"
本来就是在流位置上加减一；而引擎的光标存 `(段, 码点偏移)` 加 `caret_side_` / `caret_side_at_`，
后两者与位置天然可能失配 —— 才需要失效标记兜，而"飘"就是这个机制。

修法（一个概念）：`caret_para_` + `caret_rel_`（段内流位置，括号占位算在里面）是唯一真源；
`caret()` 与 `caretSide()` 都是派生量。落点入口收敛成 `setCaretRel`（唯一写入点）与
`setCaretAtFlow`；宿主程序化定位与 Home/End 走 `projectCaret`（`defaultSideAt` 是文本坐标→
流位置的唯一约定）。`moveCaretBy` = `rel ± 1`，在字素簇边界上停。

三个真缺陷（本刀暴露）：① `syncInlineObjects` 的"清表"排在"问聚焦字段"之前 ⇒ 光标被按空表
解释成另一个偏移、聚焦字段换人；② 宿主整批重建控件期间的中间态让侧别被抹成 0
（`app.js` 的 `setFormControls` 现在整批前后各记/摆一次）；③ `rel` 相对边界表而存在 ⇒
持树那条路必须用全部边界（`objectsFor` → 编译产物），旧路径只有显示表是残留风险。

判据：edit_test 681 → **688**（`[29](h)` 三条现象判据）；浏览器探针 202（`[2b]` 那条
"删空之后字进它自己"从失败变通过）。诊断脚本 `scripts/diag_zero_width.mjs`。

## 旧路径退役（第七刀 D 段，2026-10-13 已落地）

用户指令"移除旧路径吧"。这一刀把引擎里**两条装载/编辑路径收敛成一条**：树是唯一入口。

**前提三条先定住**（缺一条都不能删）：
1. **跨段控件在树上表达得出**（字段只支持段内区间这条老限制不成立）——31 处成对 start/end 脚本扫过，
   无一跨段。
2. **零宽字段在断点上分段要留着**（真缺陷，旧路靠 `adjustControlsOnSplit` 手工账才绿）—— 已修，判据
   见 `scripts/diag_split_empty_field.mjs`。
3. **换控件丢的"只重排受影响段"要补回**（旧 `setControls` 只标脏最靠前控件所在段）。

**删掉的 8 项 × 替身 × 为什么能删**（表在 `docs/正式化架构与迁移路线.md` 4.2 末尾）：
- `setBlocks` / `setControls` / `clearControls` / `addControl` → `setDocument` / `setFormControls`
- `tree_valid_`（27 处分叉）→ `loaded_`（只区分"装载过没有"）
- `owner_index_` 的"空即下标"回落 → `controlIndexOfOwner` 未命中返回 -1
- `objectsFor` 双分支 → 只读编译产物 `blocks_full_`
- `syncInlineObjects` 的产边界分支 → 只剩"筛显示名单"（排序是遍历的自然产物）
- `EditPolicy`（整个文件）→ `edit_mode`（`EditMode`）+`edit_capability`（`canEditAt`/`canEditRange`）；
  引擎自己持 `mode_`
- 四个 `adjustControlsOn*` → 区间是编译产物的派生结果，不再手工维护
- `policyAt` 的 `owner_hint` 参数 / `editOwnerHintAt` → `policyAt(TextPos)` 一个参数

★ **`tree_valid_` 的收敛办法**（别让后来人重新推）：它的每一处分支都是在区分"树有 / 没有"，处理办法是
**树路径下两边同时跑一遍** —— 编辑入口一律只留树分支；审计入口用 `loaded_` 保留"没装载过时返回 0 /
`{"diff":0,"note":"not-loaded"}`"。只有一处例外：`tabCaret` 末尾"退回 `focusControl`"那一支被**结构性
删除**，因为 `focusControl` 给的目标与 `tabFrom` 的口径不同。

**顺带修正**：模型文档第七节一处硬伤 —— 原文写"表单模式落点受限"，与代码（`moveCaretBy` 无模式判定）、
路线图 4.3 表格、探针 `edit_probe.mjs` 三处证据矛盾；已改成"落点不受限（与编辑模式逐项相同）"并注明
"早期草案"。

**判据**：native model 803 / edit 706 / shaping 55，浏览器探针 202，全绿。

## 判据规模

`model_test` **809**、`edit_test` **858**、`heditor_tests`(shaping) **55**、浏览器探针 **256**、
头依赖复核 **45 个 obj / `#deps 0` 0 条**，全绿；不变量可判定 **11 条 / pending 4 条**，
**判据必须含反向一半**。

## engine 层拆分（2026-10-18）

**已落地第一刀：光标与位置解析 → `src/core/engine/caret.h` / `caret.cpp`（类 `CaretResolver`）。**
拿走：位置代数（流位置↔文本坐标、侧别与归属提示、括号统计、簇吸附）、选区与光标状态
（两端 / 行别 / 列位 / 光标条宽度 + `setCaret*` 一族）、行盒几何与命中（行盒查找、光标与选区
矩形、按页二分、`hitTestInLine`）、光标移动（方向键 / 行首行尾 / 文档首末 / 上下键）。

留在引擎：帧像素与帧槽偏移、`hitTestPt`（要页像素步距）、**重排触发**（聚焦档下移动光标会换
占位集合）、编辑 / 编译 / 绘制 / 审计。输入是只读视图 `PositionView`（9 个指针）。

★ 视图的两个失效点（TRAPS 81）：摆的时机（`init` / `setDocument` / `rebuildFromTree` /
`relayout` 各摆一次）；**流索引要持 `std::optional` 的地址**，不是它内容的地址。

判据：三套 native 一条不改全绿（104 / 1129 / 809）+ 浏览器串行全绿。
`engine.cpp` 2903 → 2201 行。**已落地第二刀：命令层 → `src/core/engine/edit_command.h` / `edit_command.cpp`（类 `EditCommand`）。**
拿走：插入族、删除族（含控件边界四条规则）、跨段族（分段 / 合并）、缩进族、结构操作
（删控件 / 改控件属性）、选区删除的公共前缀（`planEraseSelection` / `commitErase` /
`replaceSelectionWithText` / `eraseFlowSteps`）与落点判定 `policyAt` / `policyErase` / `marksAtFlow`。

留在引擎：**编译产物重建**（命令只交事务，`EditHost::commitEdit` 是出口）、`loaded_` 检查、
`markAllLayoutDirty`（由 `PropResult::Structural` 带出去）、排版 / 绘制 / 帧与窗口 / 控件查询
/ 光标移动。输入与出口是两个接口 `EditView` / `EditHost`；`EditorEngine` **私有继承**
`EditHost`（虚调用，不用 `std::function`）。★ `ParaDelta` 从 `EditorEngine` 内嵌提到
`edit_command.h` —— 命令层要构造它，而它不该 include 引擎头。

判据：三套 native 一条不改全绿（104 / 1129 / 809）+ 浏览器串行全绿。`engine.cpp` 2178 → 1373 行。

## 阶段 4：行内原子（2026-10-18，已落地）

**模型**：`ImageSpec` / `FormulaSpec` 挂在 `Node` 上（资源标识、像素宽高、DPI、显示尺寸、
定位与环绕枚举 / 表达式、排版后端、显示尺寸）；不变量加"结构.原子的尺寸可定"；
序列化与 `NodeSpec` 同步补字段（★ 漏了 `NodeTree::assignFrom` 与 `NodeSpec` 会**静默丢**
这两个字段 —— 往返判据抓到的）。

**编译**：`Image` / `Formula` 各产一枚 `InlineObject{kind=Atom, w_pt, h_pt, atom,
content_hash}`，盒宽高在 flush 时定（公式的默认框要按段落字号估）。

**排版**：原子用自己的盒宽、`break_after=true`、盒高进行度量（`LineMetric::box_h_pt`），
行高取 `max(字号 x 行距, 盒高)`；竖排取 `max(列宽, 盒高)`；空段落分支同样处理。

**引擎**：`BracketCounts` 按"开 / 非开"分桶（原子进后者）⇒ `closes + opens` 仍是对象总数；
原子不被括号名单筛掉；后缀复用不把原子当成"端点会动的占位"；底图里画显式占位框
（边框 + 浅底 + 标签），行指纹混原子种类与内容指纹。

**判据**：`shaping_test [8]`（104 → 113）、`edit_test [44]`（1129 → 1144）、
`model_test` 812；浏览器 `out/_atom.mjs`（底图里真有那个边框色，无原子场景回到 0）。
注入旧行为：忽略盒高红 3 条、忽略盒宽红 4 条。

**下一刀**：按 4.7 的表，剩的三项分别被阶段 5（表格）、阶段 6（序列化）卡着
—— 也就是**引擎侧的拆分暂时做完了**，接下来该回到阶段 4/5/6 本体。

## 性能（阶段 1 之后的独立刀）

- **增量编译**（2026-10-13）：`compileChildRange` + `rebuildParagraphFromTree`，条件先判完
  再动手，不满足退回全量。编辑时编译 3000 段 **1.8ms → 0.000ms**，判据 `auditCompile()` 与
  整树全量逐字段一致（edit_test [30] 12 条）。
- **流索引去哈希**（2026-10-13）：`first_` / `span_` 从两张 `unordered_map<NodeId,int>` 换成按
  id 下标的 `std::vector<int>`（`NodeId` 是 `next_id_` 水位）。编辑时 **1.3ms → 0.000ms**，
  公共接口未动。⇒ 编辑路径上 O(文档) 项清零。

## 呈现的粒度：帧 = 视口窗口，画布 = 一页一张（2026-10-13）

**病根是量出来的**（`scripts/diag_perf.mjs`）：帧原来等于"整篇一张位图"——200 段（16 页）
1.5 倍缩放下 893 × 20523 = 18.3 Mpx，3000 段 893 × 82155 = 73 Mpx（引擎两张位图
`base_` + `bitmap_` 就要 587 MB wasm 堆，且帧高远超 GPU 纹理上限 16384）。
★ **判据**：同一串敲字，画布 `visibility:hidden` 再跑一遍 —— 18.3 Mpx 时帧间隔
**33.3 → 16.7 ms（差整整一个 vsync）**，1.13 Mpx 时差 **0.0 ms**。
这一笔不在任何引擎计时里、也不在 `putImageData` 里（后者惰性：73 Mpx 与 1.13 Mpx
贴同样一小块都返回 0.000 ms）。

**引擎侧（纯加法，默认行为逐字节不变）**：
- `setViewportAnchor(first, count)` → `repaint()` 只呈现 `[first, first+count)`；
  `count <= 0` = 整篇 = 原行为 ⇒ 单测与既有探针一行都不用改。
- `dirtyPages()`（本轮写过的绝对页号，升序）：口径与原来那份上屏 bbox **完全一致**
  （同一批矩形算出来的），但比"脏页数"更准 —— 光标与表单标记画在帧上、不进底图，
  它们所在的那页同样要重贴，页指纹里没有这两样。
- `windowFirstPage()` / `windowPageCount()` / `pagePxWidth()` / `pagePxHeight()` /
  `pageGapPx()` / `caretPage()`。
- ★ **`EditStats::total_pages` 仍是文档页数**，窗口那几页看 `window_pages` /
  `window_first_page`。合成一个数会让"脏页 / 总页"恒等于 1，而且宿主算窗口时会把整篇当成 1 页。

**宿主侧（`web/index.html` + `web/app.js`）**：
- `#stage` 滚动容器 + `#pages`（撑出全文高度）+ 窗口内每页一张 `<canvas>` 绝对定位。
- 上屏 = `putImageData` 的源矩形切片：`putImageData(img, 0, 0, 0, k*step, pw, ph)`，零拷贝；
  只贴 `dirtyPages()` 报的页。
- 窗口 = 可见页 ∪ 光标页，封顶 `MAX_WINDOW = 3` 页，**带滞回**（还盖得住就不动）。
- ★ **换文档前先 `presetWindowForLoad()`**（锚点摆到开头 + 滚动回顶）：顺序反了会先按
  上一篇文档的锚点栅格化、再挪窗口重排一次，"装载花了多少"被后一次覆盖掉。
- ★ **`settle()` 的顺序**：先滚光标入视口 → 再对窗口 → 再上屏 → 再刷面板。
  先对窗口的话会按旧的滚动位置算窗口（光标在第 0 页、屏幕停在第 15 页 ⇒ 拉成整篇）。
- 兼容层 `frameCanvasProxy`：老探针把"帧"当一张画布用（取 rect、派 mousedown、
  读 width/height），代理按"窗口帧在屏幕上的那块区域"给几何、事件转发给页容器。
- `__heditor.blitBox` **已退役**（改 `__heditor.dirtyPages`）：一次上屏贴的是若干整页、
  不是一个矩形；引擎的 `takeBlitBox` 仍在，native `edit_test` 用它钉"两次 present 的账不互相挤掉"。

**效果**：扫 zoom（1.0/1.5/2.0/2.5/3.0，帧 1.01 → 9.10 Mpx）实测，帧间隔**不再随画布面积走**，
只由单拍操作耗时决定，合成那笔在 4 档上都是 0.0 ms。3000 段的帧从 73 Mpx → 3 页 / 3.4 Mpx。

**剩下的一半**：单拍操作 13~20 ms，**字形光栅占 7~13 ms**（wasm 走 Skia；整页约 1000 个字形
每帧从头画一遍，改一行与改一页同价）⇒ 行级重光栅是下一刀，与画布无关。

**判据**：native model 803 / edit 706 / shaping 55，浏览器探针 202 + 压力探针，全绿。

## 行带重画：页粒度 → 行粒度（2026-10-13，第二刀）

**病根**：页指纹说"这一页变了"，于是整页重画 —— 一页约 1000 个字形，而敲一个字
只动其中一两行。实测字形光栅 **10~14 ms**，其中九成是白画的。

**做法（与页指纹同一把尺子、同一次遍历）**：
- `line_fp_new_` / `line_fp_in_base_`（下标 = `LineBox::line`，全局单调）。
  指纹含行盒几何（block_lo/hi、inline_origin）＋ 簇表 ＋ 这一行的字形（id/位置/朝向/颜色）。
  少一样就会出现"字形没重画但确实变了"的幽灵墨迹。
- `line_lo_in_base_` / `line_hi_in_base_`：底图里每条行带的块轴范围（页内磅）。
- `presentInternal` 的增量分支：页指纹变了 → `computeBand(page)` 用行指纹找出
  **连续的一段差异行 [d0, d1]** → 只重画这一条带，帧那边也只贴这条带。
  `band` 返回 -1（行指纹全同，只有页数/页间隙这类页外属性变了）或 0（**竖排页**）
  时退回整页重画。
- ★ **带边界取"与相邻行盒的中线"**，不是这一行自己的 block_lo/hi：行盒给的是
  ascent/descent，而个别字形（标点、带钩部件）的墨迹会探出这个范围。取中线之后
  相邻两行的带正好相接、互不重叠 —— 擦一条带绝不会碰到没重画的那一行，
  而这一行自己的全部墨迹一定落在带里。★ 边界还要取新旧两版里更靠外的那个，
  否则行往下挪之后旧墨迹留在原地（幽灵字）。
- ★ **页边框必须每次补画**：它在页矩形的边上，而带底色的填充会把它擦掉一条。
  漏掉这一笔的后果是"增量帧与从零重画的帧差 **4308** 像素" —— 正好是
  `2×(893+1263)-4`，页边框的长度。这条是量出来的，不是推出来的。

**效果**（`scripts/diag_perf.mjs`，200 段 / 16 页 / 1.5 倍）：
- 单拍操作 **21.2 → 5.0 ms**；引擎 paint **12.2~17.0 → 0.8~1.2 ms**
  （字形 **9.6~14.2 → 0.7~0.8 ms**）。
- 重画像素 1.14 M（整页）→ **147 k（占全帧 4.3%）**。
- **帧间隔 33.3 → 16.7 ms，30 拍里 0 个慢帧** —— 持续按键/退格不再卡。

**判据**：native model 803 / edit **707** / shaping 55，浏览器探针 **204** + 压力探针 69，
全绿。其中两条是这一刀真正盯住它的：
`edit_test` 里"敲字之后：增量帧 == 从零重画的帧（逐像素）"、
`edit_probe` 里"屏幕像素 == 引擎帧 / 帧逐字节相同：增量重绘 == 重新排一遍"。
★ 顺带改掉两条**过期判据**（"该页整页重画"、"每一次按键都整页重画"）：它们编码的是
旧的页粒度，行带之后不再成立 —— 判据跟着粒度走，不能让它变成一句空话。

## 第三刀：批量画字形 + 画布池 + 窗口策略（2026-10-14）

**病根（截图 + 探针量出来的）**：回车 48.7 ms 里 **paint 40.7 ms**（3 页全窗重画），
layout 只有 8 ms。所以不是"该不该重画"，是**重画一次太贵**：`drawGlyph` 是
**一个字形一次 Skia 调用**（每次新建 SkPaint、装配一遍字形运行、算一遍包围盒），
一页约 700 个字形就是这么变成 10 ms 的。

**做法**：
- `Bitmap::drawGlyphs(ids, xs, ys, count, ...)`：同一字体/颜色/朝向攒成一段一次交出去。
  `drawGlyph` 退化成 `drawGlyphs(count=1)` 的壳（两条入口只能有一条实现）。
  侧倒竖排退回逐字形。自写后端只逐个转调（那一侧固定开销本来就小）。
- 记账改批量：`SkFont::getBounds` 的批量重载一次取完包围盒。
- 新增 `Bitmap::paintedGlyphs()` / `EngineStats::painted_glyphs`：与 `paint_glyph_ms`
  配对算"每字形微秒"。★ 记**底图**不记帧（字形只画在底图上）。
- 宿主 `canvasPool`：换窗口时卸下来的页画布留着复用（钱在重新分配 backing store）。
- `desiredWindow` 以**可见页**为准，光标页只在挨着可见页时才并进来。
- `scrollCaretIntoView` / `positionIme`：光标不在窗口里时按页算 / 收起输入框。

**效果**（`scripts/diag_perf.mjs`）：
- 回车 **48.7 → 22.9 ms**（paint 40.7 → 13.6）；装载 200 段 **190 → 100~105 ms**。
- 快速滚动（每拍跳一整页）：帧间隔 **p50 50 → 16.7 ms**，慢帧 **10/12 → 4/12**。
- 画布重建 vs 复用：4 张 6.7 vs 2.0 ms。
- ★ 字形缓存认位置：同一文档连装三次 53.4 / 13.7 / 15.9 ms（2166 个字形）
  ⇒ 冷启动 24.65 µs/个、之后 ~7 µs/个，这就是"整页重画"的地板。

**判据**：native model 803 / edit 707 / shaping 55；`edit_probe` + `stress_probe` 全绿。

**剩下的账与下一条杠杆**：回车 22.9 ms = layout 3.3 + pageFp 1.0 + 字形 9.7 +
铺面积 2.6 + 帧搬运 1.3 + 上屏 1.5。① 段数变化时复用后缀（省 ~4 ms，需编辑侧给出
"插入了几段"）；② 行级位图平移（需先让纵向位置落在整数设备像素上，否则亚像素混合会让
"增量帧 == 从零重画的帧"这条硬判据失守）。

## 第四刀：段数变化也走后缀复用（2026-10-14）

**病根（用户实测截图）**：10002 段的文档里回车，`layout 330 ms + pageFp 84.6 ms`，总 361.7 ms。
账本写着"重排段 9999 / 复用 0" —— 后缀整段被重排了。原因不是复用坏了，而是
`relayout` 里那道门 `!para_count_changed_` 把整条路封了：判据是"第 from+1 段开始时的状态
与上一轮相同"，而段号一变"第 from+1 段"已经不是同一段。判据没错，错在只认那一种情形。

**做法**（`trySuffixReuse` 泛化）：
- 脏区在新旧两版里各占几段：段数没变时各占一段；段数变了时**窄的那一侧恒为一段** ——
  段落数的变化只可能来自插入或删除一枚段落标记，而一枚标记只牵动它所在的那一段
  （插入：1 段拆成 2 段；删除：2 段并回 1 段）。哪一侧是"1"由段数增量的符号决定。
- 拼接从"替换一段"推广到"替换 m_new 段"：行/字形按两端下标取脏区范围；
  段表先按 d_para 挪槽位再调长度（**往后挪要先扩容从尾到头搬，往前挪要最后才缩容** ——
  反过来会读越界，搬过去的是垃圾而排版看起来"成功了"）；
  **行盒与字形的段号字段要跟着 d_para 整体平移**（光标几何与审计都按它找行）。
- `shiftSuffixBlockAxis(from, first_glyph, after)`：`from` 改成"后缀第一段"（不再是脏段），
  内部循环起点随之从 `from + 1` 改成 `from`；`first_glyph` 单独给（脏区可能占好几段）。
- ★ **段数变化几乎总落到二级复用**：段落边界一变就多让（或少让）一次 `space_after`，
  后缀必然整体下移 —— 那不是"同状态"，而是"纯平移"。所以放行的是平移那一级。
- 删掉 `para_count_changed_`：它唯一的用途就是封那条路。

**判据（新增 3 条 + 改了 3 条过期期望）**：native model 803 / edit **715** / shaping 55 全绿。
新增：回车分段 → 重排 2 段 / 复用 99 段（4 段文档是 2/1）；退格合并 → 1/99；
两条都带 `auditLayout() == 0`（增量 vs 全量逐字段一致）。改掉的三条编码的是旧行为
（"段数变了就后缀不复用"），其中随机 40 步那条改成两侧判据：整篇重排只出现在"脏段是第 0 段"
那几步，且"没有后缀"那几步必然整篇重排。

**效果（浏览器实测，`loadStressDocument`）**：
- 10000 段在第 3 段回车：**361.7 → 66.8 ms**（排版 **330 → 10.4 ms**，重排 2 段 / 复用 9996 段）。
- 2000 段同理：16.0 ms（排版 1.7 ms）。
- 剩下的热手是**页指纹 33.9 ms**（平移那一级要把后缀所有页的指纹重算一遍 —— 它们确实都变了）。

## 第五刀：页指纹从 O(文档) 变 O(1)（2026-10-14）

**病根（用户截图 + 探针）**：2000 段文档里回车，面板是 `layout 2.9 / pageFp 21.0 / paint 15.1`，
而"合计 18 ms"与手感对不上 —— 因为合计只算 layout + paint，**pageFp 不在里面**（真实约 39 ms）。
pageFp 那两笔账都是 O(文档)：

- **后缀整体平移那一级**（回车 / 合并 / 敲字敲到换行）：要把整条后缀的字形重新哈希一遍。
  可是"后缀每一页都变了"这个结论在进那里之前就已经确定 —— 后缀每一行都沿块轴挪了。
- **续排路径**（没有后缀可复用，例如在文档末尾敲字）：传 `fp_lo = -1` ⇒ 整篇重算
  （3000 段实测 7.5 ms，而那一步只动了最后一页）。

**做法**：

- `markPagesChangedFrom(first_page)`：把 [first_page, 末页) 直接标脏，不算哈希。
  标脏值 = `kFnvBasis | kFpDirtyBit`：高位是**保留哨兵位**（真指纹一律清掉它 ⇒ 标脏值
  不可能等于任何真指纹，这不是"大概率不同"而是编码上不相交），低位按底图那一版翻一下。
  行指纹同一把尺子：那些页上的行一并标脏（行带于是覆盖整页，与"每一行都挪了"一致）。
- `trySuffixReuse` 多一个出参 `all_pages_changed`，只有 Shifted 那一级置真。
- 续排路径框出脏页范围：脏段新旧两版的第一页取更靠前的那一页，一路到末页。
  页数变了（= 从脏段起重新分页）时同样走标脏 —— `computePageFingerprints` 遇页数变化
  会退回整篇重算，这里不必。
- `EditStats::total_ms` 补上 `page_fp_ms`。面板"合计"以前不含它：显示 18 ms 而操作实际 39 ms，
  看的人自然对不上手感 —— 报账口径本身也是这一刀的一部分。

**效果**（`scripts/diag_perf.mjs`，场景 7 = 2000 段 / 160 页 / zoom 1.5）：

- 回车·中段 **66.0 → 34.4 ms**（pageFp **21.1 → 0.0**；剩下 paint 24.5 = 窗口那 2 页整页重画）
- 回车·末段 21.4 → 14.8 ms（pageFp 10.5 → 0）
- 末段敲字（3000 段）14.4 → 6.4 ms（pageFp 7.5 → 0）
- ★ **pageFp 现在与文档规模无关**：10000 段回车 pageFp 也是 0.0，而 paint 在 2000/10000 段
  完全相同（21.6 ms）—— 它只由窗口那 2 页决定。改革前 10000 段的 pageFp 是 33.9 ms。

**这一刀踩的坑（TRAPS 第 33 条）**：标脏值第一版写成了**常量** ⇒ present 之后底图也变成
那个常量 ⇒ 下一次标脏读出来是"没变" ⇒ **连续换行只有第一次把文档推下去**。
判据必须落在帧上（`auditLayout` 只比排版结果，底图漏画它照绿）：`edit_test` 新增
"连续两次回车"两条 —— 一条脆的（画面变了多少像素 > 20000），一条有界的（与从零重画差 < 5000
且通道差 ≤ 4，抗锯齿在量化边界上翻格是允许的：平移走"旧坐标 + 增量"、全量走"从头累加"，
浮点末位可差 1e-3 pt，`auditLayout` 正为此留了容差）。
反向验证：退回常量标脏 → 画面变了 **84** px、与从零重画差 **750062**（通道差 231）；
修复后 → **750148** / **562**（通道差 3）。两组差四个数量级。

**判据**：native model 803 / edit **721** / shaping 55；`edit_probe` / `stress_probe`；
`lint_comments.py` 110 文件 0 违规。`diag_perf` 另加"连续两次回车"与"连按 30 次回车"两节。

## 第六刀：控件边界的可见性与"删/改控件"的入口（2026-10-14）

需求两条：编辑模式下看不见控件边界（无从增删改）；光标在控件后面按退格该删掉那个控件。

- **可见性**：文档与表单都占位、都画，只读不占不画 —— 三处同一条件
  （`bracketedControls` / `bracketPlanId` / `drawControlMarks`）。切模式仍整帧重贴。
- **入口**：`removeControl(index, keep_content)`（真 = 拆壳留内容）/ `setControlProp(index, prop, value)`
  （只收 hidden / readonly / required / structureLock）。两者只动树 + `commitOnTree` 一次重建，
  撤销/重排/重绘/记账自动一致；门复用 `canDoStructureOp`（`structureLock` **逐控件**）。
  另补 `removeControlAndRender` / `setControlPropAndRender`（幂等时不空跑重排），
  并把 `undoEdit` / `redoEdit` 暴露到 wasm 与宿主机门面（键位见第七刀）。
- **步骤原语**：新增 `UnwrapNode`（拆壳）/ `WrapNode`（包扎），互为逆操作、共用一份记录。
  ★ 坐标一律取"这一步将要应用的那个状态"里的地址：Unwrap 在拆之前（`at_flow` = 容器起点），
  Wrap 在拆之后（内容前移一格，`at_flow` = 内容起点）。
- **退格删控件**：判据 `hi == 光标流位置`（紧贴闭括号），且必须排在"段首退格 = 并入上一段"之前；
  策略拒绝时回落普通退格并记 `blocked_edits_`（不吞按键）。
- **新陷阱（TRAPS 39）**：槽位下标活不过一次规范化 —— 撤销能过、重做红。修法见 `steps.cpp` 的
  `slotForFlow` / `runForFlowRange` / `slotAtFlow`（第三条会改树，所以只能在 `applyStep` 里）。
- **构建侧发现（TRAPS 40）**：`build/native` 里存着 **15 个 `#deps 0`** 的条目 ⇒ 改头文件不重编。
  `ninja -C build/native -t clean` 后全量重编修好；复核 `#deps 0` 必须为 0、`engine/engine.h` 出现在 9 个 obj。

**判据**：native model **804** / edit **844** / shaping **55**，浏览器探针 **251**（新增 [15] 三模式占位与绘制、
[16] 真实按键 + 引擎命令 + 撤销 + 键位），全 0 失败；反向验证：退回"只看表单模式"⇒ native **6 项红**。
★ 拆壳之后"剩下的字段区间没被撑大"单列成判据（`edit_test` [35] 两处 + 探针 [16]）—— "内容被并进
前一个字段"也能让其它几条全绿，但不量区间看不出来。

## 第七刀：撤销键位与撤销粒度（2026-10-14）

用户两条：undo / redo 绑成 Ctrl+Z 与 Ctrl+Y；**撤销不该合并**。

- **键位**（`web/app.js` 的 `onKeyDown`）：Ctrl/Cmd+Z 撤、Ctrl+Y 与 Ctrl+Shift+Z 重做。
  必须插在"带 ctrl/alt/meta 一律放行"**之前** —— 否则按键被交给浏览器，而本页没有可撤销的
  DOM 编辑（键盘承接层是只读 textarea），表现为"按了没反应"。比较前转小写（Ctrl+Shift+Z 给的是
  大写 `'Z'`）。这一对是那条"修饰键一律不吞"规则的**唯一例外**，探针里两处都钉了。
- **粒度**：`canMerge` 退役（声明 + 定义 + `commit` 里的合并分支全删），`commit` 直接压栈 ——
  **一笔编辑一个撤销单位**。理由是粒度要一致：只对 `InsertText` 生效的合并会让"敲五个字一步退、
  退五个字要五步"。口径同步改到 `transaction.h` 文件头与 `docs/文档模型设计.md` 三处。
  ★ 粒度变细 ≠ 一个字符一笔：一次 `typeTextUtf8`（如 Tab 插 4 个空格）仍是一笔事务。
- **判据**：`model_test` 1.3 的"分组"小节改写为"粒度"小节（五笔 = 5 个单位、连撤五次逐次回到
  对应状态；接着的 / 加粗的 / 隔开的三笔 = 3 个单位）；探针 [16] 新增 (g) 键位段 7 条。
  ★ 每条判据要**自己摆前置状态** —— 第一版把"Ctrl+Y 重做"接在上一条 Ctrl+Z 的结果之后，
  键位整段没绑时它照样绿（文本停在原处）。改成"先门面 `undo()` 定性退回去，再派键位"才承重。

**反向验证**：① Ctrl+Z 分支改 `false &&` ⇒ 探针 **7 项红**；② 合并加回 `commit` ⇒ `model_test`
**2 项红**。第二轮验完即还原（`diff` 与备份逐字节一致）。

### 第七刀追加：深度上限与 Tab 的口径（同日再往后）

- **撤销栈有了深度上限**：`EngineConfig::undo_limit`，默认 **100**、0 = 不限制，超出时丢最旧的。
  取值依据是查了一圈主流实现：Office 系（Word / Excel / PowerPoint）默认**保存最近 100 个可撤销
  操作**（官方支持页，可用注册表改）；网页富文本里最接近的 Quill，`history.maxStack` 默认也是
  **100**，且超限时**丢最旧的一条**；Photoshop 50 个历史状态（可调到 1000）、Illustrator 100。
  同一条口径（丢最旧）在 `UndoStack::trim` 里，`setLimit` 调小时**当场裁**（不必等下一次提交）。
  ★ 与它们的差别要记住：粒度比 Quill 细（没有 1 秒的合并窗口），同样 100 条覆盖的输入更短。
  宿主入口 `window.__heditor.setUndoLimit(n)` / `undoLimit()`。
- **Tab 定为"保持缩进"**：`tab_spaces = 4`，两种模式一致；模型里预留的 `NodeKind::Tab` 与
  `tabFrom` / `tabCaret`（跳字段）都**没有入口、没绑键位**，排版层也没有制表位。四处注释与
  页面说明已按这个口径写清（避免"文档模式 Tab = 制表符"被当成已实现）。

**判据追加**：`model_test` 1.3 新增"上限"两组（六笔留三笔、调小当场裁、0 = 不限制、撤的是最后
两笔）；`edit_test` 新增 [36]（配置项真的落到栈上 —— 引擎漏转发时只有这一节会红）；探针 [16]
新增 (h)（门面 → wasm → EngineConfig → 栈 一整条线 + 默认值 100）。
**反向验证**：把 `commit` 里的 `trim()` 去掉 ⇒ native **3 项红**（model 1 + edit 2，报出"实际 6"）。

### 阶段 5：表格（2026-10-19）

六层一次推完，提交分六笔：表格行属性与单元格对齐、表格进编译、表格排版、表格跨页断行、
表格进引擎与格线绘制、表格演示场景与文档。

- **模型**：`CellSpec`（跨度 + `v_align`）、`RowSpec`（跨页断行 / 标题行 / 行前分页 / 行高三档）、
  `TableSpec`（宽度、对齐，删 `header_rows`）。标题行降为逐行开关 + 派生量 + 前缀不变量。
- **编译**：扁平化 + `cell_path`；单元格是段落边界；产物带 `LayoutTable`。
- **排版**：格矩形改两个变量；行高在行末结算；跨页两档（切片 / 整行挪）。
- **引擎**：`tables_` 进排版；有表格一律不做增量；段起点递推取"递推值与本块 first 的较大者"。
- **绘制**：逐格外框，跨页切口按 `cont_top` / `cont_bottom` 跳过。

判据 145 / 1176 / 845，浏览器 `out/_table.mjs` 7 项；六次注入各自命中。细节与两处坑见
台账 `2026-10-19.md`。**未做**：标题行重复、`TableStyle`、竖排表格、动态表格那一半。
