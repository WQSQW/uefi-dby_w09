/**
  Copyright (C) DuoWoA authors. All rights reserved.
  Copyright (C) Microsoft Corporation.

  SPDX-License-Identifier: BSD-2-Clause-Patent

  DBY09 平台覆盖版 PlatformBootManagerLib
  ------------------------------------------------------------------
  与 SiliciumPkg/Library/BootManagerLib 的差异只有一处：
  PlatformBootManagerUnableToBoot()。

  原实现（SiliciumPkg 版本 BootManager.c:202-224）在找不到任何可启动目标时：
      DisplayBootGraphic (BG_NO_BOOT_OS);       // 三角 + "Failed to find an Operating System"
      gBS->Stall (10000000);                    // 10 s
      gRT->ResetSystem (EfiResetShutdown, ...); // 关机
      CpuDeadLoop ();
  在 DBY-W09 上 Shutdown 走不通（ABL/看门狗把它变成重启），实测表现为
  "反复重启 → 最后停在异常转储屏"。而且本机没有任何输入设备
  （DXE.inc 里只有 Qcom 的 ButtonsDxe，没有 KeypadDeviceDxe + KeypadDeviceLib，
    所以 KeyCallback.c::SetupKeypad() 的 LocateDevicePath 必然 Not Found，
    EnterBootManager() 恒为 FALSE，音量键进 Boot Manager 这条路径是断的）。

  因此这里改为：
      打印可启动项清单 + 探测各 SimpleFileSystem 上有没有 BOOTAA64.EFI
      → 逐秒倒计时 5 s（留一张原始日志的照片）
      → 起 FV 里的 UniXNU.efi（官方启动菜单，见 Dby09StartUniXNU）
      → 菜单返回后重新进菜单（不 ResetSystem，杜绝重启循环）
**/

#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/AcpiTableUpdateLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SecureBootHandlerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MsBootOptionsLib.h>
#include <Library/BootGraphicsLib.h>
#include <Library/BootGraphics.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareVolume2.h>
// EFI_SECTION_PE32 / EFI_SECTION_TE —— 取 PE 段要用
#include <Pi/PiFirmwareFile.h>

// EFI_FILE_PROTOCOL / EFI_FILE_MODE_READ 都在 SimpleFileSystem.h 里（MdePkg 没有 Protocol/FileIo.h）
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextInEx.h>
#include <Protocol/GraphicsOutput.h>
// EFI_WHITE / EFI_BACKGROUND_BLUE / EFI_LIGHTGRAY … 都在 SimpleTextOut.h 里（全屏菜单要用）
#include <Protocol/SimpleTextOut.h>
#include <Protocol/EfiKeypadDevice.h>
#include <Library/DevicePathLib.h>

#include <Configuration/BootDevices.h>

#include "BootManager.h"

//
// Global Variables
//
STATIC UINTN XPos = 0;
STATIC UINTN YPos = 0;

//
// BDS Combo Message
//
STATIC CONST CHAR8 *ComboMessage = "[Volume Up] Boot Manager";

VOID
EFIAPI
PlatformBootManagerBeforeConsole ()
{
  // Signal End Of DXE
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  // Dispatch Deferred Images
  EfiBootManagerDispatchDeferredImages ();

  // Register Boot Options
  MsBootOptionsLibRegisterDefaultBootOptions ();

  // Register Console Variables
  EfiBootManagerUpdateConsoleVariable (ConIn,  (EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath,  NULL);
  EfiBootManagerUpdateConsoleVariable (ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);

  // Execute Secondary Before Console
  DeviceBootManagerBeforeConsole (NULL, NULL);
}

VOID
PostGopSetup (OUT EFI_GRAPHICS_OUTPUT_PROTOCOL **GopProtocol)
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *LocalGopProtocol;

  // Verify Parameters
  if (GopProtocol == NULL) {
    return;
  }

  // Locate GOP Protocol
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID *)&LocalGopProtocol);
  if (EFI_ERROR (Status) && Status != EFI_UNSUPPORTED) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol from Handle! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  // Check Status
  if (Status == EFI_UNSUPPORTED) {
    // Locate GOP Protocol
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID *)&LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol! Status = %r\n", __FUNCTION__, Status));
      return;
    }

    // Install GOP Protocol
    Status = gBS->InstallProtocolInterface (&gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, EFI_NATIVE_INTERFACE, LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Install GOP Protocol onto \"ConsoleOutHandle\"! Status = %r\n", __FUNCTION__, Status));
    }
  }

  // Pass GOP Protocol
  *GopProtocol = LocalGopProtocol;
}

