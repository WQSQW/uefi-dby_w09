/**
 * SimpleFb.c —— DBY-W09 覆盖版（90° 横屏 + 可选整数放大）
 *
 * [DBY-W09 ROT90 PATCH]  覆盖 uefi-sheng/Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c
 *
 * 为什么要覆盖这个文件：
 *   物理帧缓冲是 ABL 点亮 DSI 后交下来的 "Display Reserved" 区，几何恒为
 *   1600(宽/stride 方向) x 2560(高)，stride = 1600*4 = 6400 B。PCD
 *   PcdFrameBufferWidth/Height 必须保持物理真值 —— 对调成 2560x1600 会让
 *   FrameBufferBltLib 用 10240B 的行跨度去写只有 6400B 的缓冲，每行漂移
 *   3840B，画面上就是满屏重复竖条（老树 boot_uefi_2560x1600.img 实测过）。
 *   ⇒ 想横过来，只能在 Blt 出口做坐标映射，分辨率一个字节都不许动。
 *
 * 本补丁做两件事，全部在 Blt 出口完成：
 *   1) 90° 旋转：逻辑画布 = 物理宽高交换 = 2560 x 1600，报给上层的是这个。
 *   2) 可选整数放大 FB_SCALE：每个逻辑像素铺成 FB_SCALE x FB_SCALE 个物理像素。
 *      FB_SCALE=1 时逻辑画布 2560x1600 → 文本控制台 320 列 x 84 行。
 *
 * 实现要点（和老树 build_uefi_rot.sh 的版本区别）：
 *   老版本是逐像素直写物理缓冲，整屏 clear 要跑 400 万次函数调用，很慢。
 *   这里改成：FrameBufferBltLib 照旧工作在**一块内存逻辑缓冲**上（快路径不丢），
 *   Blt 完之后只把**脏矩形**按 90° 摊到物理缓冲，且按"物理行"顺序写
 *   （目标连续、源跨行），并把 cache 刷写精确到被写过的那几行，不再整屏刷 16MB。
 *
 * 方向约定（FB_ROT_DIR = 90，顺时针）：
 *   逻辑 (x, y) -> 物理 (Px, Py) = (mPanelW - 1 - y, x)
 *   验证：逻辑 2560x1600，x∈[0,2560) 映射到物理行 Py∈[0,2560) ✓
 *         y∈[0,1600) 映射到物理列 Px∈[0,1600) ✓
 *   效果：板子按横屏拿起来看时，内容是正的（和老树成品一致）。
 */
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/PcdLib.h>

#include <Configuration/BootDevices.h>

//
// 0 = 不旋转（逻辑 = 物理）  90 = 顺时针 90  270 = 逆时针 90
//
#ifndef FB_ROT_DIR
#define FB_ROT_DIR  90
#endif

//
// 逻辑像素放大倍数。1 = 1:1（2560x1600，控制台 320x84）
//                    2 = GOP 1280x800（控制台 160x42，字形 16x38 物理像素）
// 用户明确否决过放大版："放大看着不舒服而且显示不全" ⇒ 默认 1。
//
#ifndef FB_SCALE
#define FB_SCALE    1
#endif

//
// Global Variables
//
STATIC FRAME_BUFFER_CONFIGURE *FrameBufferConfiguration; // 配置在"内存逻辑缓冲"上

STATIC UINT8  *mPhysFb;    // 物理帧缓冲基址（ABL 交下来的）
STATIC UINT32  mPanelW;    // 物理宽（= stride 方向）
STATIC UINT32  mPanelH;    // 物理高
STATIC UINT32  mLogW;      // 逻辑画布宽（报给上层）
STATIC UINT32  mLogH;      // 逻辑画布高
STATIC UINTN   mBpp;       // 每像素字节数
STATIC UINT32  mScale;     // 生效的放大倍数（FB_SCALE 不整除时退化为 1）
STATIC UINT8  *mLogicalFb; // 逻辑画布内存缓冲（Blt 真正写的地方）
STATIC UINTN   mLogicSize; // 逻辑缓冲字节数

