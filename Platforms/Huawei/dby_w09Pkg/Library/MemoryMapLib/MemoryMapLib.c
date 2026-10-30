#include <Library/MemoryMapLib.h>

//
// DBY-W09 (Huawei MatePad 11, SM8250 / kona, 6GiB) 内存描述表
//
// 来源：edk2-msm (Renegade) Platform/Oneplus/sm8250/Library/instantnoodlep/
//       PlatformMemoryMapLib/PlatformMemoryMapLib.c —— 该表是**唯一在 DBY-W09
//       真机上实测跑通的布局**（显示、UFS 均正常），故原样继承其地址，
//       仅把结构体 / 函数名换成 Mu-Silicium 的接口：
//         ARM_MEMORY_REGION_DESCRIPTOR_EX + GetPlatformMemoryMap()
//           -> EFI_MEMORY_REGION_DESCRIPTOR + GetMemoryMap()
//       字段顺序 (Name/Address/Length/HobOption/ResourceType/ResourceAttribute
//       /MemoryType/ArmAttributes) 两边完全一致，枚举前 4 项同名同序，可直接搬。
//
// 与 9707f (Lenovo Y700) 表的关键区别：
//   * UEFI FD  0xCE000000 / 32MiB   （Y700 是 0x9FC00000 / 3MiB；0xCE000000 已在
//                                    DBY-W09 实机验证，0x9FC00000 未验证）
//   * DXE Heap 0xC0000000 / 224MiB  （同上，实测区）
//   * Display Reserved 0x9C000000 / 36MiB 两表一致 —— 这是 XBL 固定保留的
//                                    framebuffer 区，SimpleFbDxe 靠它出画面
//
STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"Hypervisor",        0x80000000, 0x00600000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, NS_DEVICE},
  {"HLOS 1",            0x80600000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"AOP",               0x80700000, 0x00160000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"AOP CMD DB",        0x80860000, 0x00020000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"XBL Log Buffer",    0x80880000, 0x00014000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"HLOS 2",            0x80894000, 0x0006C000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"SMEM",              0x80900000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Removed Mem",       0x80B00000, 0x05700000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, NS_DEVICE},
  {"PIL Reserved",      0x86200000, 0x05D00000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED_XN},
  {"HLOS 3",            0x8BF00000, 0x10100000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Display Reserved",  0x9C000000, 0x02400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DBI Dump",          0x9E400000, 0x00F00000, NoHob,  MMAP_IO, INITIALIZED, Conv,   UNCACHED_UNBUFFERED_XN},
  {"HLOS 4",            0x9F300000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"SEC Heap",          0x9FF00000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"CPU Vectors",       0x9FF8C000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"MMU PageTables",    0x9FF8D000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI Stack",        0x9FF90000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"HLOS 5",            0x9FFD0000, 0x00027000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Log Buffer",        0x9FFF7000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"Info Blk",          0x9FFFF000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},

  {"HLOS 6",            0xA0000000, 0x10000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  //
  // PSTORE 的长度原表是 0x00500000，会越过 HLOS 7 的起点 0xB0400000 共 1MiB。
  // Renegade 那版不校验，所以能过去；Silicium 的 MemoryInitPeiLib:43 ValidateMemoryMap
  // 会按地址排序后逐个比 EndAddress > Next->Address，一重叠就 CpuDeadLoop（真机已实测卡在
  // "PSTORE" and "HLOS 7" Overlap!）。这里收到 0x00400000，正好与 HLOS 7 首地址接上。
  //
  {"PSTORE",            0xB0000000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"HLOS 7",            0xB0400000, 0x0C800000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  {"DXE Heap",          0xC0000000, 0x0E000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"UEFI FD",           0xCE000000, 0x02000000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},

  //
  // 0xD0000000 起 0x130000000（到 0x200000000）。
  // 数值与 Renegade 表**逐字节相同** —— 这一版就是在 DBY-W09 真机上跑通过
  // 显示与 Shell 的那一版，不在这里做任何"优化"。若日后确认本机是 6GiB，
  // 再收到 0x180000000（0x0B0000000）也不迟，但必须先有真机 RAM 布局证据。
  //
  {"RAM Partition",     0xD0000000, 0x130000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  // Other Memory Regions
  {"IMEM Base",         0x14680000, 0x00040000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
  {"IMEM Cookie Base",  0x146BF000, 0x00001000, AddDev, MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},

  // Register Regions
  {"IPC_ROUTER_TOP",    0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"SECURITY CONTROL",  0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"QUPV3_2_GSI",       0x00800000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"QUPV3_0_GSI",       0x00900000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"QUPV3_1_GSI",       0x00A00000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PRNG_CFG_PRNG",     0x00790000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"CRYPTO0 CRYPTO",    0x01DC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TCSR_TCSR_REGS",    0x01FC0000, 0x00030000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GPU_GMU_CX_BLK",    0x02C7D000, 0x00002000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GPU_CC",            0x02C90000, 0x0000A000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"QUPV3_SSC_GSI",     0x05A00000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PERIPH_SS",         0x08800000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MCCC_MCCC_MSTR",    0x090BA000, 0x00006000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"USB30_PRIM",        0x0A600000, 0x0011B000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"USB_RUMI",          0x0A720000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"USB30_SEC",         0x0A800000, 0x0011B000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"AOSS",              0x0B000000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TLMM_WEST",         0x0F100000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TLMM_SOUTH",        0x0F500000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"TLMM_NORTH",        0x0F900000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"SMMU",              0x15000000, 0x000D0000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"APSS_HM",           0x17800000, 0x0D981000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount
  )
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
