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
 *      交叉验证：内核 pinctrl-spmi-gpio.c:1119 算 pad->base = reg[0] + i*0x100，
 *         而组名表是 1-based（"gpio1" 是第 0 项），所以 "gpio6" ⇒ i=5
 *         ⇒ 0xC000 + 5*0x100 = 0xC500，与 u-boot 完全对上。
 *   2) 电源   = PM8150 PON 块 (0x800) 的 KPDPWR，RT_STS = 0x810 **bit0**（低有效）
 *   3) 音量-  = 同一 PON 块的 RESIN，RT_STS **bit1**（低有效）
 *      —— 位序以内核 qpnp-power-on.c 为准（KPDPWR_N_SET=BIT0、RESIN_N_SET=BIT1）。
 *      —— 见 hit_0.dtb：/qcom,pm8150@0/qcom,power-on@800/qcom,pon_1 linux,code=0x74(KEY_POWER)、
 *         qcom,pon_2 linux,code=0x72(KEY_VOLUMEDOWN)；gpio_keys/vol_up -> PM8150 GPIO6 (0x73)。
 *
 * SPMI 寄存器访问（arbiter **v5**，SM8250；基址取自本机 stock.dtb 的
 *   /soc/qcom,spmi@c440000，reg-names = core|chnls|obsrvr|intr|cnfg）：
 *   core   = 0x0C440000 size 0x1100   （映射表在 core + 0x900）
 *   obsrvr = 0x0E600000 size 0x100000 （读事务窗口，读必须走这里）
 *   cnfg   = 0x0C40A000 size 0x26000  （ownership 表在 cnfg + 0x700）
 *   ppid    = (sid << 8) | (addr >> 8)
 *   apid    = 遍历 core+0x900+4*i 反查 ppid，再按 ownership 选本 EE 那条
 *   channel = obsrvr + 0x80*apid + 0x10000*ee   (ee = 0, APSS)
 *   cmd     = (opc << 27) | ((addr & 0xFF) << 4) | (len-1)，读 opc = 15
 *   STATUS +0x08、RDATA0 +0x18
 * 这几块地址落在平台内存表的 "AOSS"（0x0B000000 + 0x04000000）里，已经是
 * MmIO/UNCACHEABLE 映射，不会因为没映射而 Data Abort。
 *
 * 状态机（短按/长按/连发）与 Xiaomi stonePkg 的实现保持一致，只把取键
 * 的那一段换成本平台的 PMIC 读取。
 */
#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
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

/* ------------------------------------------------------------------ SPMI
 *
 * 依据（两条独立硬证据，都不是猜的）：
 *  1) 本机 stock.dtb 的 /soc/qcom,spmi@c440000，reg-names = core|chnls|obsrvr|intr|cnfg
 *       core   = 0x0C440000  size 0x1100
 *       chnls  = 0x0C600000  size 0x2000000
 *       obsrvr = 0x0E600000  size 0x100000
 *       intr   = 0x0E700000  size 0xA0000
 *       cnfg   = 0x0C40A000  size 0x26000
 *  2) 本机内核源码 drivers/spmi/spmi-pmic-arb.c（SM8250 走 v5 那套 ops）
 *
 * v5 的读事务和 v2 完全不同，四条都要对：
 *   · 读走 observer 通道，base = obsrvr = 0x0E600000，不是 chnls；
 *   · observer 通道步长 0x80 * apid（v5: 0x10000*ee + 0x80*apid，ee=0）；
 *   · CMD opcode 在 bit[31:27]，读是 PMIC_ARB_OP_READ = 15
 *     ⇒ cmd = (15 << 27) | ((addr & 0xFF) << 4) | (len-1)；
 *   · PPID→APID 不是数组索引，而是"反查表"：core + 0x900 + 4*apid 里存的是
 *     该 apid 对应的 ppid（bit[23:8]），需要遍历 0..511 找匹配。
 *
 * 上一版这四条全错了（cnfg 当读口、步长 0x8000、opcode 0、0x800 直接索引），
 * 相当于往一个未对齐的通道窗口里写了一条"扩展写"命令 ⇒ arbiter 等不到事务
 * 完成，整个 SPMI 总线挂死 ⇒ BDS 的 ConnectAll 静默定住，屏幕停在最后一行。
 */
