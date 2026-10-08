# Android HTTPS 运行库

Qt 5.15.2 的 Android HTTPS 后端需要 OpenSSL 1.1 ABI。预览包固定使用 [KDAB android_openssl](https://github.com/KDAB/android_openssl/tree/b71f1470962019bd89534a2919f5925f93bc5779) 中 `ssl_1.1/` 的 ARM64 与 x86_64 动态库，下载后校验每个文件的 SHA256。未修改系统 OpenSSL。

- [OpenSSL 1.1.1w 对应源码](https://github.com/openssl/openssl/tree/OpenSSL_1_1_1w)
- [KDAB 构建说明](https://github.com/KDAB/android_openssl/blob/b71f1470962019bd89534a2919f5925f93bc5779/README.md)
- `LICENSE` 为 OpenSSL 1.1.1w 原始许可，条款保持原文。

OpenSSL 1.1.1 已结束公开维护，此依赖随旧版 Qt 用于本地预览验证；正式 Android 发布前须迁移到有维护的 Qt5/HTTPS 后端并重新验证，不能将预览包称为正式 Android 发布。