//
// DBY09 追加：把已存在的输入设备补进 ConIn 白名单
//
// 源码依据：MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatform.c:261-297
//     ConPlatformUpdateDeviceVariable (L"ConIn", DevicePath, Check)
//   只有返回成功，才给该 handle 装 gEfiConsoleInDeviceGuid；否则 CloseProtocol 直接放弃。
//   ⇒ ConIn 变量是"白名单"，不在名单里的 STI/STIEx 设备（含 USB 键盘）永远成不了 ConIn。
//
// 而 PlatformBootManagerBeforeConsole() 把 ConIn 只设成了 KeypadDevicePath；
// 本机没有 KeypadDeviceDxe，那条路径不存在 ⇒ 白名单里一条真设备都没有。
// 这里在 Console 之后补一遍：ConnectAll → 把已存在的 STIEx 设备路径追加进 ConIn
// → 重新 ConnectController 让 ConPlatform 重装 Guid。
//
STATIC
VOID
Dby09FixupConsoleIn (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE               *Handles;
  UINTN                     Count;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;

  EfiBootManagerConnectAll ();

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextInputExProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    DEBUG ((EFI_D_ERROR, "DBY09: fixup ConIn: no STIEx handle (%r)\n", Status));
    return;
  }

  DEBUG ((EFI_D_ERROR, "DBY09: fixup ConIn: %d STIEx handle(s)\n", Count));

  for (UINTN i = 0; i < Count; i++) {
    DevicePath = NULL;
    Status     = gBS->HandleProtocol (Handles[i], &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath);
    if (EFI_ERROR (Status) || (DevicePath == NULL)) {
      continue;
    }

    EfiBootManagerUpdateConsoleVariable (ConIn, DevicePath, NULL);
    gBS->ConnectController (Handles[i], NULL, NULL, TRUE);
  }

  FreePool (Handles);
}

VOID
EFIAPI
PlatformBootManagerAfterConsole ()
{
  EFI_STATUS                    Status      = EFI_SUCCESS;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *GopProtocol = NULL;

  // 先补 ConIn 白名单，再谈别的
  Dby09FixupConsoleIn ();

  // Post Setup GOP
  PostGopSetup (&GopProtocol);


  // Display Boot Logo
  Status = DisplayBootGraphic (BG_SYSTEM_LOGO);

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display Boot Logo! Status = %r\n", __FUNCTION__, Status));
  }

  // Register Key Callback
  Status = RegisterKeyCallback ((EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath);
  if (!EFI_ERROR (Status) && GopProtocol != NULL) {
    // Set Screen Resolution
    UINT32 ScreenWidth  = GopProtocol->Mode->Info->HorizontalResolution;
    UINT32 ScreenHeight = GopProtocol->Mode->Info->VerticalResolution;

    // Set Combo Message Position
    XPos = (ScreenWidth - AsciiStrLen (ComboMessage) * EFI_GLYPH_WIDTH) / 2;
    YPos = ScreenHeight * 48 / 50;
  }

  // Execute Secondary After Console
  DeviceBootManagerAfterConsole ();
}

VOID
EFIAPI
PlatformBootManagerOnDemandConInConnect ()
{
  // Execute Secondary On Demand
  DeviceBootManagerOnDemandConInConnect ();
}

VOID
EFIAPI
PlatformBootManagerProcessBootCompletion (OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption)
{
  return;
}

