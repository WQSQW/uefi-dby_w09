  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf

  INF Binaries/dby09/QcomPkg/Drivers/TzDxe/ScmDxeLA.inf
  INF Binaries/dby09/QcomPkg/Drivers/TzDxe/TzDxeLA.inf

  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf

  INF Binaries/dby09/QcomPkg/Drivers/EnvDxe/EnvDxe.inf

  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF Binaries/dby09/QcomPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/ULogDxe/ULogDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/dby09/QcomPkg/Drivers/CPRDxe/CPRDxe.inf
!endif
  INF Binaries/dby09/QcomPkg/Drivers/PdcDxe/PdcDxe.inf

  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF FatPkg/EnhancedFatDxe/Fat.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  INF Binaries/dby09/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/VcsDxe/VcsDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/HALIOMMUDxe/HALIOMMUDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/GpiDxe/GpiDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/I2CDxe/I2CDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/SPIDxe/SPIDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/SPMIDxe/SPMIDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/SdccDxe/SdccDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/UFSDxe/UFSDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/TLMMDxe/TLMMDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/FeatureEnablerDxe/FeatureEnablerDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF Binaries/dby09/QcomPkg/Drivers/DisplayDxe/DisplayDxe.inf
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

  INF Binaries/dby09/QcomPkg/Drivers/PlatformInfoDxe/PlatformInfoDxe.inf
  #
  # 2026-10-22：停用 Qcom 的 ButtonsDxe。
  #   ① 它的 depex 要的是 PMIC 私有协议（157A5C45-…/4BC5C6D7-…/66024A5D-…），
  #      不是开源 EFIPmicPon.h 里那个 97044B58-…，本机能不能满足全看运气；
  #   ② 本包里那份 PE32 是上游为联想 Y700 打的补丁版
  #      （ButtonsDxe.patched.efi，与原件差 3 字节：movz 立即数 + strh 偏移），
  #      按键接的 PMIC GPIO 号跟 DBY-W09 不一样 —— 真跑起来读到的是别的引脚，
  #      正好能解释之前"菜单按钮自己会动"；
  #   ③ 现在改由 SiliciumPkg 的 KeypadDeviceDxe + KeypadDxe + 平台 KeypadDeviceLib
  #      （dby09Pkg/Library/KeypadDeviceLib）走正规 KeypadDevicePath 通道，
  #      BDS 的 ConIn 白名单指的就是这条路径。
  #   要回退：把下面这行的注释去掉即可。
  #
  #INF Binaries/dby09/QcomPkg/Drivers/ButtonsDxe/ButtonsDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/PmicDxe/PmicDxeLa.inf
  INF Binaries/dby09/QcomPkg/Drivers/ChargerExDxe/ChargerExDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/QcomChargerDxe/QcomChargerDxeLA.inf
  INF Binaries/dby09/QcomPkg/Drivers/UsbPwrCtrlDxe/UsbPwrCtrlDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/AdcDxe/AdcDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/TsensDxe/TsensDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/LimitsDxe/LimitsDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/DDRInfoDxe/DDRInfoDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/XhciPciEmulationDxe/XhciPciEmulationDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/XhciDxe/XhciDxe.inf

  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

  INF Binaries/dby09/QcomPkg/Drivers/UsbMsdDxe/UsbMsdDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/UsbDeviceDxe/UsbDeviceDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/UsbConfigDxe/UsbConfigDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/UsbInitDxe/UsbInitDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/HashDxe/HashDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/CipherDxe/CipherDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/MacDxe/MacDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/RNGDxe/RngDxe.inf

  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

  INF Binaries/dby09/QcomPkg/Drivers/PILDxe/PILDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/PILProxyDxe/PILProxyDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/SPSSDxe/SPSSDxe.inf
  INF Binaries/dby09/QcomPkg/Drivers/ShmBridgeDxe/ShmBridgeDxeLA.inf