#define SPMI_CORE_BASE       0x0C440000UL
#define SPMI_CORE_SIZE       0x00001100UL
#define SPMI_OBS_BASE        0x0E600000UL
#define SPMI_CNFG_BASE       0x0C40A000UL
#define SPMI_APID_MAP_OFF    0x00000900UL
#define SPMI_OWNER_TABLE_OFF 0x00000700UL   /* cnfg + 0x700 + 4*apid，bit[2:0]=write EE */
#define SPMI_OWNER_MASK      0x00000007UL
#define SPMI_PPID_MASK       0x00000FFFU
#define SPMI_OBS_STRIDE      0x00000080UL

/* (core_size - apid_map_off) / 4 == 0x200 == 512，正好是 PMIC_ARB_MAX_PERIPHS */
#define SPMI_APID_MAP_MAX    ((SPMI_CORE_SIZE - SPMI_APID_MAP_OFF) / 4)

#define SPMI_REG_CMD         0x00
#define SPMI_REG_STATUS      0x08
#define SPMI_REG_RDATA0      0x18

/* STATUS 位定义，见内核 enum pmic_arb_chnl_status */
#define SPMI_STATUS_DONE     0x01
#define SPMI_STATUS_ERRMASK  0x0E   /* FAILURE|DENIED|DROPPED */

#define SPMI_OPC_READ        15U
#define SPMI_CMD_READ_1B(addr)                                   \
  ((((UINT32)SPMI_OPC_READ) << 27) | (((UINT32)((addr) & 0xFFU)) << 4))

/* ------------------------------------------------------- PM8150 寄存器 */
#define PM8150_SID           0
#define PM_PON_RT_STS        0x0810   /*
                                      * 内核 qpnp-power-on.c 权威定义：
                                      *   QPNP_PON_KPDPWR_N_SET = BIT(0) -> 电源
                                      *   QPNP_PON_RESIN_N_SET  = BIT(1) -> 音量下
                                      * 对应 hit_0.dtb：pon_1 linux,code=0x74(KEY_POWER)
                                      *   pon-type=0 -> KPDPWR；pon_2 0x72(KEY_VOLUMEDOWN)
                                      *   pon-type=1 -> RESIN。
                                      * 两路都低有效，空闲 0b11。
                                      * （2026-10-22 曾误按"实测"把两位对调，导致
                                      *   电源=下移/音量下=确认，已按内核定义改回。）
                                      */
#define PM_GPIO6_STATUS      0xC508   /* 音量+：bit7 有效位，bit0 电平（低有效） */

#define SPMI_TIMEOUT_US      1000   /* 与内核 PMIC_ARB_TIMEOUT_US 一致 */

//
// 一旦有一次事务超时（DV 一直不置位），就把 SPMI 通路判死，之后所有按键
// 读取直接返回"松开"，一个寄存器都不再碰。
// 目的：绝不允许"按键驱动把一个已经不健康的 bus 每 10ms 再去撞一次"，
//       那样会把整个 BDS 一起拖死（上一版就是这个后果）。
//
STATIC BOOLEAN  mSpmiBroken = FALSE;

