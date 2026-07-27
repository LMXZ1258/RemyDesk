# 板卡配置档案

配置档案只描述默认能力和参数，不携带内核、DTB、RGA/MPP动态库等厂商文件。

- `generic-rk3588`：所有未知 RK3588/RK3588S 板卡的保守默认值。
- `orangepi-5-plus`：Orange Pi 5 Plus 已验证基线。
- `firefly-rk3588`：Firefly RK3588 系列的保守基线，安装后仍需运行 doctor 验证。

自动检测只选择默认配置，不代表硬件链路已经通过。安装后必须执行：

```bash
sudo /opt/remydesk/libexec/remydesk-doctor.sh --video
```

增加新板卡时复制 `generic-rk3588`，只覆盖确有差异的变量，并在 `profile.conf` 中记录验证状态。

