/**
 * FrameBufferSerialPortLib.c —— DBY-W09 覆盖版（文字随屏幕一起 90° 旋转）
 *
 * [DBY-W09 TEXT-ROT90 PATCH]  覆盖
 *   uefi-sheng/Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib/
 *   FrameBufferSerialPortLib.c
 *
 * ── 为什么必须改这里 ────────────────────────────────────────────────
 * 本平台的 SerialPortLib 就是"把字符画进帧缓冲"的实现（不是真串口）。
 * 于是：
 *   · 所有 DEBUG ()         -> BaseDebugLibSerialPort -> SerialPortWrite -> 本文件
 *   · 所有 ConOut / Print () -> SimpleTextInOutSerial  -> SerialPortWrite -> 本文件
 * 两路都不经过 GOP，也就不经过 SimpleFbDxe 的 RotateFlush。
 * 2026-10-22 真机视频里"苹果/卡片已经横了、文字还是竖着往下跑"就是这个原因：
 * 图形走 Blt（转了），文字直接按物理 (x,y) 写进面板（没转）。
 *
 * ── 改法 ────────────────────────────────────────────────────────────
 * 文本仍然按"一行从左往右、换行往下"的常规排版走，但排版坐标系改成
 * **横屏逻辑坐标系**（宽 = 物理高 2560，高 = 物理宽 1600），逐像素落盘时
 * 用和 SimpleFbDxe 完全相同的映射搬到物理缓冲：
 *
 *     逻辑 (x, y)  ->  物理 (Px, Py) = (PanelW - 1 - y, x)
 *
 * 这样横着拿平板看，日志就是正的、从左往右；和 UniXNU 自己画的界面同向。
 * 若面板本来就是横向的（Width >= Height），自动退化为 1:1 直写，不旋转。
 *
 * ── 没动的东西 ──────────────────────────────────────────────────────
 *   · 物理几何仍然取 PcdFrameBufferWidth/Height = 1600x2560（ABL 交下来的真值），
 *     一个数都没改；
 *   · 字形、行高、换行/清屏/冻结逻辑与上游一致；
 *   · Font.h / FrameBuffer.h / .inf 原样不动，只换这一个 .c。
 */
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/SerialPortLib.h>
#include <Library/TimerLib.h>

#include <Library/FrameBufferHandoff.h>

#include "FrameBuffer.h"
#include "Font.h"

FB_GEOMETRY  mFbGeometry;

//
// 当前生效的几何。mLogW/mLogH 是"文本排版用的逻辑画布"，
// 与 SimpleFbDxe 报给 GOP 的 2560x1600 保持一致。
//
STATIC UINT8  *mFb;
STATIC UINTN   mPitchBytes;
STATIC UINT32  mPanelW;
STATIC UINT32  mPanelH;
STATIC UINT32  mLogW;
STATIC UINT32  mLogH;
STATIC BOOLEAN mRotate;

UINT8
GetFontScale ();

STATIC
VOID
SetupGeometry (
  VOID
  )
{
  mFb         = (UINT8 *)mFbGeometry.Base;
  mPitchBytes = mFbGeometry.PitchBytes;
  mPanelW     = mFbGeometry.Width;
  mPanelH     = mFbGeometry.Height;

  if (mPanelW < mPanelH) {
    // 竖着的物理面板：文本按横屏排，落盘时转 90°
    mRotate = TRUE;
    mLogW   = mPanelH;
    mLogH   = mPanelW;
  } else {
    // 面板本来就是横的：直写，不转
    mRotate = FALSE;
    mLogW   = mPanelW;
    mLogH   = mPanelH;
  }
}