//
// [DBY-W09 PHYFB v1] SimpleFbDxe -> DarwinBoot 私有协议：
// GOP 的 FrameBufferBase 报的是上面那块逻辑画布（AllocatePool 的堆内存，
// DSI 从不扫描它）。绕过 GOP 直写显存的内核加载路径（boot_args.Video[]、
// Entry.S 探针）需要的是 mPhysFb 这块"Display Reserved"真显存 —— 通过本协议
// 交给 DarwinBoot。结构定义与 DarwinBootPkg/Driver/DarwinBoot.c 里的副本
// 必须逐字段一致（跨包不共头文件，双份维护，改动要两边同步）。
//
#define DBY_PHYFB_SIGNATURE  0x46504244u /* 'DBPF' */
#define DBY_PHYFB_VERSION    1
typedef struct {
  UINT32  Signature;
  UINT32  Version;
  UINT64  PhysBase;    // 物理帧缓冲基址（"Display Reserved"）
  UINT32  PanelW;      // 物理宽（= stride 方向），本机 1600
  UINT32  PanelH;      // 物理高，本机 2560
  UINT32  PitchBytes;  // PanelW * Bpp = 6400
  UINT32  Rotation;    // 逻辑->物理旋转角（0/90/270）
  UINT32  LogicalW;    // GOP 侧逻辑几何（仅参考）
  UINT32  LogicalH;
} DBY_PHYFB_INFO;

STATIC EFI_GUID gDbyPhyFbInfoProtocolGuid = {
  0x124e37b7, 0xca39, 0x4c80, { 0x90, 0x4d, 0xeb, 0x46, 0xbf, 0x58, 0xec, 0xca }
};
STATIC EFI_HANDLE      mPhyFbInfoHandle = NULL;

EFI_STATUS
FrameBufferQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                 ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;

  // Allocate Memory
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION), (VOID *)Info);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Pass Info Size
  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);

  // Pass Frame Buffer Infos
  (*Info)->Version              = This->Mode->Info->Version;
  (*Info)->HorizontalResolution = This->Mode->Info->HorizontalResolution;
  (*Info)->VerticalResolution   = This->Mode->Info->VerticalResolution;
  (*Info)->PixelFormat          = This->Mode->Info->PixelFormat;
  (*Info)->PixelsPerScanLine    = This->Mode->Info->PixelsPerScanLine;

  return EFI_SUCCESS;
}

EFI_STATUS
FrameBufferSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN UINT32                        ModeNumber)
{
  return EFI_SUCCESS;
}

