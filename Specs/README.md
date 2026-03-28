# Specs

> 目标引擎版本：UE5.5.4 | 适用范围：AGENT + UE5 可操作层 MVP

## 1. 目录用途

`Specs/` 存放结构化设计文档（Spec），用于把用户模糊需求转换为 Agent 可执行的结构化输入。

Spec 是 Agent 执行链的起点：Agent 不直接消费自然语言，只读取结构化 Spec。

---

## 2. 三层字段模型

| 层 | 面向对象 | 允许内容 | 示例 |
|---|---|---|---|
| 用户语义层 | 用户 / 策划 | 受控的自然表达 | "大一点""放在中间" |
| 设计规范层 | Spec / Planner | 已消歧义的语义字段 | `size_profile: large` / `placement_rule: center_of_room` |
| 执行技术层 | Bridge / Tool Router | 仅明确、可读回字段 | `relative_scale3d: [1.25, 1.25, 1.25]` / `location: [0, 200, 0]` |

Spec 文件属于设计规范层的产物。进入执行前，设计层字段必须通过默认规则映射为执行层字段。

---

## 3. 禁止进入执行层的字段

以下字段可以出现在用户输入中，但**不可原样出现在 Spec 的执行层字段中**：

`size` / `position` / `center` / `middle` / `near` / `far` / `big` / `small` / `proper` / `looks good`

详细禁用清单见 `Docs/field_specification_v0_1.md` 第 6 节。

---

## 4. 目录结构

```
Specs/
├── README.md          # 本文件
├── templates/         # Spec 模板
│   └── scene_spec_template.yaml
├── scene_specs/       # 场景/关卡/摆放类 Spec（后续使用）
├── gameplay_specs/    # 系统/玩法/配置类 Spec（后续使用）
└── examples/          # 可执行的小示例（后续使用）
```

---

## 5. 与其他文档的关系

| 文档 | 关系 |
|---|---|
| `Docs/field_specification_v0_1.md` | 定义 Spec 中允许的字段、禁用字段、默认规则 |
| `Docs/tool_contract_v0_1.md` | 定义 Spec 中引用的工具参数结构 |
| `Schemas/` | 定义工具输入输出的 JSON 结构，Spec 中的执行层字段必须与 Schema 兼容 |
| `AGENTS.md` | 定义 Agent 如何读取和执行 Spec |

---

## 6. MVP 阶段说明

MVP 阶段 Spec 生成由 Agent 内部完成，不需要独立部署 Spec Translator 组件。

MVP 阶段建议先使用人工编写的 Spec 驱动执行，确保执行链稳定后再引入自动 Spec 生成。