/**
  在逻辑坐标 (Lx, Ly) 写一个像素。

  旋转分支的映射与 SimpleFbDxe:RotateFlush 的映射完全相同：
    逻辑 x -> 物理行  Py = x
    逻辑 y -> 物理列  Px = PanelW - 1 - y
**/
STATIC
VOID
PlotPixel (
  IN UINTN   Lx,
  IN UINTN   Ly,
  IN UINT32  Color
  )
{
  UINT8  *Pixel;

  if ((Lx >= mLogW) || (Ly >= mLogH)) {
    return;
  }

  if (mRotate) {
    Pixel = mFb + Lx * mPitchBytes + ((UINTN)mPanelW - 1 - Ly) * FB_BPP;
  } else {
    Pixel = mFb + Ly * mPitchBytes + Lx * FB_BPP;
  }

  if (FB_BPP == 4) {
    *(UINT32 *)Pixel = Color;
  } else {
    CopyMem (Pixel, &Color, FB_BPP);
  }
}

/**
  画一个字形。逻辑左上角 (Lx, Ly)，每个点阵像素铺成 FontScale x FontScale。

  字形位图与上游一致：UINT64 里高 32 位是前 6 行、低 32 位是后 6 行，
  每行 FONT_WIDTH(5) 位、**最低位是最左边那一点**，一共 12 行（FONT_HEIGHT-4）。
  背景色也要画（黑色），这样旧内容会被覆盖掉，和上游行为一致。
**/
STATIC
VOID
DrawGlyph (
  IN UINT64  Glyph,
  IN UINT32  FontScale,
  IN UINTN   Lx,
  IN UINTN   Ly
  )
{
  UINT32  TopGlyph;
  UINT32  BottomGlyph;
  UINT32  RowData;
  UINTN   X;
  UINTN   Y;
  UINT8   Fx;
  UINT8   Fy;

  TopGlyph    = (UINT32)(Glyph >> 32);
  BottomGlyph = (UINT32)Glyph;
  RowData     = TopGlyph;

  Y = Ly;
  for (Fy = 0; Fy < 12; Fy++) {
    if (Fy == 6) {
      RowData = BottomGlyph;
    }

    X = Lx;
    for (Fx = 0; Fx < FONT_WIDTH; Fx++) {
      UINT32 Color = ((RowData >> Fx) & 1) ? 0xFFFFFFFF : 0x00000000;
      UINT8  Dx;
      UINT8  Dy;

      for (Dy = 0; Dy < FontScale; Dy++) {
        for (Dx = 0; Dx < FontScale; Dx++) {
          PlotPixel (X + Dx, Y + Dy, Color);
        }
      }

      X += FontScale;
    }

    RowData >>= FONT_WIDTH;
    Y       += FontScale;
  }
}

/**
  清掉逻辑行带 [Ly0, Ly1)。

  非旋转分支就是一行行 ZeroMem；旋转分支下"一个逻辑行带"对应物理上
  **每一条物理行里的一段列**，所以要逐物理行清。
**/
STATIC
VOID
ClearBand (
  IN UINTN  Ly0,
  IN UINTN  Ly1
  )
{
  UINTN  Py;
  UINTN  Px0;
  UINTN  Bytes;

  if (Ly1 > mLogH) {
    Ly1 = mLogH;
  }

  if (Ly0 >= Ly1) {
    return;
  }

  if (!mRotate) {
    ZeroMem (mFb + Ly0 * mPitchBytes, (Ly1 - Ly0) * mPitchBytes);
    return;
  }

  // 逻辑 y 越大 -> 物理 Px 越小
  Px0   = (UINTN)mPanelW - Ly1;
  Bytes = (Ly1 - Ly0) * FB_BPP;

  for (Py = 0; Py < mLogW; Py++) {
    ZeroMem (mFb + Py * mPitchBytes + Px0 * FB_BPP, Bytes);
  }
}