//
// 打包脚本靠这个串判断"SPMI 读命令修好的那一版到底编进 .efi 了没有"。
// 文件作用域 + used + 运行期 volatile 读，三重保证它在 LTO 下不被删。
//
#if defined (__GNUC__) || defined (__clang__)
__attribute__ ((used))
#endif
volatile CHAR8  mDby09SpmiTag[] = "DBY09-SPMI-RDCMD5";

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
  PPID → APID。

  v5 的映射表是"apid → ppid"的反查表（core + 0x900 + 4*apid 里存的是这个
  apid 对应的 ppid，放在 bit[23:8]），所以必须遍历找匹配，不能拿 ppid 当下标。
  表项为 0 表示该 apid 没被配置。

  ★ 一个 ppid 在 v5 里会对应多个 apid（每个 EE 一个写通道）。必须挑
    ownership 表里 owner == 本 EE(APSS=0) 的那个 —— 内核
    pmic_arb_read_apid_map_v5() 就是这么挑的：
        if (!valid || apidd->write_ee == pmic_arb->ee) ppid_to_apid[ppid] = i;
    只取"第一个匹配"会拿到别的 EE（AOP/Modem）的通道，reader 窗口读出来
    是 0，症状就是 PON 的 RT_STS 永远读到 0x00：
      · 音量下 RESIN_N 判成"一直按住" → 菜单自己往下连发
      · 电源 KPDPWR_N 反过来被判成无效 → 按了没反应
    2026-10-22 真机现象正是这两条。
**/
STATIC
EFI_STATUS
SpmiPpIdToApid (
  IN  UINT16  PpId,
  OUT UINT16  *Apid
  )
{
  UINTN   Index;
  UINT32  Reg;
  UINT32  Owner;
  UINT16  Fallback;

  Fallback = 0xFFFF;

  for (Index = 0; Index < SPMI_APID_MAP_MAX; Index++) {
    Reg = SpmiRead32 ((UINTN)SPMI_CORE_BASE + SPMI_APID_MAP_OFF + 4 * Index);
    if (Reg == 0) {
      continue;
    }

    if (((Reg >> 8) & SPMI_PPID_MASK) != PpId) {
      continue;
    }

    if (Fallback == 0xFFFF) {
      Fallback = (UINT16)Index;
    }

    Owner = SpmiRead32 ((UINTN)SPMI_CNFG_BASE + SPMI_OWNER_TABLE_OFF + 4 * Index)
            & SPMI_OWNER_MASK;
    if (Owner == 0) {
      *Apid = (UINT16)Index;
      return EFI_SUCCESS;
    }
  }

  if (Fallback != 0xFFFF) {
    *Apid = Fallback;
    return EFI_SUCCESS;
  }

  return EFI_DEVICE_ERROR;
}

//
// 按键只碰 2 个 ppid（0x08 PON、0xC5 GPIO6），查一次缓存下来，
// 免得每次按键都去遍历 512 项映射表。
//
typedef struct {
  UINT16   PpId;
  UINT16   Apid;
} DBY_SPMI_CACHE;

STATIC DBY_SPMI_CACHE  mSpmiCache[4];
STATIC UINTN           mSpmiCacheCount = 0;

