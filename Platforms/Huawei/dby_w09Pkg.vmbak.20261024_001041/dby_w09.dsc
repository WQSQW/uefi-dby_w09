##
#  DBY-W09 —— Huawei MatePad 11 (SM8250 / kona, 1600x2560, 6GiB)
#
#  本包由 Mu-Silicium(sheng 分支) 的 Platform/Lenovo/9707fPkg 派生。
#  9707f（联想 Legion Tab Y700）是 SM8250 + 1600x2560 的现成模板，与 DBY-W09
#  面板几何完全一致，故只改必要的四类东西：
#    1. 身份   PLATFORM_NAME / GUID / OUTPUT_DIRECTORY / SMBIOS 厂商型号
#    2. 内存   MemoryMapLib 换成 DBY-W09 实测布局（UEFI FD 0xCE000000）
#    3. ACPI   指向 Silicium-ACPI/Platforms/Huawei/dby_w09
#    4. SoC    SOC_TYPE 0 = SM8250(865)，9707f 是 2 = SM8250-AC(870)
#
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = dby_w09
  PLATFORM_GUID                  = D3A7F60C-58B2-4E19-8C4F-1B7A2D9E3C58
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/dby_w09Pkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = dby_w09Pkg/dby_w09.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8250
  # 1 = SM8250-AB
  # 2 = SM8250-AC
  #
  SOC_TYPE                       = 0

!include KonaPkg/KonaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"HUAWEI"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"DBY-W09"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"dby_w09"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"DBY-W09"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"DBY-W09"

  #
  # Simple Frame Buffer
  #   物理真值，**禁止改动**：帧缓冲是 ABL 点亮 DSI 后交下来的 1600x2560，
  #   stride = 1600*4 = 6400B。对调成 2560x1600 会让 FrameBufferBltLib 用
  #   10240B 行跨度写只有 6400B 的缓冲 → 每行漂移 3840B → 满屏重复竖条。
  #   横屏（90°）方案 2026-10-22 真机失败：Blt 出口映射写错，菜单变成
  #   蓝底块状乱码。已全部撤回，驱动恢复上游原版，物理/逻辑几何一致。
  #   归档版在 pkg_dby_w09/Overrides/_disabled/SimpleFb_rot90.c.txt，未启用。
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1600
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2560
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

  #
  # UniXNU 日志等级：0x40 -> 0
  #
  # 改在 UniXNUPkg.dec 里（该 PCD 是 [PcdsFixedAtBuild]，dec 的默认值就是
  # 平台默认值，无需在本 dsc 重复声明包与 PCD）。
  # 原因见 UniXNUPkg.dec 那段注释：本机 DebugLib 的输出不走 SimpleFbDxe 的
  # GOP，因此不经过 RotateFlush，会和旋转后的菜单抢同一块物理帧缓冲，
  # 横拿看就是"图形正了、代码竖着往下跑"。
  #

