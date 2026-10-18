# MEIZU21-AERA_Recovery — AERA Recovery 移植工程（魅族 21 / m2461）

本仓库是 **AERA Recovery R1.0**（Android 16, `aera-16.0` manifest 链）为魅族 21
（m2461, SM8650）构建 `recovery.img` 的**自助（self-contained）工程**。它同时扮演三个角色：

1. **repo manifest**：根目录的 `default.xml → twrp-default.xml → aera.xml（+ remove-minimal.xml）`
   即 AERA 16.0 源码清单；
2. **AERA 三大源码树**（自合并，扁平化，无 .git）：
   - `android_bootable_recovery/` → 构建时覆盖 `bootable/recovery`
   - `android_external_lvgl/`    → 构建时覆盖 `external/lvgl`
   - `android_vendor_recovery/`  → 构建时覆盖 `vendor/recovery`
3. **设备树**：`device/meizu/m2461/`（含 `prebuilt/kernel` 出厂内核 + `recovery/root` 厂商 blob）。

其余 AERA 依赖（build/make、vendor/twrp、system/core 等约 25 个仓库）由 CI 直接从
`github.com/AERA-Recovery`、`LineageOS`、`TeamWin` 等公共源 sync。

## 构建（GitHub Actions，自动）

推送 `main`（或手动 `Run workflow`）→ 触发 `.github/workflows/build.yml`：

1. `repo init -u https://github.com/FengFaQ/MEIZU21-AERA_Recovery -b main`
2. `repo sync`（AOSP16 基座 + AERA/LineageOS 依赖，remove-minimal 裁剪）
3. 覆盖三大 AERA 源码树 + 安装设备树
4. `lunch twrp_m2461-bp2a-eng && mka adbd recoveryimage`
5. 产物：`recovery.img` + SHA-256（Run 页 Artifacts；成功时另发 prerelease）

## 本地构建（可选，需 ~60GB 磁盘 / 16GB+ RAM）

```bash
mkdir aosp && cd aosp
repo init --no-clone-bundle -u https://github.com/FengFaQ/MEIZU21-AERA_Recovery.git -b main
# 跳过已合并仓库（否则会从 AERA-Recovery 重复拉取）：
mkdir -p .repo/local_manifests
cat > .repo/local_manifests/00-meizu21.xml <<'XML'
<manifest>
  <remove-project name="android_bootable_recovery" />
  <remove-project name="android_external_lvgl" />
  <remove-project name="android_vendor_recovery" />
</manifest>
XML
repo sync -c --force-sync --no-tags -j$(nproc)
rm -rf bootable/recovery external/lvgl vendor/recovery device/meizu/m2461
cp -a <本仓库>/android_bootable_recovery bootable/recovery
cp -a <本仓库>/android_external_lvgl external/lvgl
cp -a <本仓库>/android_vendor_recovery vendor/recovery
mkdir -p device/meizu && cp -a <本仓库>/device/meizu/m2461 device/meizu/m2461
source build/envsetup.sh
lunch twrp_m2461-bp2a-eng
mka adbd recoveryimage
```

产物：`out/target/product/m2461/recovery.img`（目标分区 100 MiB）。

## 刷入

```
fastboot flash recovery_a recovery.img
fastboot flash recovery_b recovery.img
```

## 事实与文档

| 文档 | 内容 |
|---|---|
| `device/meizu/m2461/BLOBS.md` | 全部 prebuilt/kernel blob 出处与校验（源：adontoo/device_meizu_m2461-TWRP@twrp-16） |
| `device/meizu/m2461/M2461_INIT.md` | init rc 移植偏差记录（含 `install_keyring` 移除说明） |
| `docs/FACTS.md` | 设备分区/内核/加密/显示等核实事实 |
| `docs/BUILD.md` | 构建细节 |

## 状态

- 设备树、blobs、init、CI 已完成并提交；**尚未验证头一次编译**——首次 Run 应盯着日志修。
- `UNVERIFIED` 标记（镜像预算、状态栏高度、飞行模式构建号占位等）见各文件内注释。

## 致谢 / 许可

- 移植依据：`adontoo/device_meizu_m2461-TWRP`（已在真机验证的 TWRP，含显示/触摸/解密/刷写路径）
- AERA Recovery Project 及其上游 AOSP / TeamWin / OrangeFox / LineageOS
- 厂商二进制为魅族/高通专有，仅供个人设备内部构建输入，分发受其各自许可约束