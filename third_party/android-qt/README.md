# Android Qt 运行库

Android 预览版使用 Qt 5.15.2 的 Android 动态库及 AndroidExtras；Windows 桌面工具链保持 Qt 5.12.12。未更换 Qt5 / C++ 主框架。

- [官方 5.15.2 源码](https://download.qt.io/archive/qt/5.15/5.15.2/submodules/)
- [QtBase 固定源码](https://code.qt.io/cgit/qt/qtbase.git/tree/?h=v5.15.2)
- [QtMultimedia 固定源码](https://code.qt.io/cgit/qt/qtmultimedia.git/tree/?h=v5.15.2)
- [QtAndroidExtras 固定源码](https://code.qt.io/cgit/qt/qtandroidextras.git/tree/?h=v5.15.2)

`LICENSE.LGPL3`、`LICENSE.GPL3` 为 QtBase 该版本原始许可文件，保留原文。开发工具和二进制放在 Git 忽略的 `build/tools/android/qt/`，APK 内保留动态链接，源码及构建脚本在本仓库公开。此旧版 Qt 用于本轮安卓移植预览，完成编译不能代替新 Android 系统、16 KB 页设备、手机及 PICO 运行验收。