VOID
EFIAPI
PlatformBootManagerPriorityBoot (IN OUT UINT16 **BootNext)
{
  EFI_STATUS                   Status;
  EFI_BOOT_MANAGER_LOAD_OPTION LoadOption;
  BOOLEAN                      BootManagerFlag;

  // Unregister Key Callback
  UnregisterKeyCallback ();

  // Get Boot Manager Flag
  BootManagerFlag = EnterBootManager ();
  if (BootManagerFlag == FALSE) {
    return;
  }

  // Get Boot Manager Load Option
  Status = MsBootOptionsLibGetBootManagerMenu (&LoadOption, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to get Boot Manager Load Option! Status = %r", __FUNCTION__, Status));
    return;
  }

  // Execute Boot Manager Load Option
  EfiBootManagerBoot           (&LoadOption);
  EfiBootManagerFreeLoadOption (&LoadOption);
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (IN UINT16 TimeoutRemain)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color;

  // Verify Combo Message Position
  if (XPos == 0 && YPos == 0) {
    return;
  }

  // Get Timeout Time
  UINT16 Timeout = PcdGet16 (PcdPlatformBootTimeOut);

  // Check Remaining Timeout
  if (TimeoutRemain == Timeout || TimeoutRemain == 0) {
    // Set Draw Color
    UINT8 DrawColor = (TimeoutRemain == Timeout) ? 0xFF : 0x00;

    // Set GOP Draw Color
    Color.Red = Color.Green = Color.Blue = DrawColor;

    // Print Combo Message
    AsciiPrintXY (XPos, YPos, &Color, NULL, ComboMessage);
  }
}

//
// DBY09 追加：把已注册的启动项全部打到屏幕上（DEBUG 由 ShengGopLogDxe 画进 framebuffer）
//
STATIC
VOID
Dby09DumpBootOptions (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION *Options = NULL;
  UINTN                         Count   = 0;

  Options = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  DEBUG ((EFI_D_ERROR, "DBY09: boot options = %d\n", Count));

  if (Options == NULL) {
    return;
  }

  for (UINTN i = 0; i < Count; i++) {
    DEBUG ((
      EFI_D_ERROR,
      "DBY09:   [%d] %s  attr=0x%x\n",
      Options[i].OptionNumber,
      Options[i].Description,
      Options[i].Attributes
      ));
  }

  EfiBootManagerFreeLoadOptions (Options, Count);
}

//
// DBY09 追加：逐个 SimpleFileSystem 探测 \EFI\BOOT\BOOTAA64.EFI，
// 用来判断 UFS 里到底有没有可启动的 ESP（不猜，直接看）
//
STATIC
VOID
Dby09ProbeFileSystems (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                      *Handles = NULL;
  UINTN                            Count   = 0;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs      = NULL;
  EFI_FILE_PROTOCOL               *Root    = NULL;
  EFI_FILE_PROTOCOL               *File    = NULL;
  STATIC CONST CHAR16             *Probes[] = {
    L"\\EFI\\BOOT\\BOOTAA64.EFI",
    L"\\startup.nsh",
    L"\\efi\\boot\\bootaa64.efi"
  };

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    DEBUG ((EFI_D_ERROR, "DBY09: no SimpleFileSystem handle (%r)\n", Status));
    return;
  }

  DEBUG ((EFI_D_ERROR, "DBY09: SimpleFileSystem handles = %d\n", Count));

  for (UINTN i = 0; i < Count; i++) {
    Fs   = NULL;
    Root = NULL;

    Status = gBS->HandleProtocol (Handles[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status) || (Fs == NULL)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
      DEBUG ((EFI_D_ERROR, "DBY09:  fs[%d] OpenVolume failed (%r)\n", i, Status));
      continue;
    }

    for (UINTN j = 0; j < ARRAY_SIZE (Probes); j++) {
      File   = NULL;
      Status = Root->Open (Root, &File, (CHAR16 *)Probes[j], EFI_FILE_MODE_READ, 0);
      DEBUG ((EFI_D_ERROR, "DBY09:  fs[%d] %s -> %r\n", i, Probes[j], Status));

      if (!EFI_ERROR (Status) && (File != NULL)) {
        File->Close (File);
      }
    }

    Root->Close (Root);
  }

  FreePool (Handles);
}