/**
  把逻辑缓冲里的一个矩形按 90° 摊到物理缓冲。

  @param[in] X, Y        逻辑矩形左上角
  @param[in] Width       逻辑宽
  @param[in] Height      逻辑高
**/
STATIC
VOID
RotateFlush (
  IN UINTN X,
  IN UINTN Y,
  IN UINTN Width,
  IN UINTN Height)
{
  UINTN XEnd;
  UINTN YEnd;
  UINTN Row;
  UINTN Col;

  if ((Width == 0) || (Height == 0) || (mLogicalFb == NULL)) {
    return;
  }

  XEnd = X + Width;
  YEnd = Y + Height;
  if (XEnd > mLogW) {
    XEnd = mLogW;
  }

  if (YEnd > mLogH) {
    YEnd = mLogH;
  }

  if (X >= XEnd || Y >= YEnd) {
    return;
  }

  //
  // 快路径（默认 FB_SCALE=1，32bpp）：物理行 Py = 逻辑列 x，
  // 行内 Px 由小到大对应逻辑 y 由大到小 ⇒ 目标连续写、源跨行读。
  //
  if ((mScale == 1) && (mBpp == 4)) {
    //
    // Row 遍历的是**逻辑 x**（= 物理行 Py），Col 遍历的是**逻辑 y**。
    // 逻辑缓冲是标准 (x,y) 布局：像素在 LogicalFb[ y * mLogW + x ]。
    // 所以取源必须是 Y 那一行、Row 那一列 ⇒ 起点 LogicalFb[ Y*mLogW + Row ]，
    // 行内沿 y 递增 ⇒ 每次跨 mLogW 个像素（源跨行读、目标连续写）。
    //
    // ★ 2026-10-22：上一版写成 LogicalFb[ Row*mLogW + Y ] 且行内连续取
    //   Src[Col-1-Y]，等于把逻辑缓冲当成转置布局 —— 每个 Blt 矩形被读成
    //   转置块（字符矩形的宽高接近，看起来就是"竖排/乱码"），而大块图形
    //   （苹果 logo）转置后仍近似可辨，所以出现"苹果出来了但字全乱"。
    //
    for (Row = X; Row < XEnd; Row++) {
      UINT32 *Src = (UINT32 *)(mLogicalFb + ((UINTN)Y * mLogW + Row) * mBpp);
      UINT32 *Dst = (UINT32 *)(mPhysFb + ((UINTN)Row * mPanelW + (mPanelW - YEnd)) * mBpp);

      for (Col = YEnd; Col > Y; Col--) {
        *Dst++ = Src[(Col - 1 - Y) * mLogW];
      }

      // 只刷这一行里真正被写到的字节，别整屏 16MB 刷
      WriteBackInvalidateDataCacheRange (mPhysFb + ((UINTN)Row * mPanelW + (mPanelW - YEnd)) * mBpp, (YEnd - Y) * mBpp);
    }

    return;
  }

  //
  // 通用路径（FB_SCALE > 1 或非 32bpp）：一个逻辑像素铺成 mScale x mScale 块。
  //
  for (Row = Y; Row < YEnd; Row++) {
    CONST UINT8 *SrcRow = mLogicalFb + ((UINTN)Row * mLogW + X) * mBpp;

    for (Col = X; Col < XEnd; Col++) {
      UINT8  Color[8];
      UINT32 Dx;
      UINT32 Dy;

      CopyMem (Color, SrcRow + (Col - X) * mBpp, mBpp);

      for (Dy = 0; Dy < mScale; Dy++) {
        UINTN  Py    = Col * mScale + Dy;
        UINT8 *DstRow = mPhysFb + Py * mPanelW * mBpp;

        for (Dx = 0; Dx < mScale; Dx++) {
          UINTN Px = mPanelW - 1 - (Row * mScale + Dx);
          CopyMem (DstRow + Px * mBpp, Color, mBpp);
        }
      }
    }
  }

  for (Row = X * mScale; Row < XEnd * mScale; Row++) {
    WriteBackInvalidateDataCacheRange (mPhysFb + Row * mPanelW * mBpp, mPanelW * mBpp);
  }
}

