# 贡献指南

感谢你对 dztrader 项目的关注！本文档提供贡献代码的指南。

## 代码风格

- 遵循项目命名规范（C/C++ 命名以现有代码为准）
- 提交前用 clang-format 格式化代码（项目已提供 `.clang-format`）
- 运行 clang-tidy 静态分析（项目已提供 `.clang-tidy`）
- 合理使用 C++20 特性
- 性能关键代码优先使用模板而非虚继承
- 所有共享内存结构体必须使用 `DZ_DECLARE_ALIGNED_STRUCT`

## 提交信息

遵循 [约定式提交](https://www.conventionalcommits.org/) 格式：

```
<类型>: <中文描述>

<可选正文>
```

类型：`feat`（新功能）、`fix`（修复）、`refactor`（重构）、`docs`（文档）、`test`（测试）、`chore`（杂务）、`perf`（性能）、`ci`（持续集成）

示例：
- `feat: 添加 CTP 行情网关`
- `fix: 修复共享内存帧对齐问题`
- `refactor: 提取通用网关接口`

## Pull Request 流程

1. 从 `main` 创建功能分支
2. 编写代码并添加相应测试
3. 确保所有测试通过：`ctest --preset win-release`（或 `linux-release`）
4. 确保代码已格式化：`clang-format -i <文件>`
5. 如有需要，更新文档
6. 提交 PR，清晰描述变更内容

## 测试

- 单元测试使用 Google Test
- 每个库有自己的测试目录（`libs/<名称>/tests/`）
- 集成测试在 `tests/` 目录
- 新功能必须包含测试
- 目标：80%+ 代码覆盖率

## 架构决策

重大架构变更应记录为 ADR（架构决策记录），放在 `docs/adr/` 目录。

## 问题

如有问题或讨论，请提交 Issue。