//
// DBY09 追加：启动 **UniXNU 官方启动菜单**（UniXNU.efi）
//
// 之前这里 boot 的是 tianocore 的 MdeModulePkg/Application/BootManagerMenuApp，
// 那是 EDK2 的**通用文本菜单**（80x25 控制台、蓝底白字），跟 UniXNU 官方的
// 那个界面（苹果 logo + "UniXNU version 0.1.0" + Return to firmware / Restart /
// Power off）不是一回事。用户要的是后者。
//
// 官方界面在 UniXNUPkg/Application/UniXNU/UniXNU.inf（FILE_GUID
// 9B665204-A133-47A2-9396-9E2B370A4E25），install_dby_w09.sh 把上游
// UniXNU/package/UniXNUPkg 原样铺到 $UEFI/UniXNUPkg，本包 dsc/fdf 把它编进 FvMain。
// 它直接用 GOP Blt 画像素（Library/UniXNULib/Ui.c：FillRect / EfiBltBufferToVideo），
// 版面按 GOP 分辨率算，且有专门的 Portrait 布局 —— 本机 GOP = 1600x2560 竖屏，
// 所以它天然就是"竖着、铺满整屏"，不需要任何旋转补丁。
//
// 官方加载方式是 UniXNUDxe 注册 Boot#### 再让 BDS 去 boot；本机 BDS 的
// BootOrder 是空的（EnterBootManager 恒 FALSE，且没有 BootOption 刷新路径），
// 靠它注册未必轮得上，所以这里直接从 FV 读文件、自己 LoadImage + StartImage，
// 并按 UniXNU.c:24-44 的约定塞 LoadOptions = "unixnu:firmware"，
// 让 App 走 UniXNUHostFirmware 分支（"Return to firmware" 会返回 EFI_ABORTED，
// 让 BDS 继续下一个选项）。App 本体一个字节没改。
//
#define DBY09_DEBUG_MENU  0

STATIC CONST EFI_GUID  mUniXNUAppFileGuid = {
  0x9B665204, 0xA133, 0x47A2, { 0x93, 0x96, 0x9E, 0x2B, 0x37, 0x0A, 0x4E, 0x25 }
};

//
// 与 UniXNU/package/UniXNUPkg/Application/UniXNU/UniXNU.c:22 完全一致。
// sizeof 包含结尾 NUL，UniXNU.c 那边比较的也是 sizeof(mFirmwareMarker)。
//
STATIC CONST CHAR16  mUniXNULoadOptions[] = L"unixnu:firmware";

