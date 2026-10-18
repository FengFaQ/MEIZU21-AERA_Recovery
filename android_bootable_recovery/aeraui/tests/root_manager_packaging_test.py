#!/usr/bin/env python3
"""Exercise actual packaging rules in temporary fixtures, never Android out/."""
import os
import pathlib
import re
import subprocess
import tempfile
import unittest

RECOVERY = pathlib.Path(__file__).resolve().parents[2]
VENDOR = RECOVERY.parents[1] / 'vendor/recovery'


class RootPackagingTest(unittest.TestCase):
    def test_soong_ui_and_host_receive_the_same_feature_flag(self):
        source = (RECOVERY / 'aeraui/build/aeraui_defaults.go').read_text()
        for hook in ('aeraUiDefaults', 'globalFlags'):
            body = re.search(r'func ' + hook + r'\([^\n]+\{\n(.*?)\n\}', source, re.S)
            self.assertIsNotNone(body, hook)
            self.assertIn('aeraRootManagerFlags(ctx)', body.group(1), hook)
        self.assertIn('getMakeVars(ctx, "AERA_ENABLE_ROOT_MANAGER") == "1"', source)

    def test_android_kati_configuration_and_vendor_argument(self):
        kati = RECOVERY.parents[1] / 'prebuilts/build-tools/linux-x86/bin/ckati'
        if not kati.is_file():
            self.skipTest('Android Kati prebuilt is not available')
        source = (RECOVERY / 'aera_build.mk').read_text()
        bridge = source.split('LOCAL_CFLAGS += -DAERA_ENABLE_ROOT_MANAGER=', 1)[1]
        bridge = 'LOCAL_CFLAGS += -DAERA_ENABLE_ROOT_MANAGER=' + bridge.split('# mask read errors', 1)[0]
        with tempfile.TemporaryDirectory(prefix='aera-root-kati-') as tmp:
            makefile = pathlib.Path(tmp) / 'fixture.mk'
            makefile.write_text('$(KATI_obsolete_export Exports are prohibited)\ninclude ' +
                                str(RECOVERY / 'aera_config.mk') + '\nAERA_VENDOR := vendor-hook.sh\n' +
                                bridge + '\n.PHONY: all\nall:\n\t@printf "%s\\n" $(AERA_VENDOR)\n')
            environment = os.environ.copy()
            environment.pop('AERA_ENABLE_ROOT_MANAGER', None)
            for flag in ('1', '0', None):
                command = [str(kati), '-f', str(makefile), 'all']
                if flag is not None:
                    command.append('AERA_ENABLE_ROOT_MANAGER=' + flag)
                output = subprocess.check_output(command, env=environment, text=True)
                self.assertIn('AERA_ENABLE_ROOT_MANAGER=' + ('1' if flag == '1' else '0'), output)

    def test_incremental_provider_enable_disable_enable(self):
        source = (RECOVERY / 'aera_build.mk').read_text()
        rules = source.split('# Root Manager is one optional feature;', 1)[1]
        rules = '# Root Manager is one optional feature;' + rules.split('# AERA Remote', 1)[0]
        with tempfile.TemporaryDirectory(prefix='aera-root-packaging-') as tmp:
            root = pathlib.Path(tmp)
            assets = root / 'source/prebuilt/aera/root'
            assets.mkdir(parents=True)
            for provider in ('kernelsu', 'next', 'sukisu'):
                (assets / (provider + '.ko')).write_bytes(provider.encode())
            makefile = root / 'fixture.mk'
            makefile.write_text('include ' + str(RECOVERY / 'aera_config.mk') + '\n' +
                                rules + '\n.PHONY: all\nall: $(AERA_ROOT_ASSETS_STAMP)\n')
            environment = os.environ.copy()
            environment.pop('AERA_ENABLE_ROOT_MANAGER', None)
            for flag in ('1', '0', '1', None):
                command = ['make', '--no-print-directory', '-f', str(makefile), 'all',
                           'TARGET_ARCH=arm64', 'commands_TWRP_local_path=' + str(root / 'source'),
                           'TARGET_RECOVERY_ROOT_OUT=' + str(root / 'ramdisk')]
                if flag is not None:
                    command.append('AERA_ENABLE_ROOT_MANAGER=' + flag)
                subprocess.run(command, env=environment, check=True, stdout=subprocess.PIPE)
                output = root / 'ramdisk/system/etc/aera'
                providers = output / 'root-providers'
                self.assertEqual(providers.is_dir(), flag == '1')
                if flag == '1':
                    self.assertEqual(sorted(p.name for p in providers.iterdir()),
                                     ['kernelsu.ko', 'next.ko', 'sukisu.ko'])
                stamps = list(output.glob('.root-providers*.stamp'))
                self.assertEqual(len(stamps), 1)
                self.assertEqual(stamps[0].name, '.root-providers-' +
                                 ('enabled' if flag == '1' else 'disabled') + '.stamp')

    def test_ksud_and_obsolete_addons(self):
        source = (VENDOR / 'AERA_A16.sh').read_text()
        block = source.split('  # Clear obsolete installer addons', 1)[1]
        block = '  # Clear obsolete installer addons' + block.split('  # enable the app manager?', 1)[0]
        with tempfile.TemporaryDirectory(prefix='aera-ksud-packaging-') as tmp:
            root = pathlib.Path(tmp)
            binary = root / 'vendor/prebuilt/arm64/ksud'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'fixture ksud')
            ramdisk = root / 'ramdisk'
            (ramdisk / 'sbin').mkdir(parents=True)
            environment = os.environ.copy()
            environment.update(AERA_RAMDISK=str(ramdisk), AERA_VENDOR_PATH=str(root / 'vendor'),
                               TARGET_ARCH='arm64', RAMDISK_SBIN='sbin', CP='/bin/cp', GREEN='', NC='')
            for flag in ('1', '0', '1', None):
                if flag is None:
                    environment.pop('AERA_ENABLE_ROOT_MANAGER', None)
                else:
                    environment['AERA_ENABLE_ROOT_MANAGER'] = flag
                legacy = ramdisk / 'AERA/Files/KernelSU'
                legacy.mkdir(parents=True)
                (legacy / 'old-installer.zip').write_bytes(b'obsolete')
                subprocess.run(['bash', '-e', '-c', block], env=environment, check=True,
                               stdout=subprocess.PIPE)
                self.assertFalse(legacy.exists())
                ksud = ramdisk / 'sbin/ksud'
                self.assertEqual(ksud.exists(), flag == '1')
                if flag == '1':
                    self.assertEqual(ksud.read_bytes(), binary.read_bytes())
                    self.assertTrue(os.access(ksud, os.X_OK))


if __name__ == '__main__':
    unittest.main()
