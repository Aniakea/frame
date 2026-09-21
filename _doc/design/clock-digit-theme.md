# 「花火 Flower-Burst」花式数字字体设计

状态: Design(设计冻结 v1)

本文件是 clock main 花式数字字体的设计规范与验收依据,实现于 `tools/frame_tools/clock_digits.py`,规范资产位于 `_doc/design/clock-digits/`。按 GOV-004,本文与配套 host tooling 属于设计阶段交付;不声称任何 PoC/gate 通过(TEST-001),也不构成产品 clock plugin 代码。

## 1. 概念与需求引用

- [UI-004](../requirements-v4.2.md):clock main 使用 project-original flower/explosion contrast theme,white background 上显示 stylized `HH:mm`;dedicated resource-only MPB 恰好含 11 个 bitmap:数字 `0..9` 各 `48x80`、colon 一个 `20x80`;无 copyrighted character assets,无秒显示。
- [UI-002](../requirements-v4.2.md):纯 1-bit 单色、高对比、white background;不依赖灰阶、透明或颜色语义。
- [UI-005](../requirements-v4.2.md):`YYYY-MM-DD weekday` 行使用 core standard font,不使用本字体(全区域预览中仅以占位黑条示意)。
- [development-plan](../development-plan.md) PoC-B 验收第 7 条(参考):11 bitmap exact dimensions、无 seconds/character assets 的机器验证即由本设计的测试承担。

主题命名「花火」= 花(flower)+ 火(burst):**上部花开**——自由笔画终点绽放 5 瓣花;**角部火绽**——字形角部锚点迸发 3 射线火花。闭合字形(0、8)以角部迸发为主,开口字形以终点花为主,每个字形同时含有两种元素形成对比。

## 2. 构造系统(归一化几何)

所有几何以数字字高 H 为单位归一化定义,不存在硬编码像素常量;48x80 规范资产与任意目标尺寸出自同一几何:

参数 | 值(占字高 H 比例)| 说明
--- | --- | ---
数字单元宽 | `3/5 × H`(48/80) | 宽高比恒定
冒号单元宽 | `1/4 × H`(20/80) | 冒号宽度跟踪数字字高
主干笔画 | `0.10 × H` | 圆帽厚线段/贝塞尔采样折线
字身范围 | y ∈ [0.075, 0.925] | 顶/底安全边距 ≥ 2px(规范尺寸下 6px)
5 瓣花 | 外径 ≈ `0.18 × H` | core r=0.025H + 5 花瓣(v0=0.02H,长 0.07H,基半宽 0.029H)
种子圆点 | r = `0.05 × H` | 基线端点等不承载花的终点
迸发射线 | 长 `0.11 × H`,宽 `0.046 × H` | 每锚点 3 条,相邻夹角 35°

- 花瓣为水滴形(基宽渐缩至尖),指向花心外方向;花朝向由终点相对字形中心的离心方向自动决定,全家族一致。
- 每字形迸发锚点 1–2 个,依字形留白手工布点;射线帽端经边界校验,规范尺寸下全部墨迹落于安全像素区间:数字 [2,45]×[2,77],冒号 [2,17]×[2,77]。

## 3. 可放缩性与细节层级(LOD)

同一归一化几何按目标字高选择细节档,退化是确定性的几何切换而非后处理:

字高(像素)| 档位 | 花元素 | 迸发元素 | 主干笔画
--- | --- | --- | --- | ---
≥ 48 | Full | 5 瓣花 + core | 3 射线 | 0.10H
24 – 47 | Simple | 4 瓣花 + 1.25× core | 无 | 0.10H
< 24 | Skeleton | 终点为种子圆点 | 无 | 0.10H

最小推荐字高:Full 档 48px(4.2" 反射屏一臂距离清晰);Simple 档下限 24px;更小尺寸仅保骨架。`12:34` 在 96/64/48/32/24/16px 阶梯渲染见 `preview_scale_ladder.png`。

## 4. 区域合成规则(部分屏渲染)

- 输入任意目标矩形 `(x, y, w, h)`,合成函数按 5% 边距自适应字高并居中;整数像素对齐。
- **裁剪不变量**:墨迹严格不越出目标矩形(有测试断言,区域外墨迹数为 0)。
- 全区域规范预览(`preview_clock_400x300.png`)按 UI 布局以规范 80px 字高居中排布;子区域预览(`preview_clock_subregion.png`)演示 200x120 偏置矩形内的缩放合成与虚线边界。

## 5. 每字形注释

字形 | 骨架 | 花元素 | 迸发锚点
--- | --- | --- | ---
0 | 椭圆花环(闭合) | —(无自由端) | 西北、东南
1 | 干 + 旗 + 足 | 旗端 | 东北、西南
2 | 拱 + 对角线 + 基线 | 拱左端 | 东北、东南偏东
3 | 双碗 + 中腰内收 | 上左端、下右端 | 东南偏东(水平扇)
4 | 闭合三角 + 右干 + 横档 | 横档左角、顶点 | 西北、西南
5 | 顶横 + 左干 + 下碗 | 横右端、碗端 | 东南偏东
6 | 顶弧 + 左干 + 下碗 | 弧起点、碗端 | 东南偏东
7 | 顶横 + 对角线 | 横左端(足端为种子点) | 西南、东南
8 | 上下双环(闭合) | —(无自由端) | 西北、东南
9 | 开口碗 + 尾 | 碗起点(尾端为种子点) | 西北、东南
: | 双微花于 1/3、2/3 高度 | 两朵 5 瓣微花 | —

## 6. 颜色语义与文件格式

- **1-bit 语义**:bit=1 为黑墨,bit=0 为白(PBM P4 语义);无抗锯齿、无抖动、无灰阶。
- **规范资产(入库)**:`digit_0.pbm … digit_9.pbm`(48x80,payload 480 bytes)、`colon.pbm`(20x80,payload 240 bytes),PBM P4 二进制、MSB-first、行按字节边界填充(冒号 3 bytes/行 × 80 行)。
- **预览(仅 QA,不入产品)**:`*.png`(灰阶 8-bit,2x 最近邻放大),共 4 张。
- **派生产物(不入库)**:无头 packed 1-bit `.bin` payload(480/240 bytes)供未来 resource-only MPB 打包(PLUG-001/STOR-003),由 `--emit-raw DIR` 按需生成;`*.bin` 受全局 gitignore 约束。任意非规范字高的字形集由 `--glyph-height N` 按需生成,同样不入库。

## 7. 生成与验证命令

```sh
uv run frame-clock-digits --output _doc/design/clock-digits        # 重建规范资产+预览(字节确定性)
uv run frame-clock-digits --glyph-height 96 --output <dir>         # 派生缩放字形集
uv run frame-clock-digits --emit-raw <dir>                         # 派生 .bin payload
uv run pytest tests/test_clock_digits.py -q                        # 机器验证
```

测试断言:恰好 11 个字形文件且无多余字形资产;PBM 头与 payload 尺寸精确;committed 文件与生成器输出逐字节一致(确定性);raw payload 等于 PBM 位流;墨迹安全边距;数字墨量 0.18–0.40(实测 0.25–0.38),冒号 ≥ 0.10(实测 0.145);缩放保持宽高比;LOD 档位阈值;区域合成裁剪不变量与边距拟合。

## 8. 溯源与许可

全部几何为本项目原创(参数化骨架 + 花/迸发装饰),不包含第三方字体轮廓、位图或受版权角色形象(UI-004 禁止项)。资产与代码随仓库适用 GPL-3.0-or-later(见根目录 `LICENSE`/`NOTICE`)。