//
// 从 FV 里取出 UniXNU 的 **PE32 段**（成功时 *FileBuffer 是 GetSection 分配的缓冲，
// 调用者 FreePool），同时拼出该文件的 Fv(vol)/FvFile(guid) 设备路径（同样由调用者 FreePool）。
//
// ★ 注意：拿到的必须是 PE 段，不是 FFS 文件流。见下面 ReadSection 那段注释。
//
// ★ 为什么两样都要给 DxeCore：
//   1) FilePath 不能为 NULL —— MdeModulePkg/Core/Dxe/Image/Image.c:1230-1245
//      （CoreLoadImageCommon）不管 SourceBuffer 有没有，都会执行
//          CoreLocateDevicePath (&gEfiDevicePathProtocolGuid, &HandleFilePath, ...)
//      HandleFilePath 就是 FilePath。上一版传 NULL 直接解引用空指针
//      → Data abort (Translation fault, third level)，X0 = 0、X2 = 0x1A030。
//   2) SourceBuffer 必须自己给 —— 传 NULL 的话 DxeCore 会去调
//      GetFileBufferByFilePath()，自己读、自己给，这条路径就彻底不用管了。
//
// ★ Fv->ReadFile 的四个出参一个都不能传 NULL：
//   MdeModulePkg/Core/Dxe/FwVol/FwVolRead.c 的 FvReadFile 无条件解引用
//       *FoundType / *FileAttributes / *AuthenticationStatus / *BufferSize
//   2026-10-22 真机崩就是因为 FileAttributes、AuthenticationStatus 传了 NULL，
//   屏幕上连 "DBY09: UniXNU.efi =" 都打不出来（崩在 ReadFile 内部）。
//
//   Buffer 传 NULL 时只回填 FileSize 就返回（不分配、不拷贝），所以这里只用它探长度。
//
STATIC
EFI_STATUS
Dby09ReadFvFile (
  IN  CONST EFI_GUID             *FileGuid,
  OUT VOID                      **FileBuffer,
  OUT UINTN                      *FileSize,
  OUT EFI_DEVICE_PATH_PROTOCOL  **FilePath
  )
{
  EFI_STATUS                         Status;
  EFI_HANDLE                        *Handles;
  UINTN                              Count;
  UINTN                              Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL     *Fv;
  EFI_DEVICE_PATH_PROTOCOL          *FvPath;
  VOID                              *Section;
  UINTN                              SectionSize;
  UINTN                              Size;
  EFI_FV_FILETYPE                    FoundType;
  EFI_FV_FILE_ATTRIBUTES             FileAttr;
  UINT32                             AuthStatus;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FvFileNode;

  *FileBuffer = NULL;
  *FileSize   = 0;
  *FilePath   = NULL;

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiFirmwareVolume2ProtocolGuid,
                   NULL,
                   &Count,
                   &Handles
                   );
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    DEBUG ((EFI_D_ERROR, "DBY09: no FirmwareVolume2 handle (%r)\n", Status));
    return Status;
  }

  DEBUG ((EFI_D_ERROR, "DBY09: FirmwareVolume2 handles = %d\n", Count));

  for (Index = 0; Index < Count; Index++) {
    Fv = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv))) {
      continue;
    }

    Size   = 0;
    Status = Fv->ReadFile (Fv, FileGuid, NULL, &Size, &FoundType, &FileAttr, &AuthStatus);
    DEBUG ((
      EFI_D_ERROR,
      "DBY09: FV[%d] probe -> %r  size=0x%x type=%d attr=0x%x\n",
      (UINT32)Index,
      Status,
      (UINT32)Size,
      (UINT32)FoundType,
      (UINT32)FileAttr
      ));
    if (EFI_ERROR (Status) || (Size == 0)) {
      continue;
    }

    FvPath = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, (VOID **)&FvPath)) ||
        (FvPath == NULL))
    {
      continue;
    }

    //
    // ★★ 不能用 ReadFile 取镜像！
    //   MdeModulePkg/Core/Dxe/FwVol/FwVolRead.c:FvReadFile 只跳过 24 字节 FFS 文件头，
    //   把**整个 section 流**原样拷给调用者。而 GenFv 生成的 FFS 文件里
    //   section 流的第一段通常是 EFI_SECTION_USER_INTERFACE（本机就是 "UniXNU"），
    //   所以读出来的东西开头是 15 55 00 6E 00 69 …，根本不是 "MZ"。
    //   2026-10-22 真机现场：ReadFile 成功、size=0x1A018，但
    //       DBY09: LoadImage(UniXNU) -> Unsupported
    //   因为 PeCoffLoaderGetImageInfo 解析不到 DOS/PE 头，返回 EFI_UNSUPPORTED。
    //   （文件实体 0x1A030−0x18 正好等于 0x1A018，说明流被整体搬过来了。）
    //
    //   正确做法：ReadSection(EFI_SECTION_PE32)，由 Section Extraction Protocol
    //   只把 PE32 段数据交出来 —— 这也是 DxeCore 的 GetFileBufferByFilePath
    //   处理 FvFile 设备路径时走的路。
    //
    //   ReadSection 的 Buffer 同样是 IN OUT VOID **，但这里 *Buffer 传 NULL 是
    //   **故意**的：GetSection() 会在 *Buffer == NULL 时自己 AllocatePool，
    //   返回值由调用者 FreePool。FwVolRead.c:FvReadFileSection 里只拒绝
    //   "Buffer（二级指针本身）为 NULL"，不拒绝 *Buffer 为 NULL。
    //
    Section     = NULL;
    SectionSize = 0;
    Status      = Fv->ReadSection (
                        Fv,
                        FileGuid,
                        EFI_SECTION_PE32,
                        0,
                        &Section,
                        &SectionSize,
                        &AuthStatus
                        );
    if (EFI_ERROR (Status) || (Section == NULL)) {
      //
      // GenFv 也可能产出 EFI_SECTION_TE（Terse Executable），兜一手。
      //
      Section     = NULL;
      SectionSize = 0;
      Status      = Fv->ReadSection (
                          Fv,
                          FileGuid,
                          EFI_SECTION_TE,
                          0,
                          &Section,
                          &SectionSize,
                          &AuthStatus
                          );
    }
    if (EFI_ERROR (Status) || (Section == NULL)) {
      DEBUG ((EFI_D_ERROR, "DBY09: FV[%d] ReadSection(PE32/TE) failed (%r)\n", (UINT32)Index, Status));
      continue;
    }

    DEBUG ((
      EFI_D_ERROR,
      "DBY09: FV[%d] PE section: size=0x%x magic=%c%c\n",
      (UINT32)Index,
      (UINT32)SectionSize,
      ((CHAR8 *)Section)[0],
      ((CHAR8 *)Section)[1]
      ));

    ZeroMem (&FvFileNode, sizeof (FvFileNode));
    FvFileNode.Header.Type    = MEDIA_DEVICE_PATH;
    FvFileNode.Header.SubType = MEDIA_PIWG_FW_FILE_DP;
    SetDevicePathNodeLength (&FvFileNode.Header, sizeof (FvFileNode));
    CopyGuid (&FvFileNode.FvFileName, FileGuid);

    *FilePath = AppendDevicePathNode (FvPath, (EFI_DEVICE_PATH_PROTOCOL *)&FvFileNode);
    if (*FilePath == NULL) {
      FreePool (Section);
      Status = EFI_OUT_OF_RESOURCES;
      continue;
    }

    *FileBuffer = Section;
    *FileSize   = SectionSize;
    Status      = EFI_SUCCESS;
    break;
  }

  gBS->FreePool (Handles);

  if (*FileBuffer == NULL) {
    Status = EFI_NOT_FOUND;
  }

  return Status;
}

