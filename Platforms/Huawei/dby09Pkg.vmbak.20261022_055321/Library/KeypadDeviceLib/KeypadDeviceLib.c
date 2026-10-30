/**
 * KeypadDeviceLib.c —— DBY-W09 (Huawei MatePad 11, SM8250) 平台按键设备层
 *
 * 这是 SiliciumPkg/Drivers/KeypadDeviceDxe 需要的"平台实现"：
 *   KeypadDeviceDxe 安装 gKeypadDeviceProtocolGuid + KeypadDevicePath（VenHw
 *   D7F58A0E-…），KeypadDxe 再绑上去装 SimpleTextIn；
 *   PlatformBootManagerLib 早就把 KeypadDevicePath 写进了 ConIn 白名单，
 *   所以只要这一层存在，三个实体键就真的能当方向键/回车用。
 *
 * 硬件来源（全部是实测过的，不是猜的）：
 *   1) 音量+  = PM8150 (SPMI slave 0) GPIO6，寄存器块 0xC500，STATUS = 0xC508
 *      —— 见用户提供的 u-boot 驱动 button-dby-pmic-gpio-commented.c：
 *         bit7 必须是 1（GPIO 已使能、状态有效），bit0 = 电平，
 *         外部上拉，所以 0 = 按下、1 = 松开。ABL 已经把它配成数字输入，
 *         这里只读、不配置。
 *   2) 电源   = PM8150 PON 块 (0x800) 的 KPDPWR，RT_STS = 0x810 bit0（低有效）
 *   3) 音量-  = 同一 PON 块的 RESIN，RT_STS bit1（低有效）
 *      —— 见 hit_0.dtb：/qcom,pm8150@0/qcom,power-on@800/qcom,pon_1 linux,code=0x74(KEY_POWER)、
 *         qcom,pon_2 linux,code=0x72(KEY_VOLUMEDOWN)；gpio_keys/vol_up -> PM8150 GPIO6 (0x73)。
 *
 * SPMI 寄存器访问（arbiter v2，基址取自本机 stock.dtb 的
 *   /soc/qcom,spmi@c440000：chnls=0x0C600000 size 0x2000000、cnfg=0x0C40A000）：
 *   ppid   = (sid << 8) | (addr >> 8)
 *   apid   = read32(cnfg + 0x800 + 4*ppid) & 0xFF     (0xFF = 本 EE 没有这个外设)
 *   channel= chnls + 0x8000*apid + 0x1000*ee          (ee = 0, APSS)
 *   cmd    = (opc << 27) | ((addr & 0xFF) << 4) | (len-1)，读操作 opc = 0
 *   STATUS +0x08、RDATA0 +0x18
 * 这两块地址落在平台内存表的 "AOSS"（0x0B000000 + 0x04000000）里，已经是
 * MmIO/UNCACHEABLE 映射，不会因为没映射而 Data Abort。
 *
 * 状态机（短按/长按/连发）与 Xiaomi stonePkg 的实现保持一致，只把取键
 * 的那一段换成本平台的 PMIC 读取。
 */
#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/TimerLib.h>

//
// SCAN_xx / CHAR_CARRIAGE_RETURN 正规定义在
// MdePkg/Include/Uefi/UefiInternalFormRepresentation.h 里，是否被 Uefi.h 链式
// 带进来取决于 MdePkg 版本。这里兜一层 #ifndef：编进去的值与 MdePkg 完全一致
// （SCAN_UP=0x01 … SCAN_ESC=0x17，CR=0x0D），带进来过就一个都不生效。
//
#ifndef SCAN_UP
#define SCAN_UP               0x0001
#endif
#ifndef SCAN_DOWN
#define SCAN_DOWN             0x0002
#endif
#ifndef SCAN_RIGHT
#define SCAN_RIGHT            0x0003
#endif
#ifndef SCAN_LEFT
#define SCAN_LEFT             0x0004
#endif
#ifndef SCAN_ESC
#define SCAN_ESC              0x0017
#endif
#ifndef CHAR_CARRIAGE_RETURN
#define CHAR_CARRIAGE_RETURN  0x000D
#endif

