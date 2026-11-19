# EasyCoding Deck 桌面预览核心

此目录提供组件预览和固定 WASM 重建使用的纯 C 核心，包括场景、游戏、绘图、集合、表单、导航、程序和音效语义。

- `pc/scripts/build-*-runtime.mjs` 从这里构建固定产品 WASM，不编译或执行组件包内的任意代码。
- 普通开发使用已提交的 WASM 与适配器，无需重建。
- 显式重建使用 LLVM/LLD 20.1.8，并保留既有内存、导出和权限边界。
- 内置资源元数据在 `pc/builtin-catalog.json`，静态契约夹具在 `pc/tests/contracts/`。

这里不包含板级驱动、外设初始化或设备固件构建入口。源码中的接口名与类型是现有运行时契约的一部分，修改时须同步对应适配器、生成工具和测试。

保留文件内的来源标识与版权说明；项目许可及第三方声明见根目录 `LICENSE` 与 `THIRD_PARTY_NOTICES.md`。