//
// 直接起 UniXNU.efi；成功运行返回时 Status = EFI_ABORTED（Return to firmware）。
// 返回 EFI_NOT_FOUND 表示 FV 里没有这个 App（没铺 UniXNUPkg 或没编进去）。
//
STATIC
EFI_STATUS
Dby09StartUniXNU (
  VOID
  )
{
  EFI_STATUS                   Status;
  EFI_DEVICE_PATH_PROTOCOL    *FilePath;
  VOID                        *FileBuffer;
  UINTN                        FileSize;
  CHAR16                      *PathText;
  EFI_HANDLE                   ImageHandle;
  EFI_LOADED_IMAGE_PROTOCOL   *Loaded;
  UINTN                        ExitDataSize;
  CHAR16                      *ExitData;

  FilePath   = NULL;
  FileBuffer = NULL;
  FileSize   = 0;

  Status = Dby09ReadFvFile (&mUniXNUAppFileGuid, &FileBuffer, &FileSize, &FilePath);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "DBY09: UniXNU.efi (9B665204) not in any FV (%r)\n", Status));
    return EFI_NOT_FOUND;
  }

  PathText = ConvertDevicePathToText (FilePath, FALSE, FALSE);
  DEBUG ((
    EFI_D_ERROR,
    "DBY09: UniXNU.efi = %s  buf=%p size=0x%x\n",
    (PathText != NULL) ? PathText : L"(device path)",
    FileBuffer,
    (UINT32)FileSize
    ));
  if (PathText != NULL) {
    FreePool (PathText);
  }

  //
  // FilePath 与 SourceBuffer 同时给：
  //   FilePath    —— 满足 CoreLoadImageCommon 里 CoreLocateDevicePath 的解引用前提
  //   SourceBuffer —— 让 DxeCore 直接用这份缓冲，不再走 GetFileBufferByFilePath
  //
  ImageHandle = NULL;
  Status      = gBS->LoadImage (
                       FALSE,
                       gImageHandle,
                       FilePath,
                       FileBuffer,
                       FileSize,
                       &ImageHandle
                       );
  DEBUG ((EFI_D_ERROR, "DBY09: LoadImage(UniXNU) -> %r handle=%p\n", Status, ImageHandle));

  FreePool (FilePath);
  FreePool (FileBuffer);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // LoadOptions 必须在 StartImage 之前塞进已加载镜像的 LoadedImage。
  //
  Loaded = NULL;
  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded)) &&
      (Loaded != NULL))
  {
    Loaded->LoadOptionsSize = sizeof (mUniXNULoadOptions);
    Loaded->LoadOptions     = AllocateCopyPool (sizeof (mUniXNULoadOptions), mUniXNULoadOptions);
    if (Loaded->LoadOptions == NULL) {
      Loaded->LoadOptionsSize = 0;
    }
  }

  DEBUG ((EFI_D_ERROR, "DBY09: StartImage(UniXNU) ...\n"));

  ExitDataSize = 0;
  ExitData     = NULL;
  Status       = gBS->StartImage (ImageHandle, &ExitDataSize, &ExitData);

  DEBUG ((
    EFI_D_ERROR,
    "DBY09: UniXNU returned (%r)%s%s\n",
    Status,
    (ExitData != NULL) ? L"  exit=" : L"",
    (ExitData != NULL) ? ExitData : L""
    ));

  return Status;
}