VOID
AdvanceNewLine (
  IN EFI_PHYSICAL_ADDRESS       FbBase,
  IN UINT64                     FbLength,
  IN EFI_FRAME_BUFFER_POSITION *CurrentPosition,
  IN EFI_FRAME_BUFFER_POSITION  MaxPosition
  )
{
  UINT8  FontScale;
  UINTN  CellHeight;
  UINT32 NextYPos;

  // Wait
  MicroSecondDelay (FB_DELAY_US);

  // Update Position
  CurrentPosition->XPos  = 0;
  CurrentPosition->YPos += 1;

  // Stop at the Bottom when the oldest Lines must stay readable
  if (CurrentPosition->YPos >= MaxPosition.YPos && FB_FREEZE) {
    CurrentPosition->YPos = FB_FROZEN_YPOS;
    return;
  }

  // Wrap to the Top, keeping the previous Screen visible
  if (CurrentPosition->YPos >= MaxPosition.YPos) {
    CurrentPosition->YPos = 0;
  }

  // Clear the Line about to be written and the next one, a blank Separator
  // between the newest and the oldest visible Lines
  FontScale  = GetFontScale ();
  CellHeight = (UINTN)(FONT_HEIGHT - 4) * FontScale;
  NextYPos   = (CurrentPosition->YPos + 1 < MaxPosition.YPos) ? CurrentPosition->YPos + 1 : 0;

  ClearBand ((UINTN)CurrentPosition->YPos * CellHeight, (UINTN)CurrentPosition->YPos * CellHeight + CellHeight);
  ClearBand ((UINTN)NextYPos * CellHeight, (UINTN)NextYPos * CellHeight + CellHeight);
}

UINT8
GetFontScale ()
{
  // Use the Platform Scale when set
  if (FB_SCALE != 0) {
    return FB_SCALE;
  }

  // Get Smaller Display Dimension
  UINT32 ShorterDimension = (FB_WIDTH < FB_HEIGHT) ? FB_WIDTH : FB_HEIGHT;

  // Return Calculated Font Scale
  return (ShorterDimension < 426) ? 1 : (UINT8)(ShorterDimension / 426);
}

VOID
GetFbPositions (
  IN  EFI_PHYSICAL_ADDRESS        FbBase,
  IN  UINT64                      FbLength,
  IN  UINT8                       FontScale,
  OUT EFI_FRAME_BUFFER_POSITION **CurrentPosition,
  OUT EFI_FRAME_BUFFER_POSITION  *MaxPosition
  )
{
  EFI_FRAME_BUFFER_POSITION LocalMaxPosition;

  // Get Current Position
  *CurrentPosition = (EFI_FRAME_BUFFER_POSITION *)(FbBase + FbLength - sizeof (EFI_FRAME_BUFFER_POSITION));

  // Calculate Max Positions（横屏排版的网格：宽 2560 / 高 1600）
  LocalMaxPosition.XPos = mLogW / ((FONT_WIDTH  + 1) * FontScale);
  LocalMaxPosition.YPos = mLogH / ((FONT_HEIGHT - 4) * FontScale);

  if (LocalMaxPosition.XPos == 0) {
    LocalMaxPosition.XPos = 1;
  }

  if (LocalMaxPosition.YPos == 0) {
    LocalMaxPosition.YPos = 1;
  }

  // 这块内存不在 SimpleFbDxe 清屏范围内（它只清 1600*2560*4），
  // 首次开机里面是垃圾，位置可能是天文数字 —— 越界就回左上角。
  if ((*CurrentPosition)->YPos != FB_FROZEN_YPOS) {
    if ((*CurrentPosition)->YPos >= LocalMaxPosition.YPos) {
      (*CurrentPosition)->YPos = 0;
    }

    if ((*CurrentPosition)->XPos >= LocalMaxPosition.XPos) {
      (*CurrentPosition)->XPos = 0;
    }
  }

  // Pass Max Position
  *MaxPosition = LocalMaxPosition;
}

