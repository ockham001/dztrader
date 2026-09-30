---
description: 用 clang-format 格式化 C++ 源码
agent: build
---

用项目提供的 `.clang-format` 格式化 C++ 源码。

**全量格式化（Windows / PowerShell）**

```powershell
Get-ChildItem -Path libs,apps -Recurse -Include *.h,*.cpp | clang-format -i
```

**全量格式化（Linux / Bash）**

```bash
find libs apps \( -name "*.h" -o -name "*.cpp" \) | xargs clang-format -i
```

**只格式化指定文件（两平台通用）**

```bash
clang-format -i <file1> <file2>
```

关于 `find` 的括号：不加括号时 GNU find 会把隐式 `-print` 套在整个 `-o` 表达式上，结果相同；但一旦以后有人给某个分支加 `-delete` / `-exec`，缺少括号就会变成陷阱。保持括号。

提交前应确认已格式化。若用户指定了文件（$ARGUMENTS），只格式化那些文件。请实际执行并报告结果。