//
// 菜单入口：先起官方 UniXNU；FV 里没有它才退回 tianocore 文本菜单。
//
STATIC
VOID
Dby09BootManagerMenu (
  VOID
  )
{
  EFI_STATUS                   Status;
  EFI_BOOT_MANAGER_LOAD_OPTION LoadOption;
  CHAR16                      *PathText;

  Status = Dby09StartUniXNU ();
  if (Status != EFI_NOT_FOUND) {
    return;
  }

  DEBUG ((EFI_D_ERROR, "DBY09: fallback to EDK2 BootManagerMenuApp\n"));

  Status = MsBootOptionsLibGetBootManagerMenu (&LoadOption, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "DBY09: GetBootManagerMenu failed (%r)\n", Status));
    return;
  }

  PathText = NULL;
  if (LoadOption.FilePath != NULL) {
    PathText = ConvertDevicePathToText (LoadOption.FilePath, FALSE, FALSE);
  }

  DEBUG ((
    EFI_D_ERROR,
    "DBY09: launching EDK2 Boot Manager Menu: %s  [%s]\n",
    LoadOption.Description,
    (PathText != NULL) ? PathText : L"(null)"
    ));

  if (PathText != NULL) {
    FreePool (PathText);
  }

  EfiBootManagerBoot           (&LoadOption);
  EfiBootManagerFreeLoadOption (&LoadOption);

  DEBUG ((EFI_D_ERROR, "DBY09: Boot Manager Menu returned\n"));
}

//
// DBY09 追加：把一组 handle 的设备路径打成可读文本
//
STATIC
VOID
Dby09DumpHandlePaths (
  IN EFI_HANDLE *Handles,
  IN UINTN       Count
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  CHAR16                   *Text;

  for (UINTN i = 0; i < Count; i++) {
    DevicePath = NULL;
    Text       = NULL;

    Status = gBS->HandleProtocol (Handles[i], &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath);
    if (!EFI_ERROR (Status) && (DevicePath != NULL)) {
      Text = ConvertDevicePathToText (DevicePath, FALSE, FALSE);
    }

    DEBUG ((
      EFI_D_ERROR,
      "DBY09:    handle[%d] %p  %s\n",
      i,
      Handles[i],
      (Text != NULL) ? Text : L"(no device path)"
      ));

    if (Text != NULL) {
      FreePool (Text);
    }
  }
}