EFI_STATUS
FrameBufferGopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL      *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL     *BltBuffer,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation OPTIONAL,
  IN UINTN                              SourceX,
  IN UINTN                              SourceY,
  IN UINTN                              DestinationX,
  IN UINTN                              DestinationY,
  IN UINTN                              Width,
  IN UINTN                              Height,
  IN UINTN                              Delta OPTIONAL)
{
  EFI_STATUS Status;
  EFI_TPL    Tpl;

  // Raise TPL Level
  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  // Draw to the logical buffer first（FrameBufferBltLib 的快路径原样保留）
  Status = FrameBufferBlt (FrameBufferConfiguration, BltBuffer, BltOperation, SourceX, SourceY, DestinationX, DestinationY, Width, Height, Delta);

  //
  // 只有"往屏幕上写"的三种操作需要摊到物理缓冲；
  // EfiBltVideoToBltBuffer 是读屏，不写。
  //
  if (!EFI_ERROR (Status)) {
    switch (BltOperation) {
      case EfiBltVideoFill:
      case EfiBltBufferToVideo:
      case EfiBltVideoToVideo:
        RotateFlush (DestinationX, DestinationY, Width, Height);
        break;

      default:
        break;
    }
  }

  // Restore TPL Level
  gBS->RestoreTPL (Tpl);

  return Status;
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mFrameBuffer = {
  FrameBufferQueryMode,
  FrameBufferSetMode,
  FrameBufferGopBlt,
  NULL
};

EFI_STATUS
GetFrameBufferInfos (
  OUT EFI_PHYSICAL_ADDRESS *BaseAddress,
  OUT UINT32               *Width,
  OUT UINT32               *Height,
  OUT UINT32               *ColorDepth)
{
  EFI_MEMORY_REGION_DESCRIPTOR FrameBufferRegion = { 0 };

  // Locate "Display Reserved" Memory Region
  LocateMemoryRegionByName ("Display Reserved", &FrameBufferRegion);
  LocateMemoryRegionByName ("Display_Reserved", &FrameBufferRegion);

  // Verify Frame Buffer Region
  if (!FrameBufferRegion.Address) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate \"Display Reserved\" Memory Region!\n"));
    return EFI_NOT_FOUND;
  }

  // Get Frame Buffer Details（物理真值，禁止改）
  *Width      = FixedPcdGet32 (PcdFrameBufferWidth);
  *Height     = FixedPcdGet32 (PcdFrameBufferHeight);
  *ColorDepth = FixedPcdGet32 (PcdFrameBufferColorDepth);

  // Verify Frame Buffer Resolution
  if (*Width < 640 || *Height < 475) {
    DEBUG ((EFI_D_ERROR, "Frame Buffer Resolution is too Small!\n"));
    DEBUG ((EFI_D_ERROR, "Minimal Supported Resolution = 640x475\n"));

    return EFI_UNSUPPORTED;
  }

  // Pass Frame Buffer Address
  *BaseAddress = FrameBufferRegion.Address;

  return EFI_SUCCESS;
}

EFI_STATUS
AllocateGopMemory ()
{
  EFI_STATUS Status;

  // Allocate Memory
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE), (VOID *)&mFrameBuffer.Mode);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for GOP Mode! Status = %r\n", Status));
    return Status;
  }

  // Clear Memory
  ZeroMem (mFrameBuffer.Mode, sizeof (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));

  // Allocate Memory
  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION), (VOID *)&mFrameBuffer.Mode->Info);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for GOP Info! Status = %r\n", Status));
    return Status;
  }

  // Clear Memory
  ZeroMem (mFrameBuffer.Mode->Info, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));

  return Status;
}