/* ------------------------------------------------------------------ SPMI */
#define SPMI_ARB_CHNLS_BASE  0x0C600000UL
#define SPMI_ARB_CNFG_BASE   0x0C40A000UL
#define SPMI_APID_OFFSET     0x00000800UL
#define SPMI_CHNL_STRIDE     0x00008000UL
#define SPMI_EE_STRIDE       0x00001000UL
#define SPMI_REG_CMD         0x00
#define SPMI_REG_STATUS      0x08
#define SPMI_REG_RDATA0      0x18

/* ------------------------------------------------------- PM8150 寄存器 */
#define PM8150_SID           0
#define PM_PON_RT_STS        0x0810   /* bit0 KPDPWR_N, bit1 RESIN_N（低有效） */
#define PM_GPIO6_STATUS      0xC508   /* 音量+：bit7 有效位，bit0 电平（低有效） */

#define SPMI_TIMEOUT_US      200

STATIC
UINT32
SpmiRead32 (
  IN UINTN  Address
  )
{
  return *(volatile UINT32 *)Address;
}

STATIC
VOID
SpmiWrite32 (
  IN UINTN   Address,
  IN UINT32  Value
  )
{
  *(volatile UINT32 *)Address = Value;
}

/**
  读 PMIC 的一个字节寄存器（SPMI 单字节读）。

  @param[in]  Sid     SPMI slave id（PM8150 = 0）
  @param[in]  Addr    12 位 PMIC 内部地址（高 4 位进 ppid，低 8 位进 cmd）
  @param[out] Value   读到的字节

  @retval EFI_SUCCESS            成功
  @retval EFI_DEVICE_ERROR       该外设不归 APSS 所有 / 通道没响应
**/
STATIC
EFI_STATUS
SpmiReadByte (
  IN  UINT8  Sid,
  IN  UINT16 Addr,
  OUT UINT8  *Value
  )
{
  UINTN   PpId;
  UINT32  Apid;
  UINTN   Channel;
  UINTN   Timeout;

  PpId    = ((UINTN)Sid << 8) | ((Addr >> 8) & 0xFF);
  Apid    = SpmiRead32 ((UINTN)SPMI_ARB_CNFG_BASE + SPMI_APID_OFFSET + 4 * PpId) & 0xFF;

  if (Apid == 0xFF) {
    return EFI_DEVICE_ERROR;
  }

  Channel = (UINTN)SPMI_ARB_CHNLS_BASE + SPMI_CHNL_STRIDE * Apid;

  SpmiWrite32 (Channel + SPMI_REG_CMD, (UINT32)((Addr & 0xFF) << 4));

  //
  // 等事务完成：不同 arbiter 版本 STATUS 的 DV 位语义不完全一致，
  // 所以这里既轮询 bit0，也保证至少等够一个事务的时间，再取 RDATA0。
  //
  for (Timeout = 0; Timeout < SPMI_TIMEOUT_US; Timeout++) {
    if ((SpmiRead32 (Channel + SPMI_REG_STATUS) & 0x1) != 0) {
      break;
    }

    MicroSecondDelay (1);
  }

  MicroSecondDelay (20);

  *Value = (UINT8)(SpmiRead32 (Channel + SPMI_REG_RDATA0) & 0xFF);
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------ 键位映射 */
typedef enum {
  DBY_KEY_VOLUME_UP,
  DBY_KEY_VOLUME_DOWN,
  DBY_KEY_POWER,
  DBY_KEY_COUNT
} DBY_KEY_ID;

STATIC KEY_CONTEXT  mKeyContext[DBY_KEY_COUNT];
STATIC BOOLEAN      mKeysReady = FALSE;

//
// 自己实现位图，不依赖 BitmapLib（SiliciumPkg.dsc.inc 只映射了 TimerLib，
// 没映射 BitmapLib，引进来会在构建期报 "Library class BitmapLib not found"）。
// 0x18 个 scancode / 0x7f 个 unicode，各留 4 个 UINT32 足够。
//
STATIC UINT32  mScanCodeBits[4] = { 0 };
STATIC UINT32  mUnicodeBits[4]  = { 0 };

STATIC
VOID
BitSet (
  IN UINT32 *Bits,
  IN UINT32  Index
  )
{
  if (Index < 128) {
    Bits[Index >> 5] |= (UINT32)(1U << (Index & 31));
  }
}

STATIC
VOID
BitClear (
  IN UINT32 *Bits,
  IN UINT32  Index
  )
{
  if (Index < 128) {
    Bits[Index >> 5] &= (UINT32)~(1U << (Index & 31));
  }
}

STATIC
BOOLEAN
BitTest (
  IN UINT32 *Bits,
  IN UINT32  Index
  )
{
  if (Index >= 128) {
    return FALSE;
  }

  return (Bits[Index >> 5] & (UINT32)(1U << (Index & 31))) != 0;
}

EFI_KEY_DATA gKeyDataPowerDown      = { .Key = { .ScanCode = SCAN_RIGHT } };
EFI_KEY_DATA gKeyDataPowerUp        = { .Key = { .ScanCode = SCAN_LEFT } };
EFI_KEY_DATA gKeyDataPowerLongpress = { .Key = { .ScanCode = SCAN_ESC } };

#define MS2NS(ms)  (((UINT64)(ms)) * 1000000ULL)

STATIC
VOID
KeySetState (
  IN UINT16  ScanCode,
  IN CHAR16  UnicodeChar,
  IN BOOLEAN Value
  )
{
  if ((ScanCode != 0) && (ScanCode < 0x18)) {
    if (Value) {
      BitSet (mScanCodeBits, ScanCode);
    } else {
      BitClear (mScanCodeBits, ScanCode);
    }
  }

  if ((UnicodeChar != 0) && (UnicodeChar < 0x7f)) {
    if (Value) {
      BitSet (mUnicodeBits, UnicodeChar);
    } else {
      BitClear (mUnicodeBits, UnicodeChar);
    }
  }
}

STATIC
BOOLEAN
KeyGetState (
  IN UINT16  ScanCode,
  IN CHAR16  UnicodeChar
  )
{
  if ((ScanCode != 0) && (ScanCode < 0x18)) {
    if (!BitTest (mScanCodeBits, ScanCode)) {
      return FALSE;
    }
  }

  if ((UnicodeChar != 0) && (UnicodeChar < 0x7f)) {
    if (!BitTest (mUnicodeBits, UnicodeChar)) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
VOID
KeyContextInit (
  IN KEY_CONTEXT *Context
  )
{
  SetMem (&Context->KeyData, sizeof (Context->KeyData), 0);
  Context->Time      = 0;
  Context->State     = KEYSTATE_RELEASED;
  Context->Repeat    = FALSE;
  Context->Longpress = FALSE;
}

STATIC
VOID
KeyUpdateStatus (
  IN KEY_CONTEXT       *Context,
  IN KEYPAD_RETURN_API *KeypadReturnApi,
  IN BOOLEAN            IsPressed,
  IN UINT64             Delta
  )
{
  KeySetState (Context->KeyData.Key.ScanCode, Context->KeyData.Key.UnicodeChar, IsPressed);
  Context->Time += Delta;

  switch (Context->State) {
    case KEYSTATE_RELEASED:
      if (IsPressed) {
        Context->Time  = 0;
        Context->State = KEYSTATE_PRESSED;
      }

      break;

    case KEYSTATE_PRESSED:
      if (IsPressed) {
        if (Context->Repeat && (Context->Time >= MS2NS (100))) {
          KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Context->KeyData);
          Context->Time   = 0;
          Context->Repeat = TRUE;
        } else if (!Context->Longpress && (Context->Time >= MS2NS (500))) {
          if (Context->KeyData.Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            if (KeyGetState (SCAN_DOWN, 0)) {
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &gKeyDataPowerDown);
            } else if (KeyGetState (SCAN_UP, 0)) {
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &gKeyDataPowerUp);
            } else {
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &gKeyDataPowerLongpress);
            }
          } else {
            if (!KeyGetState (0, CHAR_CARRIAGE_RETURN)) {
              KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Context->KeyData);
              Context->Time   = 0;
              Context->Repeat = TRUE;
            }
          }

          Context->Longpress = TRUE;
        }
      } else {
        if (!Context->Longpress) {
          KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &Context->KeyData);
          Context->State = KEYSTATE_LONGPRESS_RELEASE;
        } else if (Context->Time >= MS2NS (10)) {
          Context->Time      = 0;
          Context->Repeat    = FALSE;
          Context->Longpress = FALSE;
          Context->State     = KEYSTATE_RELEASED;
        }
      }

      break;

    case KEYSTATE_LONGPRESS_RELEASE:
      Context->Time      = 0;
      Context->Repeat    = FALSE;
      Context->Longpress = FALSE;
      Context->State     = KEYSTATE_RELEASED;
      break;

    default:
      break;
  }
}