//
// DBY09 追加：输入设备现状
//   目的：用真机证据判断 "没有输入" 到底缺哪一环（ConIn 有没有、STIEx 在不在、
//   KeypadDevice 协议有没有被 ButtonsDxe 发布出来），而不是继续靠猜。
//
STATIC
VOID
Dby09DumpInputDevices (
  VOID
  )
{
  EFI_STATUS   Status;
  EFI_HANDLE  *Handles;
  UINTN        Count;
  CHAR16      *KeypadText;

  DEBUG ((EFI_D_ERROR, "DBY09: ConIn=%p  ConOut=%p\n", gST->ConIn, gST->ConOut));

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextInputExProtocolGuid, NULL, &Count, &Handles);
  DEBUG ((EFI_D_ERROR, "DBY09: SimpleTextInEx handles=%d (%r)\n", Count, Status));
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    Dby09DumpHandlePaths (Handles, Count);
    FreePool (Handles);
  }

  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gKeypadDeviceProtocolGuid, NULL, &Count, &Handles);
  DEBUG ((EFI_D_ERROR, "DBY09: KeypadDevice handles=%d (%r)\n", Count, Status));
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    Dby09DumpHandlePaths (Handles, Count);
    FreePool (Handles);
  }

  KeypadText = ConvertDevicePathToText ((EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath, FALSE, FALSE);
  DEBUG ((EFI_D_ERROR, "DBY09: KeypadDevicePath = %s\n",
    (KeypadText != NULL) ? KeypadText : L"(null)"));
  if (KeypadText != NULL) {
    FreePool (KeypadText);
  }
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot ()
{
  // Execute Secondary Unable To Boot
  DeviceBootManagerUnableToBoot ();

  //
  // 原实现：DisplayBootGraphic (BG_NO_BOOT_OS) → Stall(10s) → ResetSystem(Shutdown)
  // 本机上 Shutdown 走不通会被 ABL/看门狗变成重启，故不再调用。
  //
  DEBUG ((EFI_D_ERROR, "\n"));
  DEBUG ((EFI_D_ERROR, "DBY09: ========================================\n"));
  DEBUG ((EFI_D_ERROR, "DBY09: no bootable OS found (Internal Storage / USB both failed)\n"));
  DEBUG ((EFI_D_ERROR, "DBY09: fallback -> Boot Manager menu, NOT shutdown\n"));
  DEBUG ((EFI_D_ERROR, "DBY09: ========================================\n"));

  Dby09DumpBootOptions ();
  Dby09ProbeFileSystems ();
  Dby09DumpInputDevices ();

  //
  // UniXNU 一起来就整屏重绘（GOP Blt 填底色 + 画 logo/文字），上面这些诊断
  // 会被直接盖掉。所以这里留 5 秒：给"原始 DEBUG 日志"留一张照片，然后切菜单。
  // 静默停住会被当成卡死，所以逐秒打一行。
  //
  for (UINTN Left = 5; Left > 0; Left--) {
    DEBUG ((
      EFI_D_ERROR,
      "DBY09: ---- photo the raw log above ---- switching to UniXNU menu in %d s\n",
      (UINT32)Left
      ));
    gBS->Stall (1000000);
  }

  //
  // 菜单循环：UniXNU 的 "Return to firmware" 返回 EFI_ABORTED（见
  // UniXNU.c:38-44），语义是"交回固件继续下一个启动项"。本机 BDS 的
  // BootOrder 是空的，交回来还是回到这里，所以直接重进菜单 —— 与"BDS 里
  // 还有下一个选项"的行为等价，比停在黑屏/死循环可读。
  // 每轮之间停 0.5 s，避免上电即失败时空转把 CPU 打满。
  //
  for ( ;; ) {
    Dby09BootManagerMenu ();
    DEBUG ((EFI_D_ERROR, "DBY09: menu returned, re-entering (no other boot option)\n"));
    gBS->Stall (500000);
  }
}

VOID
EFIAPI
PlatformBootManagerBdsEntry ()
{
  EFI_STATUS Status;

  // Setup Secure Boot
  Status = SetupSecureBoot ();
  ASSERT_EFI_ERROR (Status);

  // Update ACPI Tables
  UpdateAcpiTables ();

  // Execute Secondary BDS Entry
  DeviceBootManagerBdsEntry ();
}
