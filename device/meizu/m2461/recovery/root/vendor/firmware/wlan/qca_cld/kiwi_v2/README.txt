This directory is intentionally empty in the repository.

The stock Meizu 21 vendor image ships two Wi-Fi files here as symlinks:

    WCNSS_qcom_cfg.ini -> /vendor/etc/wifi/kiwi_v2/WCNSS_qcom_cfg.ini
    wlan_mac.bin       -> /mnt/vendor/persist/kiwi_v2/wlan_mac.bin

They are recreated by the "Overlay consolidated AERA sources and install device
tree" step of .github/workflows/build.yml, because a Windows checkout cannot
carry symlinks (core.symlinks=false) and `git add` refuses to index them.

The real config file lives at vendor/etc/wifi/kiwi_v2/WCNSS_qcom_cfg.ini in
this tree, and the MAC address is read from the persist partition, which
recovery.fstab now mounts at /mnt/vendor/persist.