VOID
WriteFrameBuffer (
  IN EFI_PHYSICAL_ADDRESS FbBase,
  IN UINT64               FbLength,
  IN UINT8                Character
  )
{
  EFI_FRAME_BUFFER_POSITION *CurrentPosition;
  EFI_FRAME_BUFFER_POSITION  MaxPosition;
  UINT8                      GlyphFontScale;
  UINTN                      GlyphWidth;
  UINTN                      GlyphHeight;

  // Set Glyph Font
  STATIC CONST UINT64 GlyphFont[] = GLYPH_FONT;

  // Set Glyph Font Scale
  GlyphFontScale = GetFontScale ();

  // Get Frame Buffer Positions
  GetFbPositions (FbBase, FbLength, GlyphFontScale, &CurrentPosition, &MaxPosition);

  // Ignore everything after the Log froze
  if (CurrentPosition->YPos == FB_FROZEN_YPOS) {
    return;
  }

  // Skip Invalid ACSII Characters
  if (Character >= 127) {
    return;
  }

  // Handle Control Characters
  if (Character < 32) {
    if (Character == '\f') {
      // Form feed: clear the whole screen and start again at the top left, so a
      // message that matters (the hand-off to a kernel) fills the screen.
      ZeroMem ((VOID *)mFb, mPitchBytes * mPanelH);
      CurrentPosition->XPos = 0;
      CurrentPosition->YPos = 0;
    } else if (Character == '\n') {
      AdvanceNewLine (FbBase, FbLength, CurrentPosition, MaxPosition);
    } else if (Character == '\r') {
      CurrentPosition->XPos = 0;
    }

    return;
  }

  // Skip leading Spaces
  if (CurrentPosition->XPos == 0 && Character == ' ') {
    return;
  }

  // Translate ACSII Character
  UINT64 Glyph = GlyphFont[Character - 32];

  // Calculate Glyph Dimension
  GlyphWidth  = (FONT_WIDTH  + 1) * GlyphFontScale;
  GlyphHeight = (FONT_HEIGHT - 4) * GlyphFontScale;

  // Draw Glyph（逻辑坐标 = 横屏排版坐标）
  DrawGlyph (
    Glyph,
    GlyphFontScale,
    (UINTN)CurrentPosition->XPos * GlyphWidth,
    (UINTN)CurrentPosition->YPos * GlyphHeight
    );

  // Increase X Position
  CurrentPosition->XPos++;

  // Check Max X Position
  if (CurrentPosition->XPos >= MaxPosition.XPos) {
    AdvanceNewLine (FbBase, FbLength, CurrentPosition, MaxPosition);
  }
}

EFI_STATUS
GetFrameBufferMemory (
  OUT EFI_PHYSICAL_ADDRESS *Base,
  OUT UINT64               *Length
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR FbRegion;

  ZeroMem (&FbRegion, sizeof (FbRegion));

  // Locate "Display Reserved" Memory Region
  LocateMemoryRegionByName ("Display Reserved", &FbRegion);
  LocateMemoryRegionByName ("Display_Reserved", &FbRegion);
  if (!FbRegion.Address && !FbRegion.Length) {
    return EFI_NOT_FOUND;
  }

  // Pass Frame Buffer Memory
  *Base   = FbRegion.Address;
  *Length = FbRegion.Length;

  return EFI_SUCCESS;
}