EFI_STATUS
SetFrameBufferModeDetails (
  IN EFI_PHYSICAL_ADDRESS BaseAddress,
  IN UINT32               Width,
  IN UINT32               Height,
  IN UINT32               ColorDepth)
{
  // Set Info Size
  mFrameBuffer.Mode->SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);

  // Set Mode Infos
  mFrameBuffer.Mode->MaxMode = 1;
  mFrameBuffer.Mode->Mode    = 0;

  // Set Frame Buffer Range（报物理真值：这块内存是 ABL 交下来的）
  mFrameBuffer.Mode->FrameBufferBase = BaseAddress;
  mFrameBuffer.Mode->FrameBufferSize = (UINT32)((UINTN)mPanelW * mPanelH * mBpp);

  // Set Mode Info Version
  mFrameBuffer.Mode->Info->Version = 0;

  // Set Frame Buffer Infos（逻辑几何：旋转 + 缩放之后，上层看到的）
  mFrameBuffer.Mode->Info->HorizontalResolution = Width;
  mFrameBuffer.Mode->Info->VerticalResolution   = Height;
  mFrameBuffer.Mode->Info->PixelsPerScanLine    = Width;

  // Set Pixel Format
  switch (ColorDepth) {
    case 32:
      mFrameBuffer.Mode->Info->PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
      break;

    case 24:
      mFrameBuffer.Mode->Info->PixelFormat                   = PixelBitMask;
      mFrameBuffer.Mode->Info->PixelInformation.RedMask      = 0x00FF0000;
      mFrameBuffer.Mode->Info->PixelInformation.GreenMask    = 0x0000FF00;
      mFrameBuffer.Mode->Info->PixelInformation.BlueMask     = 0x000000FF;
      mFrameBuffer.Mode->Info->PixelInformation.ReservedMask = 0x00000000;
      break;

    case 16:
      mFrameBuffer.Mode->Info->PixelFormat                   = PixelBitMask;
      mFrameBuffer.Mode->Info->PixelInformation.RedMask      = 0x0000F800;
      mFrameBuffer.Mode->Info->PixelInformation.GreenMask    = 0x000007E0;
      mFrameBuffer.Mode->Info->PixelInformation.BlueMask     = 0x0000001F;
      mFrameBuffer.Mode->Info->PixelInformation.ReservedMask = 0x00000000;
      break;

    default:
      DEBUG ((EFI_D_ERROR, "Color Depth %u is not Supported!\n", ColorDepth));
      return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CreateFrameBufferConfig (IN EFI_PHYSICAL_ADDRESS BaseAddress)
{
  EFI_STATUS Status;

  // Set Configuration Size
  STATIC UINTN FrameBufferConfigurationSize = 0;

  // Get Frame Buffer Configuration Size
  FrameBufferBltConfigure ((VOID *)(UINTN)BaseAddress, mFrameBuffer.Mode->Info, FrameBufferConfiguration, &FrameBufferConfigurationSize);

  // Allocate Memory
  FrameBufferConfiguration = AllocatePool (FrameBufferConfigurationSize);
  if (FrameBufferConfiguration == NULL) {
    DEBUG ((EFI_D_ERROR, "Failed to Allocate Memory for the Frame Buffer Configuration!\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  // Create FrameBufferBltLib Configuration
  Status = FrameBufferBltConfigure ((VOID *)(UINTN)BaseAddress, mFrameBuffer.Mode->Info, FrameBufferConfiguration, &FrameBufferConfigurationSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Create the Frame Buffer Configuration! Status = %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}

VOID
FreeMemoryBuffers ()
{
  // Free Buffer
  if (mFrameBuffer.Mode->Info != NULL) {
    gBS->FreePool (mFrameBuffer.Mode->Info);
  }

  // Free Buffer
  if (mFrameBuffer.Mode != NULL) {
    gBS->FreePool (mFrameBuffer.Mode);
  }

  // Free Buffer
  if (FrameBufferConfiguration != NULL) {
    FreePool (FrameBufferConfiguration);
  }

  // Free Buffer
  if (mLogicalFb != NULL) {
    FreePool (mLogicalFb);
  }
}

EFI_STATUS
EFIAPI
RegisterFrameBuffer (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS BaseAddress;
  UINT32               Width;
  UINT32               Height;
  UINT32               ColorDepth;

  // Get Frame Buffer Infos（物理）
  Status = GetFrameBufferInfos (&BaseAddress, &Width, &Height, &ColorDepth);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mPhysFb = (UINT8 *)(UINTN)BaseAddress;
  mPanelW = Width;
  mPanelH = Height;
  mBpp    = ColorDepth / 8;

  //
  // 缩放倍数：物理宽高必须能被整除，否则退化为 1，绝不硬来。
  //
  mScale = FB_SCALE;
  if (mScale == 0) {
    mScale = 1;
  }

  if (((mPanelW % mScale) != 0) || ((mPanelH % mScale) != 0)) {
    DEBUG ((EFI_D_ERROR, "SimpleFbDxe: %ux%u not divisible by %u, scale -> 1\n", (UINTN)mPanelW, (UINTN)mPanelH, (UINTN)mScale));
    mScale = 1;
  }

  // 逻辑画布：先按旋转交换宽高，再除以放大倍数
#if (FB_ROT_DIR == 90) || (FB_ROT_DIR == 270)
  mLogW = mPanelH / mScale;
  mLogH = mPanelW / mScale;
#else
  mLogW = mPanelW / mScale;
  mLogH = mPanelH / mScale;
#endif

  mLogicSize = (UINTN)mLogW * mLogH * mBpp;

  // Allocate Memory（逻辑缓冲）
  mLogicalFb = AllocatePool (mLogicSize);
  if (mLogicalFb == NULL) {
    DEBUG ((EFI_D_ERROR, "SimpleFbDxe: failed to allocate %u B for logical buffer\n", (UINTN)mLogicSize));
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (mLogicalFb, mLogicSize);

  DEBUG ((
    EFI_D_ERROR,
    "SimpleFbDxe: physical %ux%u bpp %u  rot %d scale %u -> logical %ux%u (buf %u B)\n",
    (UINTN)mPanelW,
    (UINTN)mPanelH,
    (UINTN)ColorDepth,
    (UINTN)FB_ROT_DIR,
    (UINTN)mScale,
    (UINTN)mLogW,
    (UINTN)mLogH,
    (UINTN)mLogicSize
    ));

  // Allocate Memory
  Status = AllocateGopMemory ();
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // Set Frame Buffer Mode Details（逻辑几何）
  Status = SetFrameBufferModeDetails ((EFI_PHYSICAL_ADDRESS)(UINTN)mLogicalFb, mLogW, mLogH, ColorDepth);
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // Create Frame Buffer Config（配置在逻辑缓冲上）
  Status = CreateFrameBufferConfig ((EFI_PHYSICAL_ADDRESS)(UINTN)mLogicalFb);
  if (EFI_ERROR (Status)) {
    goto cleanup;
  }

  // 清屏：逻辑缓冲与物理缓冲都清黑（物理那块是 ABL 留下的脏数据）
  ZeroMem (mLogicalFb, mLogicSize);
  ZeroMem ((VOID *)mPhysFb, (UINTN)mPanelW * mPanelH * mBpp);
  WriteBackInvalidateDataCacheRange ((VOID *)mPhysFb, (UINTN)mPanelW * mPanelH * mBpp);

  // Register GOP Protocol
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gEfiDevicePathProtocolGuid, &DisplayDevicePath, &gEfiGraphicsOutputProtocolGuid, &mFrameBuffer, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Register GOP Protocol!\n"));
    ASSERT_EFI_ERROR (Status);
  }

  //
  // [DBY-W09 PHYFB v1] 发物理显存真值（GOP 语义一个字都不动，菜单/控制台照旧）。
  //
  //
  // [DBY-W09 PHYFB v2] EfiReservedMemory persistent alloc: v1 static block dies
  // with the driver image after BDS unload (kernel load window reuses it).
  //
  {
    DBY_PHYFB_INFO *Phy;
    if (!EFI_ERROR (gBS->AllocatePool (EfiReservedMemoryType, sizeof (DBY_PHYFB_INFO), (VOID **)&Phy))) {
      Phy->Signature  = DBY_PHYFB_SIGNATURE;
      Phy->Version    = DBY_PHYFB_VERSION;
      Phy->PhysBase   = (UINT64)(UINTN)mPhysFb;
      Phy->PanelW     = mPanelW;
      Phy->PanelH     = mPanelH;
      Phy->PitchBytes = (UINT32)(mPanelW * mBpp);
      Phy->Rotation   = FB_ROT_DIR;
      Phy->LogicalW   = mLogW;
      Phy->LogicalH   = mLogH;
      gBS->InstallProtocolInterface (
             &mPhyFbInfoHandle,
             &gDbyPhyFbInfoProtocolGuid,
             EFI_NATIVE_INTERFACE,
             Phy
             );
      DEBUG ((EFI_D_ERROR, "SimpleFbDxe: PHYFB v2 phys 0x%lx %ux%u row %u rot %u\n",
              Phy->PhysBase, mPanelW, mPanelH, Phy->PitchBytes, Phy->Rotation));
    }
  }

  return EFI_SUCCESS;

cleanup:
  // Free Memory Buffers
  FreeMemoryBuffers ();

  return Status;
}
