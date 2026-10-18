// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0

package twrp

import (
	"android/soong/android"
	"android/soong/cc"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// AERA UI owns all visible recovery rendering. Only its runtime fallback fonts
// belong in /twres; the legacy XML page/theme bundle is deliberately omitted.
func copyAeraFonts(ctx android.BaseContext) bool {
	twRes := filepath.Join(ctx.Config().Getenv("OUT"), "recovery/root/twres")
	if err := os.RemoveAll(twRes); err != nil {
		fmt.Printf("Could not clear obsolete recovery UI resources: %v\n", err)
		return false
	}
	fontDest := filepath.Join(twRes, "fonts")
	if err := os.MkdirAll(fontDest, os.ModePerm); err != nil {
		fmt.Printf("Could not create AERA font directory: %v\n", err)
		return false
	}
	copyDir(getRecoveryAbsDir(ctx)+"aeraui/localization/fonts/assets/", fontDest)
	return true
}

func aeraRootManagerFlags(ctx android.BaseContext) []string {
	rootManager := "0"
	if getMakeVars(ctx, "AERA_ENABLE_ROOT_MANAGER") == "1" {
		rootManager = "1"
	}
	return []string{"-DAERA_ENABLE_ROOT_MANAGER=" + rootManager}
}

func globalFlags(ctx android.BaseContext) []string {
	cflags := aeraRootManagerFlags(ctx)
	if getMakeVars(ctx, "AB_OTA_UPDATER") == "true" {
		cflags = append(cflags, "-DAB_OTA_UPDATER=1")
	}
	adaptiveResolution := strings.Trim(getMakeVars(ctx, "AERA_UI_ADAPTIVE_RESOLUTION"), "\"")
	if adaptiveResolution == "1" || strings.EqualFold(adaptiveResolution, "true") {
		cflags = append(cflags, "-DAERA_UI_ADAPTIVE_RESOLUTION=1")
	}
	for _, metric := range []string{
		"AERA_SCREEN_H", "AERA_STATUS_H", "AERA_STATUS_INDENT_LEFT", "AERA_STATUS_INDENT_RIGHT",
	} {
		value := strings.Trim(getMakeVars(ctx, metric), "\"")
		if parsed, err := strconv.Atoi(value); err == nil && parsed > 0 {
			cflags = append(cflags, fmt.Sprintf("-D%s=%d", metric, parsed))
		}
	}
	cflags = append(cflags, aeraUiLanguageFlags(ctx)...)
	return cflags
}

func aeraUiLanguageFlags(ctx android.BaseContext) []string {
	var cflags []string
	defaultLanguage := strings.Trim(getMakeVars(ctx, "AERA_DEFAULT_LANGUAGE"), "\"")
	if defaultLanguage == "" {
		defaultLanguage = "en"
	}
	validLanguage := true
	for _, r := range defaultLanguage {
		if !(r == '_' || r == '-' || r >= 'a' && r <= 'z' ||
			r >= 'A' && r <= 'Z' || r >= '0' && r <= '9') {
			validLanguage = false
			break
		}
	}
	if !validLanguage {
		defaultLanguage = "en"
	}
	cflags = append(cflags, fmt.Sprintf("-DAERA_DEFAULT_LANGUAGE=\"%s\"", defaultLanguage))
	if getMakeVars(ctx, "AERA_EXTRA_LANGUAGES") == "true" {
		cflags = append(cflags, "-DAERA_EXTRA_LANGUAGES=1")
	}
	return cflags
}

func aeraHostDefaults(ctx android.LoadHookContext) {
	type props struct {
		Target struct {
			Android struct {
				Cflags  []string
				Enabled *bool
			}
		}
		Cflags       []string
		Include_dirs []string
	}
	p := &props{}
	p.Cflags = globalFlags(ctx)
	ctx.AppendProperties(p)
	legacyFlags := &props{}
	legacyFlags.Cflags = aeraGlobalFlags(ctx)
	ctx.AppendProperties(legacyFlags)
	if !copyAeraFonts(ctx) {
		os.Exit(-1)
	}
}

func init() {
	android.RegisterModuleType("aera_host_defaults", aeraHostDefaultsFactory)
	android.RegisterModuleType("aeraui_defaults", aeraUiDefaultsFactory)
}

func aeraHostDefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, aeraHostDefaults)
	return module
}

func aeraUiDefaults(ctx android.BaseContext) []string {
	return append(aeraUiLanguageFlags(ctx), aeraRootManagerFlags(ctx)...)
}

func aeraUiDefaultsFactory() android.Module {
	module := cc.DefaultsFactory()
	android.AddLoadHook(module, func(ctx android.LoadHookContext) {
		type props struct {
			Cflags []string
		}
		ctx.AppendProperties(&props{Cflags: aeraUiDefaults(ctx)})
	})
	return module
}