/**
  读一个键的当前状态。读失败一律当成"松开"，绝不让一次 SPMI 故障
  把按键卡死在按下态（那样菜单会自己一直动）。
**/
STATIC
BOOLEAN
KeyIsPressed (
  IN DBY_KEY_ID  Key
  )
{
  EFI_STATUS  Status;
  UINT8       Value = 0;

  switch (Key) {
    case DBY_KEY_VOLUME_UP:
      Status = SpmiReadByte (PM8150_SID, PM_GPIO6_STATUS, &Value);
      if (EFI_ERROR (Status)) {
        return FALSE;
      }

      if ((Value & 0x80) == 0) {
        // bit7 = 0 表示 GPIO 还没使能/状态无效，此时 bit0 没有意义
        return FALSE;
      }

      return (Value & 0x01) == 0;

    case DBY_KEY_VOLUME_DOWN:
      Status = SpmiReadByte (PM8150_SID, PM_PON_RT_STS, &Value);
      if (EFI_ERROR (Status)) {
        return FALSE;
      }

      return (Value & 0x02) == 0;   // RESIN_N 低有效

    case DBY_KEY_POWER:
      Status = SpmiReadByte (PM8150_SID, PM_PON_RT_STS, &Value);
      if (EFI_ERROR (Status)) {
        return FALSE;
      }

      //
      // RT_STS 的 bit0/bit1 是低有效，空闲态就是 0b11。真机上不可能两路同时
      // 按下，所以读成 0b00 只可能是 SPMI 事务没成、RDATA0 是残值（0x00）。
      // 这种情况一律当松开——否则菜单会像"按钮自己动"那样一路乱跳。
      //
      if ((Value & 0x03) == 0) {
        return FALSE;
      }

      return (Value & 0x01) == 0;   // KPDPWR_N 低有效

    default:
      return FALSE;
  }
}