STATIC
VOID
LoadGeometry (
  IN EFI_PHYSICAL_ADDRESS  FbBase,
  IN UINT64                FbLength
  )
{
  CONST volatile FB_HANDOFF *Handoff;

  // Build-time Default
  mFbGeometry.Base       = (UINTN)FbBase;
  mFbGeometry.Width      = FixedPcdGet32 (PcdFrameBufferWidth);
  mFbGeometry.Height     = FixedPcdGet32 (PcdFrameBufferHeight);
  mFbGeometry.PitchBytes = mFbGeometry.Width * FB_BPP;

  // Handed-off Geometry, valid only once its Magic is set
  Handoff = (CONST volatile FB_HANDOFF *)(UINTN)(FbBase + FbLength - FB_HANDOFF_OFFSET);
  if (Handoff->Magic == FB_HANDOFF_MAGIC) {
    mFbGeometry.Base       = (UINTN)Handoff->Base;
    mFbGeometry.Width      = Handoff->Width;
    mFbGeometry.Height     = Handoff->Height;
    mFbGeometry.PitchBytes = Handoff->PitchBytes;
  }

  if ((mFbGeometry.PitchBytes == 0) || (mFbGeometry.Width == 0) || (mFbGeometry.Height == 0)) {
    // 兜底：交下来的几何缺字段就退回物理真值
    mFbGeometry.Base       = (UINTN)FbBase;
    mFbGeometry.Width      = FixedPcdGet32 (PcdFrameBufferWidth);
    mFbGeometry.Height     = FixedPcdGet32 (PcdFrameBufferHeight);
    mFbGeometry.PitchBytes = mFbGeometry.Width * FB_BPP;
  }
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8 *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS FbBase;
  UINT64               FbLength;
  BOOLEAN              InterruptState;

  // Get Frame Buffer Memory
  Status = GetFrameBufferMemory (&FbBase, &FbLength);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  // Use the Geometry of the Frame Buffer that is really scanned out
  LoadGeometry (FbBase, FbLength);
  SetupGeometry ();

  // Get Interrupts State
  InterruptState = ArmGetInterruptState ();

  // Disable Interrupts
  if (InterruptState) {
    ArmDisableInterrupts ();
  }

  // Write Debug Message to Frame Buffer
  for (UINTN i = 0; i < NumberOfBytes; i++) {
    WriteFrameBuffer (FbBase, FbLength, *Buffer++);
  }

  // Enable Interrupts
  if (InterruptState) {
    ArmEnableInterrupts ();
  }

  // Flush Frame Buffer
  WriteBackInvalidateDataCacheRange ((VOID *)mFb, mPitchBytes * mPanelH);

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8 *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  return 0;
}

BOOLEAN
EFIAPI
SerialPortPoll ()
{
  return FALSE;
}

EFI_STATUS
EFIAPI
SerialPortSetControl (IN UINT32 Control)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
SerialPortGetControl (OUT UINT32 *Control)
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64             *BaudRate,
  IN OUT UINT32             *ReceiveFifoDepth,
  IN OUT UINT32             *Timeout,
  IN OUT EFI_PARITY_TYPE    *Parity,
  IN OUT UINT8              *DataBits,
  IN OUT EFI_STOP_BITS_TYPE *StopBits
  )
{
  return EFI_UNSUPPORTED;
}

//
// 打包脚本靠这个串判断"旋转版到底编进 .efi 了没有"。
//
// 注意：上一版写成函数内 STATIC CONST 数组 + 局部 volatile 指针，结果
// CLANGPDB 的 LTO 把"只被赋给一个再没人读的局部变量"的数组整个删掉，
// 编出来的 .efi 里一个字节都搜不到（2026-10-22 实测：BdsDxe.efi 里
// 有新版代码字符串、却没有这个标记串；构建命令行也确实带 -flto 和
// -Wno-unused-const-variable）。这里改成真实存在的全局 volatile 对象 +
// used 属性 + 运行期 volatile 读，三重保证它一定落在 .data 里。
//
// 注意用小写 volatile：本文件没 include <Base.h>，EDK2 的 VOLATILE 宏不可用
// （2026-10-22 第一次改完就栽在这上面：error: unknown type name 'VOLATILE'）。
//
#if defined (__GNUC__) || defined (__clang__)
__attribute__ ((used))
#endif
volatile CHAR8  mDbyW09FbTextRotTag[] = "DBY_W09-FBTEXT-ROT90";

EFI_STATUS
EFIAPI
SerialPortInitialize ()
{
  //
  // 一次真实的 volatile 读：编译器不能把这个对象当死代码删掉，
  // 也不能把比较结果常量折叠。条件恒不成立，逻辑不变。
  //
  if (mDbyW09FbTextRotTag[0] != 'D') {
    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}
