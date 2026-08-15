# UI System & Trading Interfaces

## UI System

- **Qt 后端**：单端口，广播数据，前端根据权限 show/hide
- **Web UI 后端**：Vue 3 + TypeScript + Vite，单端口，后端严格过滤权限（不可见数据不发送）
- **用户管理**：统一用户数据，认证、IP 限制、黑白名单、安全模式（白名单/自动/不限制）
- **权限控制**：账户可见性、策略可见性、操作权限，管理员实时修改
- 功能：登录、行情展示、行情源管理、策略管理、权限过滤、账户管理、硬件监控、NTP 监控/同步、日志查询分析

## Trading Interfaces (Planned)

按优先级开发：CTP → 融航 → 其他

完整列表：CTP、CTP测试、CTP Mini、飞马、CTP期权、顶点飞创、顶点HTS、恒生UFT、易盛、中泰XTP、国泰君安统一交易网关、华鑫奇点股票、华鑫奇点期权、中亿汇达Comstar、东方证券OST、盈透证券、易盛9.0外盘、直达期货、融航、TTS、飞鼠、金仕达黄金