/* ------------------------------------------------------- 协议实现 */
EFI_STATUS
EFIAPI
KeypadDeviceReset (
  IN KEYPAD_DEVICE_PROTOCOL *This
  )
{
  KeyContextInit (&mKeyContext[DBY_KEY_VOLUME_UP]);
  mKeyContext[DBY_KEY_VOLUME_UP].KeyData.Key.ScanCode = SCAN_UP;

  KeyContextInit (&mKeyContext[DBY_KEY_VOLUME_DOWN]);
  mKeyContext[DBY_KEY_VOLUME_DOWN].KeyData.Key.ScanCode = SCAN_DOWN;

  KeyContextInit (&mKeyContext[DBY_KEY_POWER]);
  mKeyContext[DBY_KEY_POWER].KeyData.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;

  mKeysReady = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
KeypadDeviceGetKeys (
  IN KEYPAD_DEVICE_PROTOCOL *This,
  IN KEYPAD_RETURN_API      *KeypadReturnApi,
  IN UINT64                  Delta
  )
{
  DBY_KEY_ID  Key;

  if (KeypadReturnApi == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!mKeysReady) {
    KeypadDeviceReset (This);
  }

  for (Key = DBY_KEY_VOLUME_UP; Key < DBY_KEY_COUNT; Key++) {
    KeyUpdateStatus (&mKeyContext[Key], KeypadReturnApi, KeyIsPressed (Key), Delta);
  }

  return EFI_SUCCESS;
}
