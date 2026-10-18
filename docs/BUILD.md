# BUILD.md — 构建细节

## 触发方式

- 自动：推送 `main` 分支且变更涉及 `device/**` 或 `.github/workflows/**`；
- 手动：仓库 Actions 页 → `AERA Recovery for Meizu 21 (m2461)` → **Run workflow**。

## CI 步骤（.github/workflows/build.yml）

1. `maximize-build-space`（释放 Runner 磁盘，加 8G swap）；
2. 安装 `git/python3/repo`（repo 取 storage.googleapis.com 官方 bootstrap）；
3. `actions/checkout` 拉取本仓库（= manifest + 三大 AERA 源码树 + 设备树）；
4. `repo init -u https://github.com/FengFaQ/MEIZU21-AERA_Recovery.git -b main`；
5. 写入 local manifest：`remove-project` 三个已合并仓库
   （`android_bootable_recovery` / `android_external_lvgl` / `android_vendor_recovery`）；
6. `repo sync -c --force-sync --no-tags -j$(nproc)`（失败自动重试 3 次；
   android.googlesource.com 偶发 429）；
7. 覆盖：`bootable/recovery ← android_bootable_recovery`，
   `external/lvgl ← android_external_lvgl`，`vendor/recovery ← android_vendor_recovery`，
   并安装 `device/meizu/m2461`；
8. `source build/envsetup.sh && lunch twrp_m2461-bp2a-eng`
9. `mka adbd recoveryimage` → `out/target/product/m2461/recovery.img`；
10. 成功 → 上传 Artifact + 打 prerelease；失败 → 上传 build-log 便于排查。

## 下载产物

- Actions 运行页 → **Artifacts** → `recovery-image-<run>`（`recovery.img` + sha256）；
- 或 Release 页（AERA-m2461-<run>，prerelease）。

## 已知风险（首次编译需盯）

| 风险 | 说明 |
|---|---|
| 镜像 ≤ 100 MiB | 目标 recovery 分区 104857600 B；AERA 内置 lvgl 运行时偏大，首次构建后需核对大小 |
| googlesource 限流 | repo sync 重试处理；仍失败时考虑镜像源 |
| `install_keyring` 移除 | AERA init 无此 builtin（已从 rc 移除），解密逻辑需真机复测 |
| 状态栏高度等 UI 参数 | `AERA_STATUS_H/INDENT_*` 为未验证初值，真机上再调 |