#
# 文本控制台几何
#
# 坑：MdeModulePkg.dec:1953/1958 的默认值是 PcdConOutRow|25 / PcdConOutColumn|80。
# GraphicsConsoleControllerDriverStart() 命中"用户指定 80x25"分支后，按
#   DeltaX = (水平分辨率 - 80*8)  >> 1
#   DeltaY = (垂直分辨率 - 25*19) >> 1
# 居中摆放 ⇒ 屏上只有正中间一小块（1600 宽下左右各留 640px 黑边）。
# 这就是"Boot Manager 怎么这么小"的原因，App 本身没问题（就是上游原版）。
#
# 两个 PCD 置 0 后走 else 分支"取最大列/行"，控制台铺满：
#   1600x2560 → 200 列 x 160 行（8x16 字形正好铺满，DeltaX/DeltaY = 0）
#
# ★ 但只设上面两个不够！官方 BootManagerMenuApp 起来时会调
#   SetVideoMode(mSetupTextModeColumn, mSetupTextModeRow)，而这两个值取自
#   PcdSetupConOutColumn / PcdSetupConOutRow（BootManagerMenu.c:1085-1086），
#   默认 80 / 25 —— 它会把已经铺满的控制台再往 80x25 切。
#   更糟的是 GraphicsConsoleDxe 的模式表（GraphicsConsole.c:41-52）里
#   根本没有 80x25 这一项（最小是 100x31），切模式必然 EFI_UNSUPPORTED，
#   而 App 那边是 ASSERT_EFI_ERROR(Status) —— DEBUG 构建下 ASSERT 直接
#   死循环。所以这两个必须一起钉成满屏值 200 / 160。
#
# 200 / 160 的来历（不是拍的）：GraphicsConsole.c:239-251
#   MaxColumns = 1600 / 8  = 200
#   MaxRows    = 2560 / 16 = 160
#   并把 {200,160} 填成模式表最后一项（full screen mode）。
#   分辨率一变这两个数就得跟着变，所以写死在这里并在 install 里核对。
#
# 注：这 4 个 PCD 在 MdeModulePkg.dec:1949 的段头是
#     [PcdsPatchableInModule, PcdsDynamic, PcdsDynamicEx]，没有 FixedAtBuild。
#     这里选 PcdsDynamicDefault 而不是 PatchableInModule，原因是 FV 里的
#     BootManagerMenuApp.c:895-898 有 PcdSet32S(PcdConOutColumn/Row, ...)，
#     PatchableInModule 的 PCD 不允许运行时 Set，会直接把编译打挂。
#     （视频分辨率那两个已由 SiliciumPkg.dsc.inc:108-109 置 0，这里不重复。）
#
#
# ★ 2026-10-22 第二轮：显示改走 90° 横屏覆盖（SimpleFbDxe overrides），
#   GOP 报给上层的逻辑画布变成 2560x1600（面板物理仍是 1600x2560，stride 6400B）。
#   所以这里不再写死 200 / 160 —— 那是 1600x2560 竖屏的满屏值，横过来以后
#   字符格子就不对了（这正是上一版横屏"蓝底块状乱码"的嫌疑点之一）。
#
#   4 个 PCD 全部置 0，让 GraphicsConsoleDxe 走 else 分支自己按当前分辨率取
#   "最大列 / 最大行"：竖屏 1600x2560 -> 200x160（实测过），横屏 2560x1600 -> 320x100。
#   分辨率以后再变，这里也不用跟着改。
#
#   之前"必须钉死 200/160"的唯一理由是官方 BootManagerMenuApp 会
#   SetVideoMode(PcdSetupConOutColumn, PcdSetupConOutRow)，把铺满的控制台再往
#   80x25 切（GraphicsConsole 模式表里没这项 -> ASSERT）。现在我们用的是
#   UniXNU（纯 GOP Blt 绘制，不碰 ConOut，也不切模式），这个约束已经不成立。
#
# 注：这 4 个 PCD 在 MdeModulePkg.dec:1949 的段头是
#     [PcdsPatchableInModule, PcdsDynamic, PcdsDynamicEx]，没有 FixedAtBuild。
#     这里选 PcdsDynamicDefault 而不是 PatchableInModule，原因是 FV 里的
#     BootManagerMenuApp.c:895-898 有 PcdSet32S(PcdConOutColumn/Row, ...)，
#     PatchableInModule 的 PCD 不允许运行时 Set，会直接把编译打挂。
#     （视频分辨率那两个已由 SiliciumPkg.dsc.inc:108-109 置 0，这里不重复。）
#
[PcdsDynamicDefault]
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|0

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|dby_w09Pkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # 平台启动管理
  #   覆盖 SiliciumPkg 版：UnableToBoot 时进 Boot Manager 菜单，
  #   不再 DisplayBootGraphic + Stall + ResetSystem(Shutdown)
  #   （本机 Shutdown 走不通 → 被 ABL/看门狗变成重启循环）
  #
  PlatformBootManagerLib|dby_w09Pkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|dby_w09Pkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

  #
  # UniXNU 官方启动菜单
  #   源码是上游 UniXNU/package/UniXNUPkg 原样（install_dby_w09.sh 的 1.7/5 铺到
  #   $UEFI/UniXNUPkg）。它只用 MdePkg + BaseFdtLib，FdtLib 已在
  #   SiliciumPkg.dsc.inc:322 映射好，这里只补它自己的库类。
  #
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf

  #
  # 按键（实体键 -> SimpleTextIn）
  #   SiliciumPkg 只提供 KeypadDevice 协议的生产者(KeypadDeviceDxe)和
  #   消费者(KeypadDxe)，平台必须给一份 KeypadDeviceLib 实现。
  #   本机实现：volume+ = PM8150 GPIO6、power = PON_KPDPWR、volume- = PON_RESIN。
  #   PlatformBootManagerLib 早就把 KeypadDevicePath 写进 ConIn 白名单，
  #   所以这一行存在 == 三个实体键能当方向键/回车用。
  #
  KeypadDeviceLib|dby_w09Pkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

[Components]
  #
  # ACPI Tables
  #
  dby_w09/AcpiTables.inf

  #
  # Boot Manager 菜单
  #
  # (a) UniXNU 官方菜单 —— 屏幕上的那个"苹果 logo + UniXNU version 0.1.0 +
  #     Return to firmware / Restart / Power off"界面，就是它。FILE_GUID
  #     9B665204-A133-47A2-9396-9E2B370A4E25，由
  #     PlatformBootManagerLib/BootManager.c 的 DbyW09StartUniXNU() 直接从 FV
  #     读出来 LoadImage + StartImage。
  #
  # (b) 不要再在这里列 MdeModulePkg/Application/BootManagerMenuApp/BootManagerMenuApp.inf：
  #     KonaPkg 的公共 inc（DXE.inc / QcomPkg Extra.fdf.inc）已经把它的 FFS 放进 FvMain 了。
  #     再列一次会让 GenFv 直接失败：
  #       GenFv: ERROR 2000: Invalid parameter
  #       the 93th file and 97th file have the same file GUID.
  #       eec25bdc-67f2-4d95-b1d5-f81b2039d11d
  #     它现在只作为 UniXNU 的兜底（FV 里找不到 9B665204 时才用）。
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf

  #
  # 按键：KeypadDeviceDxe 装 gKeypadDeviceProtocolGuid + KeypadDevicePath，
  #       KeypadDxe 绑上去装 SimpleTextIn/SimpleTextInEx。
  #       两个都必须在 FV 里，缺一个 ConIn 就是空的（现在就是这状态）。
  #
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
