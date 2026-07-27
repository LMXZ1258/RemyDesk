# 参与贡献

1. 从 `main` 创建功能分支。
2. 修改后至少执行：

   ```bash
   cmake -S . -B build -DBUILD_TESTING=ON
   cmake --build build
   ctest --test-dir build --output-on-failure
   (cd native/webrtc-publisher && go test ./...)
   bash -n scripts/*.sh native/desktop-streamer/*.sh
   ```

3. 涉及 DRM/RGA/MPP 的修改必须说明测试板卡、系统镜像、内核、RGA/MPP版本和实际后端。
4. 不提交构建产物、私钥、真实 Wi-Fi 密码、设备日志中的个人数据或厂商不允许再分发的二进制。
5. 新板卡优先增加 `profiles/<name>/`，避免在源码中按产品名堆叠条件分支。