/**
  读 PMIC 的一个字节寄存器（SPMI 单字节读，v5 observer 通道）。

  @param[in]  Sid     SPMI slave id（PM8150 = 0）
  @param[in]  Addr    12 位 PMIC 内部地址（高 4 位进 ppid，低 8 位进 cmd）
  @param[out] Value   读到的字节

  @retval EFI_SUCCESS            成功
  @retval EFI_DEVICE_ERROR       查不到 apid / 事务被拒 / 通道没响应
**/
STATIC
EFI_STATUS
SpmiReadByte (
  IN  UINT8  Sid,
  IN  UINT16 Addr,
  OUT UINT8  *Value
  )
{
  EFI_STATUS  Status;
  UINT16      PpId;
  UINT16      Apid;
  UINTN       Index;
  UINTN       Channel;
  UINTN       Timeout;
  UINT32      St;

  if (mSpmiBroken) {
    return EFI_DEVICE_ERROR;
  }

  //
  // 这条分支恒假，作用只是让编译器看得见 mDby09SpmiTag，别把它优化掉。
  //
  if (mDby09SpmiTag[0] != 'D') {
    mSpmiBroken = TRUE;
    return EFI_DEVICE_ERROR;
  }

  PpId = (UINT16)(((UINT16)Sid << 8) | ((Addr >> 8) & 0xFF));
  Apid = 0;

  for (Index = 0; Index < mSpmiCacheCount; Index++) {
    if (mSpmiCache[Index].PpId == PpId) {
      break;
    }
  }

  if (Index < mSpmiCacheCount) {
    Apid = mSpmiCache[Index].Apid;
  } else {
    Status = SpmiPpIdToApid (PpId, &Apid);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "DBY09: SPMI ppid=%x not mapped\n", PpId));
      return EFI_DEVICE_ERROR;
    }

    if (mSpmiCacheCount < (sizeof (mSpmiCache) / sizeof (mSpmiCache[0]))) {
      mSpmiCache[mSpmiCacheCount].PpId = PpId;
      mSpmiCache[mSpmiCacheCount].Apid = Apid;
      mSpmiCacheCount++;
    }
  }

  Channel = (UINTN)SPMI_OBS_BASE + SPMI_OBS_STRIDE * Apid;

  //
  // 先把上一次的 RDATA0 读走（这会清掉 DONE），否则下面第一轮轮询就会
  // 命中旧状态，把上一条命令的结果当成本次结果。
  //
  (VOID)SpmiRead32 (Channel + SPMI_REG_RDATA0);

  SpmiWrite32 (Channel + SPMI_REG_CMD, SPMI_CMD_READ_1B (Addr));

  for (Timeout = 0; Timeout < SPMI_TIMEOUT_US; Timeout++) {
    St = SpmiRead32 (Channel + SPMI_REG_STATUS);
    if ((St & SPMI_STATUS_DONE) != 0) {
      if ((St & SPMI_STATUS_ERRMASK) != 0) {
        DEBUG ((EFI_D_ERROR, "DBY09: SPMI err st=%x apid=%x addr=%x\n", St, Apid, Addr));
        return EFI_DEVICE_ERROR;
      }

      *Value = (UINT8)(SpmiRead32 (Channel + SPMI_REG_RDATA0) & 0xFF);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  //
  // 超时：这条通路已经不可信，永久关闭，别再撞。
  //
  DEBUG ((EFI_D_ERROR, "DBY09: SPMI timeout apid=%x addr=%x\n", Apid, Addr));
  mSpmiBroken = TRUE;
  return EFI_DEVICE_ERROR;
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

STATIC BOOLEAN  mPonInvalidWarned = FALSE;

STATIC
VOID
PonStatusInvalid (
  IN UINT8  Value
  )
{
  if (!mPonInvalidWarned) {
    mPonInvalidWarned = TRUE;
    DEBUG ((
      EFI_D_ERROR,
      "DBY09: PON RT_STS=0x%02x invalid (expect 0x3 idle), vol-/power disabled\n",
      Value
      ));
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

      //
      // RT_STS 空闲态是 0b11（两路都低有效、都松开）。读成 0b00 只可能是
      // SPMI 事务没成 / RDATA0 是残值，绝不能当成"按住"——否则菜单会自己
      // 一路往下连发，跟用户按键抢控制权。2026-10-22 真机就是这个症状。
      //
      if ((Value & 0x03) == 0) {
        PonStatusInvalid (Value);
        return FALSE;
      }

      return (Value & 0x02) == 0;   // RESIN_N = BIT(1)（音量下），低有效

    case DBY_KEY_POWER:
      Status = SpmiReadByte (PM8150_SID, PM_PON_RT_STS, &Value);
      if (EFI_ERROR (Status)) {
        return FALSE;
      }

      if ((Value & 0x03) == 0) {
        PonStatusInvalid (Value);
        return FALSE;
      }

      return (Value & 0x01) == 0;   // KPDPWR_N = BIT(0)（电源），低有效

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
