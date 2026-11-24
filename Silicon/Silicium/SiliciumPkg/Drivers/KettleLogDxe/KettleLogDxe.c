/** @file
  KettleLogDxe - boot diagnostics that survive the boot.

  A phone with no serial port and no working display gives no feedback at all: a driver
  that fails looks exactly like a driver that was never dispatched. This module writes
  what it can observe into the `logfs` partition, which can then be read back over EDL
  without any button presses on the device.

  Two reports are written, because the interesting question is a before/after pair:
    * at driver entry, before BDS has dispatched anything;
    * at ReadyToBoot, once every driver whose depex was satisfied has run.
  Comparing them distinguishes "DisplayDxe never dispatched" from "DisplayDxe ran and
  failed", which is the difference between a dependency problem and a driver problem.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Guid/GlobalVariable.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PartitionInfo.h>

//
// GUIDs DisplayDxe ANDs together in its depex. Two of them are produced by nothing in
// this firmware tree, so a listing of which are present answers "why is it not running".
// Keep this list in step with Binaries/<device>/.../DisplayDxe.depex.
//
STATIC EFI_GUID mDisplayDeps[] = {
  { 0x157A5C45, 0x21B2, 0x43C5, { 0xBA, 0x7C, 0x82, 0x2F, 0xEE, 0x5F, 0xE5, 0x99 } },
  { 0xB0760469, 0x970C, 0x487A, { 0xA4, 0xB5, 0x28, 0xDB, 0x7B, 0x45, 0xCE, 0xF1 } },
  { 0x5BEF7C41, 0xA560, 0x435B, { 0x83, 0x5B, 0x3C, 0x3E, 0xBF, 0x2B, 0xD1, 0x51 } },
  { 0xC1BFF82D, 0x6C92, 0x48FB, { 0x83, 0x55, 0xC7, 0xFB, 0x42, 0x18, 0xA4, 0xEA } },
  { 0xA966CC4F, 0xEE6C, 0x424D, { 0xA7, 0x62, 0x8E, 0xD3, 0xB5, 0xA4, 0xA5, 0x01 } },
  { 0x47861DFF, 0x556F, 0x4269, { 0x9B, 0x28, 0x06, 0x2E, 0xC3, 0xEC, 0xB3, 0x0D } },
  { 0xAD9AEC18, 0x7BF0, 0x4809, { 0x9E, 0x96, 0x30, 0x12, 0x30, 0x9F, 0x3D, 0xF7 } },
  { 0xB27AE8B1, 0x3E10, 0x4D07, { 0xAB, 0x5C, 0xEB, 0x9A, 0x6D, 0xC6, 0xFA, 0x8F } },
  { 0x241AFAE6, 0x885F, 0x4F6C, { 0xA7, 0xEA, 0xC2, 0x8E, 0xAB, 0x79, 0xC3, 0xE5 } },
};

STATIC CONST CHAR16 *mDisplayDepNames[] = {
  L"L1 (pmic?)", L"L2", L"L3", L"L4", L"L5 (UNPRODUCED)",
  L"L6 (UNPRODUCED)", L"L7 i2c?", L"L8 i2c?", L"L9 clock?",
};

STATIC EFI_BLOCK_IO_PROTOCOL  *mLogIo;
STATIC UINTN                   mLogPartitionBytes;
STATIC EFI_EVENT               mRetryTimer;
STATIC BOOLEAN                 mStorageNoted;
//
// Once logfs is writable, keep sampling briefly: DisplayDxe installing GOP can happen
// well after storage comes up, and the exact second it does is the answer to whether the
// display path works at all.
//
STATIC BOOLEAN                 mGopSeen;
STATIC CHAR8                  *mBuf;
STATIC UINTN                   mBufUsed;
STATIC UINTN                   mBufCap;
//
// Fine-grained liveness. The first real logfs report showed only the entry record and
// then nothing, which is consistent with the firmware dying within a second but does not
// say when. A 100 ms tick turns "died somewhere under a second" into a count.
//
STATIC UINTN                   mTicks;
STATIC BOOLEAN                 mReachedReadyToBoot;
//
// ramoops carveout (see Platform/ZTE/nx729jPkg MemoryMapLib "Ramoops").
//
// A kernel that dies before UFS initialises leaves its console only in DRAM, and DRAM
// cannot be read from EDL -- the firehose loader rejects Peek. Mu runs before the kernel
// on the next boot and can read that physical address directly, so the firmware that
// boots Linux also retrieves the previous attempt's body. Written to logfs, which
// survives power cycles. Console-only ramoops puts the zone at offset 0, so no zone
// arithmetic is needed.
//
#define KETTLE_RAMOOPS_BASE   ((UINTN)0x86E00000)
#define KETTLE_RAMOOPS_SIZE   0x00200000
#define RAMOOPS_MAX_RUNS      120
#define RAMOOPS_RUN_MAX       180
STATIC BOOLEAN                 mRamoopsDumped;
STATIC BOOLEAN                 mLogCleared;
//
// BootManager writes how far each boot got into this NVRAM variable, precisely so the
// next boot can report it. It only ever reached DEBUG and the panel, neither of which is
// readable here -- so the one signal that distinguishes "UEFI never handed off" from
// "the loader took the machine down" was being thrown away. Reading it at entry reports
// the PREVIOUS boot's ending, which is the interesting one: a stage left at BOOTING means
// control passed to the loader and never came back.
//
STATIC EFI_GUID mKettleSettingsGuid = {
  0x2a7d61f4, 0x8c35, 0x4b92, { 0x9e, 0x14, 0x6d, 0x08, 0x53, 0xab, 0x7f, 0x21 }
};

#define KETTLE_STAGE_IDLE       0
#define KETTLE_STAGE_CONSOLE    1
#define KETTLE_STAGE_ESP_FOUND  2
#define KETTLE_STAGE_BOOTING    3
#define KETTLE_STAGE_RETURNED   4

//
// Stamp the build so a logfs dump identifies the firmware that wrote it. The partition is
// never cleared between boots, so without this a stale report is indistinguishable from a
// fresh one -- which is exactly the confusion that wasted the last few boots.
//
STATIC CONST CHAR8 mBuildStamp[] = "build: " __DATE__ " " __TIME__ "\r\n";

//
// The platform's memory map reserves a 32 KiB "Log Buffer" at this address. Writing the
// report there as well means the diagnostics survive even when UFS never initialises --
// which is one of the things we are trying to find out. It can be read back with
// `edl.py peek 0xA7605000 0x8000 out.bin`.
//
#define KETTLE_LOG_RAM_BASE   ((UINT8 *)0xA7605000)
#define KETTLE_LOG_RAM_SIZE   0x8000
STATIC CONST CHAR8 mLogRamMagic[] = "KTLLOG1\n";
//
// Before the first real report, blank a fixed span at the start of logfs.
//
// LogFlush rewrites from offset 0 and never shortens the partition, so a shorter report
// leaves the previous boot's tail in place. The result reads as one continuous log: a
// dump appeared to run to t=210s while claiming ReadyToBoot was never reached, because
// the heartbeat lines were left over from an earlier session. Anything parsed from such
// a record can be attributed to the wrong boot. 64 KiB is twice the largest buffer this
// driver can build, so clearing it bounds what a reader can see to this boot alone.
//
#define KETTLE_LOG_CLEAR_BYTES  (64 * 1024)

/**
  Blank the head of logfs once, so no previous boot's text survives into this report.
**/
STATIC
VOID
ClearLogOnce (
  VOID
  );

/**
  Locate the GPT partition named "logfs" and take its block I/O.
**/
STATIC
EFI_STATUS
FindLogPartition (
  VOID
  )
{
  EFI_STATUS                      Status;
  EFI_HANDLE                      *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_PARTITION_INFO_PROTOCOL     *Info;
  EFI_BLOCK_IO_PROTOCOL           *BlockIo;

  if (mLogIo != NULL) {
    return EFI_SUCCESS;
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPartitionInfoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (EFI_ERROR (Status)) {
      continue;
    }
    if (Info->Type != PARTITION_TYPE_GPT) {
      continue;
    }
    if (StrCmp (Info->Info.Gpt.PartitionName, L"logfs") != 0) {
      continue;
    }

    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo->Media == NULL) || !BlockIo->Media->MediaPresent) {
      continue;
    }

    mLogIo             = BlockIo;
    mLogPartitionBytes = (UINTN)(BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
    //
    // First moment storage exists: drop the previous boot's trailing text now, before
    // any report is flushed over it.
    //
    ClearLogOnce ();
    FreePool (Handles);
    return EFI_SUCCESS;
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

/**
  Blank the head of logfs once, so no previous boot's text survives into this report.

  Called only after the block I/O exists, because there is nothing to clear without it.
**/
STATIC
VOID
ClearLogOnce (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       BlockSize;
  UINTN       Blocks;
  VOID       *Zeros;

  if (mLogCleared || (mLogIo == NULL) || (mLogIo->Media == NULL)) {
    return;
  }

  mLogCleared = TRUE;

  BlockSize = mLogIo->Media->BlockSize;
  if (BlockSize == 0) {
    return;
  }

  Blocks = KETTLE_LOG_CLEAR_BYTES / BlockSize;
  if (Blocks == 0) {
    Blocks = 1;
  }
  if ((Blocks * BlockSize) > mLogPartitionBytes) {
    Blocks = mLogPartitionBytes / BlockSize;
  }

  Zeros  = AllocateZeroPool (Blocks * BlockSize);
  if (Zeros == NULL) {
    return;
  }

  //
  // Best effort: a failure here only costs the old tail, not this boot's report.
  //
  Status = mLogIo->WriteBlocks (mLogIo, mLogIo->Media->MediaId, 0,
                                Blocks * BlockSize, Zeros);
  FreePool (Zeros);

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "KettleLog: logfs clear failed: %r\n", Status));
  }
}

STATIC
EFI_STATUS
LogFlush (
  VOID
  );

/**
  Append formatted ASCII to the in-memory log buffer, flushing whole blocks as it fills.
**/
STATIC
VOID
LogPrint (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  UINTN    Room;
  UINTN    Written;

  if (mBuf == NULL) {
    return;
  }

  Room = mBufCap - mBufUsed;
  if (Room < 2) {
    return;
  }

  VA_START (Marker, Format);
  Written = AsciiVSPrint ((CHAR8 *)mBuf + mBufUsed, Room, Format, Marker);
  VA_END (Marker);

  //
  // AsciiVSPrint returns the length it wanted, which can exceed what fitted.
  //
  if (Written >= Room) {
    mBufUsed = mBufCap;
  } else {
    mBufUsed += Written;
  }

  //
  // Persist immediately. A firmware that resets under a second after this driver runs
  // loses anything still sitting in a buffer, so the buffer is treated as write-through:
  // whatever has been logged is already on the partition before the next line is built.
  //
  LogFlush ();
}

/**
  Publish the buffer to RAM and, if UFS came up, to the logfs partition.

  Both are written from offset 0 rather than appended: the report is rebuilt in place, so
  a later flush replaces the earlier one instead of interleaving.
**/
STATIC
EFI_STATUS
LogFlush (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       BlockSize;
  UINTN       Blocks;
  UINTN       Bytes;
  VOID       *Scratch;
  UINTN       RamBytes;

  if (mBufUsed == 0) {
    return EFI_SUCCESS;
  }

  //
  // RAM copy first: it does not depend on any driver having found storage.
  //
  RamBytes = mBufUsed;
  if (RamBytes > KETTLE_LOG_RAM_SIZE - AsciiStrLen (mLogRamMagic) - 16) {
    RamBytes = KETTLE_LOG_RAM_SIZE - AsciiStrLen (mLogRamMagic) - 16;
  }
  CopyMem (KETTLE_LOG_RAM_BASE, mLogRamMagic, AsciiStrLen (mLogRamMagic));
  CopyMem (KETTLE_LOG_RAM_BASE + AsciiStrLen (mLogRamMagic), mBuf, RamBytes);

  if ((mLogIo == NULL) || (mLogIo->Media == NULL) || !mLogIo->Media->MediaPresent) {
    return EFI_SUCCESS;
  }

  BlockSize = mLogIo->Media->BlockSize;
  Bytes     = mBufUsed;
  if (Bytes > mLogPartitionBytes) {
    Bytes = mLogPartitionBytes;
  }

  Blocks  = (Bytes + BlockSize - 1) / BlockSize;
  Scratch = AllocateZeroPool (Blocks * BlockSize);
  if (Scratch == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Scratch, mBuf, Bytes);

  Status = mLogIo->WriteBlocks (mLogIo, mLogIo->Media->MediaId, 0,
                                Blocks * BlockSize, Scratch);
  FreePool (Scratch);
  return Status;
}

/**
  Describe the graphics state: whether a GOP exists at all, and at what resolution.

  A GOP at 1116x2480 is direct proof that a display driver initialised the panel.
**/
STATIC
VOID
DumpGop (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_HANDLE                   *Handles;
  UINTN                         Count;
  UINTN                         Index;

  //
  // Enumerate every GOP rather than taking the first. A report of "0x0 with a null
  // framebuffer" is impossible from SimpleFbDxe, which refuses to install unless the
  // resolution is at least 640x475 -- so if that is what is present, something else
  // installed it, and the count is what distinguishes "SimpleFbDxe failed" (0 handles)
  // from "a stub exists" (1+ handles).
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Count == 0)) {
    LogPrint ("GOP: none (no display driver bound)\r\n");
    return;
  }

  LogPrint ("GOP: %u handle(s)\r\n", (UINT32)Count);

  for (Index = 0; Index < Count; Index++) {
    Gop = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop)) ||
        (Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL))
    {
      LogPrint ("  [%u] handle=%p: no usable mode info\r\n", (UINT32)Index, Handles[Index]);
      continue;
    }

    LogPrint (
      "  [%u] handle=%p %ux%u fmt=%u fb=0x%lx size=0x%lx maxmode=%u\r\n",
      (UINT32)Index,
      Handles[Index],
      Gop->Mode->Info->HorizontalResolution,
      Gop->Mode->Info->VerticalResolution,
      Gop->Mode->Info->PixelFormat,
      (UINT64)(UINTN)Gop->Mode->FrameBufferBase,
      (UINT64)Gop->Mode->FrameBufferSize,
      Gop->Mode->MaxMode
      );
  }

  FreePool (Handles);
}

/**
  Report whether the framebuffer region SimpleFbDxe depends on actually exists.

  SimpleFbDxe's first act is LocateMemoryRegionByName ("Display Reserved"); if that comes
  back empty it returns EFI_NOT_FOUND before installing anything, and the panel stays dark
  with no GOP. Making the same call here names the failure exactly: the region is either in
  the memory map at this point in the boot or it is not.
**/
STATIC
VOID
DumpFramebufferRegion (
  VOID
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Region;
  EFI_STATUS                    Status;

  ZeroMem (&Region, sizeof (Region));
  Status = LocateMemoryRegionByName ("Display Reserved", &Region);
  if (EFI_ERROR (Status) || (Region.Address == 0)) {
    LogPrint ("  screenfb: \"Display Reserved\" NOT FOUND (%r) -- SimpleFbDxe cannot install\r\n",
              Status);
  } else {
    LogPrint ("  screenfb: \"Display Reserved\" at 0x%lx, length 0x%lx\r\n",
              (UINT64)Region.Address, Region.Length);
  }
}

/**
  Report which of DisplayDxe's depex protocols exist right now.
**/
STATIC
VOID
DumpDisplayDeps (
  IN CONST CHAR8  *When
  )
{
  EFI_STATUS  Status;
  UINTN       Index;
  UINTN       Present;
  VOID       *Interface;

  LogPrint ("\r\n--- %a: DisplayDxe depex ---\r\n", When);
  Present = 0;
  for (Index = 0; Index < ARRAY_SIZE (mDisplayDeps); Index++) {
    Status = gBS->LocateProtocol (&mDisplayDeps[Index], NULL, &Interface);
    if (!EFI_ERROR (Status)) {
      Present++;
    }
    LogPrint (
      "  [%u] %a %a\r\n",
      (UINT32)Index,
      EFI_ERROR (Status) ? "absent " : "PRESENT",
      mDisplayDepNames[Index]
      );
  }
  LogPrint ("  => %u of %u present; DisplayDxe dispatches only when all are\r\n",
            (UINT32)Present, (UINT32)ARRAY_SIZE (mDisplayDeps));
}

STATIC
VOID
DumpRamoops (
  VOID
  )
{
  UINT8    *Base;
  UINTN     Offset;
  UINTN     Total;
  UINTN     Printed;
  UINTN     RunStart;
  UINTN     Skip;
  BOOLEAN   InRun;
  BOOLEAN   Printable;

  if (mRamoopsDumped) {
    return;
  }

  mRamoopsDumped = TRUE;
  Base           = (UINT8 *)KETTLE_RAMOOPS_BASE;

  LogPrint ("\r\n--- ramoops 0x%lx (previous kernel console) ---\r\n",
            (UINT64)KETTLE_RAMOOPS_BASE);
  LogPrint ("  head: %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
            Base[0], Base[1], Base[2], Base[3], Base[4], Base[5], Base[6], Base[7]);

  //
  // First pass: count printable runs so only the tail is printed. The console ring is
  // written circularly, so the newest text is not necessarily at the end of the buffer,
  // but taking the last runs still beats truncating at 32 KiB of buffer.
  //
  Total   = 0;
  InRun   = FALSE;
  RunStart = 0;
  for (Offset = 0; Offset < KETTLE_RAMOOPS_SIZE; Offset++) {
    UINT8  Byte = Base[Offset];

    Printable = (Byte >= 0x20 && Byte < 0x7F) || Byte == '\r' || Byte == '\n' || Byte == '\t';
    if (Printable && !InRun) {
      InRun    = TRUE;
      RunStart = Offset;
    } else if (!Printable && InRun) {
      InRun = FALSE;
      if ((Offset - RunStart) >= 16) {
        Total++;
      }
    }
  }
  if (InRun && ((KETTLE_RAMOOPS_SIZE - RunStart) >= 16)) {
    Total++;
  }

  LogPrint ("  %u printable run(s) of >=16 bytes\r\n", (UINT32)Total);
  if (Total == 0) {
    LogPrint ("  (empty: the kernel never wrote a console record)\r\n");
    LogPrint ("--- end ramoops ---\r\n");
    LogFlush ();
    return;
  }

  Skip = (Total > RAMOOPS_MAX_RUNS) ? (Total - RAMOOPS_MAX_RUNS) : 0;
  LogPrint ("  showing the last %u run(s)\r\n", (UINT32)(Total - Skip));

  //
  // Second pass: emit. Newlines are flattened to spaces so one kernel line stays one
  // logfs line, which keeps the dump readable when pulled out over EDL.
  //
  Printed = 0;
  InRun   = FALSE;
  RunStart = 0;
  for (Offset = 0; Offset <= KETTLE_RAMOOPS_SIZE; Offset++) {
    Printable = FALSE;
    if (Offset < KETTLE_RAMOOPS_SIZE) {
      UINT8  Byte = Base[Offset];

      Printable = (Byte >= 0x20 && Byte < 0x7F) || Byte == '\r' || Byte == '\n' || Byte == '\t';
    }

    if (Printable && !InRun) {
      InRun    = TRUE;
      RunStart = Offset;
    } else if (!Printable && InRun) {
      UINTN  Length;

      InRun  = FALSE;
      Length = Offset - RunStart;
      if (Length >= 16) {
        if (Printed >= Skip) {
          CHAR8  Tmp[RAMOOPS_RUN_MAX + 1];
          UINTN  Index;
          UINTN  Take;

          Take = (Length > RAMOOPS_RUN_MAX) ? RAMOOPS_RUN_MAX : Length;
          for (Index = 0; Index < Take; Index++) {
            UINT8  Ch = Base[RunStart + Index];

            Tmp[Index] = (Ch == '\r' || Ch == '\n' || Ch == '\t') ? ' ' : (CHAR8)Ch;
          }
          Tmp[Take] = 0;
          LogPrint ("  | %a\r\n", Tmp);
        }
        Printed++;
      }
    }
  }

  LogPrint ("--- end ramoops ---\r\n");
  LogFlush ();
}

/**
  Report the boot stage BootManager recorded, and say what it implies.

  Stages: 0 idle, 1 after console, 2 Kettle ESP found, 3 handing off to loader,
  4 loader returned. A stage of 3 means UEFI handed control to the loader and the
  machine went down with it -- the kernel path, not UEFI. A stage of 2 or lower means
  UEFI never reached the handoff, so no kernel was ever involved.
**/
STATIC
VOID
DumpBootStage (
  IN CONST CHAR8  *When
  )
{
  EFI_STATUS  Status;
  UINT32      Stage;
  UINTN       Size;
  CONST CHAR8 *Name;

  Size   = sizeof (Stage);
  Status = gST->RuntimeServices->GetVariable (
                                     L"KettleBootStage",
                                     &mKettleSettingsGuid,
                                     NULL,
                                     &Size,
                                     &Stage
                                     );
  if (EFI_ERROR (Status) || (Size != sizeof (Stage)) || (Stage > KETTLE_STAGE_RETURNED)) {
    LogPrint ("--- bootstage (%a): unavailable (%r)\r\n", When, Status);
    return;
  }

  switch (Stage) {
    case KETTLE_STAGE_IDLE:      Name = "idle";                    break;
    case KETTLE_STAGE_CONSOLE:   Name = "after console";            break;
    case KETTLE_STAGE_ESP_FOUND: Name = "Kettle ESP found";         break;
    case KETTLE_STAGE_BOOTING:   Name = "handing off to loader";    break;
    case KETTLE_STAGE_RETURNED:  Name = "loader returned";          break;
    default:                     Name = "?";                         break;
  }

  LogPrint ("--- bootstage (%a): %u (%a)\r\n", When, Stage, Name);
  if (Stage == KETTLE_STAGE_BOOTING) {
    LogPrint ("    => UEFI DID hand off; the loader took the machine down\r\n");
  } else if (Stage < KETTLE_STAGE_BOOTING) {
    LogPrint ("    => UEFI never reached the handoff; no kernel was involved\r\n");
  } else if (Stage == KETTLE_STAGE_RETURNED) {
    LogPrint ("    => loader ran and came back (it failed, not the kernel)\r\n");
  }
}

//
// Firmware DEBUG() ring, written by KettleDebugRingSerialPortLib to the reserved 1 MiB at
// 0x87000000 (see that library for the layout and why it is linear rather than circular).
//
#define KETTLE_DBG_BASE   0x87000000ULL
#define KETTLE_DBG_DATA   0x00000040U
#define KETTLE_DBG_CAP    (0x00100000U - KETTLE_DBG_DATA)
#define KETTLE_DBG_MAGIC  SIGNATURE_32 ('K', 'D', 'B', 'G')
#define KETTLE_DBG_LINE   240

//
// How much of the ring has already reached logfs.
//
// Tracking an offset instead of clearing the ring after each dump preserves the whole
// boot's narrative. BDS signals ReadyToBoot once per boot attempt, so clearing there
// discarded every earlier event -- which is exactly how six consecutive "Booting MsTemp"
// lines came to look like the only thing that had ever happened on this boot.
//
STATIC UINT32 mDbgDumped;

/**
  Copy the firmware's DEBUG() text into logfs.

  This is the output Mu draws on the panel in a DEBUG build: memory map, CPU bring-up,
  driver dispatch order, BDS decisions. It was previously unreachable -- compiled out in
  RELEASE and drawn only on the panel in DEBUG -- so it is dumped here, split into lines,
  with non-printable bytes folded to '.' to keep one record line per firmware line.

  The ring is cleared only after the text has reached logfs, so a boot that dies before
  this runs still leaves its text behind for the next boot to report.
**/
STATIC
BOOLEAN
DumpDebugRing (
  IN CONST CHAR8  *When
  )
{
  volatile UINT32  *Header;
  UINT32           Magic;
  UINT32           Used;
  UINT32           Flags;
  UINT8            *Text;
  UINTN            Offset;
  UINTN            LineLen;
  CHAR8            Line[KETTLE_DBG_LINE + 1];

  Header = (volatile UINT32 *)(UINTN)KETTLE_DBG_BASE;
  Magic  = Header[0];
  Used   = Header[1];
  Flags  = Header[3];

  LogPrint ("\r\n--- firmware DEBUG ring (%a) ---\r\n", When);
  if (Magic != KETTLE_DBG_MAGIC) {
    LogPrint ("  (no ring; magic %08x)\r\n", Magic);
    LogPrint ("--- end debug ring ---\r\n");
    LogFlush ();
    return FALSE;
  }

  if (Used > KETTLE_DBG_CAP) {
    Used = KETTLE_DBG_CAP;
  }

  LogPrint (
    "  %u bytes of DEBUG text, %u new%s\r\n",
    Used,
    Used - mDbgDumped,
    ((Flags & 1) != 0) ? " (ring FULL: tail dropped)" : ""
    );

  //
  // A ring left over from a boot that never reached a harvest would otherwise be reported
  // as this boot's output; say so rather than implying it.
  //
  //
  // Never dump backwards: if DRAM lost the header the used count can be lower than what
  // was already reported, and restarting is friendlier than printing garbage.
  //
  if (Used < mDbgDumped) {
    mDbgDumped = 0;
  }

  Text    = (UINT8 *)(UINTN)(KETTLE_DBG_BASE + KETTLE_DBG_DATA);
  Offset  = mDbgDumped;
  LineLen = 0;
  while (Offset < Used) {
    UINT8  Ch = Text[Offset++];

    if ((Ch == '\n') || (Ch == '\r')) {
      if (LineLen > 0) {
        Line[LineLen] = 0;
        LogPrint ("  F| %a\r\n", Line);
        LineLen = 0;
      }
      continue;
    }

    if (Ch == '\t') {
      Ch = ' ';
    }
    if ((Ch < 0x20) || (Ch > 0x7E)) {
      Ch = '.';
    }

    if (LineLen >= KETTLE_DBG_LINE) {
      Line[LineLen] = 0;
      LogPrint ("  F| %a\r\n", Line);
      LineLen = 0;
    }
    Line[LineLen++] = (CHAR8)Ch;
  }

  if (LineLen > 0) {
    Line[LineLen] = 0;
    LogPrint ("  F| %a\r\n", Line);
  }

  LogPrint ("--- end debug ring ---\r\n");
  LogFlush ();

  //
  // Only advance once the text actually reached logfs; otherwise a boot that ran before
  // storage was up would cause this text to be skipped forever.
  //
  if (mLogPartitionBytes != 0) {
    mDbgDumped = Used;
  }
  return TRUE;
}

/**
  Print BDS's actual boot order and what each entry points at.

  The DEBUG ring showed BDS repeatedly booting an option called "MsTemp" and never our
  "Kettle Linux", but not enough to say whether MsTemp is a stray NVRAM entry, a
  temporary option MsBootPolicy builds, or our own entry under the wrong name. The
  variables are the ground truth, and they cost nothing to read.
**/
STATIC
VOID
DumpBootOptions (
  IN CONST CHAR8  *When
  )
{
  STATIC CONST CHAR8  Hex[] = "0123456789ABCDEF";
  EFI_STATUS          Status;
  UINTN               Size;
  UINT16              Order[64];
  UINTN               Count;
  UINTN               Index;
  UINT8               Buffer[512];
  CHAR8               Desc[64];

  LogPrint ("\r\n--- BDS boot options (%a) ---\r\n", When);

  Size   = sizeof (Order);
  Status = gST->RuntimeServices->GetVariable (
                                     L"BootOrder",
                                     &gEfiGlobalVariableGuid,
                                     NULL,
                                     &Size,
                                     Order
                                     );
  if (EFI_ERROR (Status)) {
    LogPrint ("  BootOrder: %r\r\n", Status);
    LogPrint ("--- end boot options ---\r\n");
    LogFlush ();
    return;
  }

  Count = Size / sizeof (UINT16);
  LogPrint ("  BootOrder: %u entr%s\r\n", (UINT32)Count, (Count == 1) ? "y" : "ies");

  for (Index = 0; Index < Count; Index++) {
    CHAR16  Name[9];
    UINTN   BufferSize;
    UINTN   DpLen;
    UINT32  Attributes;
    UINTN   Chars;

    //
    // Build L"Boot%04x" by hand rather than pulling in UefiLib for UnicodeSPrint: this
    // diagnostic must not depend on libraries the driver does not already use.
    //
    Name[0] = 'B';
    Name[1] = 'o';
    Name[2] = 'o';
    Name[3] = 't';
    Name[4] = Hex[(Order[Index] >> 12) & 0xF];
    Name[5] = Hex[(Order[Index] >> 8) & 0xF];
    Name[6] = Hex[(Order[Index] >> 4) & 0xF];
    Name[7] = Hex[Order[Index] & 0xF];
    Name[8] = 0;

    BufferSize = sizeof (Buffer);
    Status     = gST->RuntimeServices->GetVariable (
                                                 Name,
                                                 &gEfiGlobalVariableGuid,
                                                 NULL,
                                                 &BufferSize,
                                                 Buffer
                                                 );
    if (EFI_ERROR (Status)) {
      LogPrint ("  [%u] Boot%04x: %r\r\n", (UINT32)Index, Order[Index], Status);
      continue;
    }

    if (BufferSize < 8) {
      LogPrint ("  [%u] Boot%04x: short (%u bytes)\r\n", (UINT32)Index, Order[Index], (UINT32)BufferSize);
      continue;
    }

    Attributes = *(UINT32 *)&Buffer[0];
    DpLen      = *(UINT16 *)&Buffer[4];

    Chars = 0;
    while (((6 + (Chars + 1) * 2) <= BufferSize) && (Chars < 63)) {
      CHAR16  C = *(CHAR16 *)&Buffer[6 + Chars * 2];

      if (C == 0) {
        break;
      }
      Desc[Chars] = ((C < 0x20) || (C > 0x7E)) ? '?' : (CHAR8)C;
      Chars++;
    }
    Desc[Chars] = 0;

    LogPrint (
      "  [%u] Boot%04x attr=0x%x devpath=%u \"%a\"\r\n",
      (UINT32)Index,
      Order[Index],
      Attributes,
      (UINT32)DpLen,
      Desc
      );
  }

  LogPrint ("--- end boot options ---\r\n");
  LogFlush ();
}

STATIC
VOID
WriteReport (
  IN CONST CHAR8  *When
  )
{
  //
  // UFS may only have come up by now, so retry the lookup at this later point: the
  // difference between "found at entry" and "found at ready-to-boot" is itself the answer
  // to whether storage initialisation is working.
  //
  if (mLogIo == NULL) {
    if (!EFI_ERROR (FindLogPartition ())) {
      LogPrint ("%a: logfs appeared late (%u bytes, block size %u)\r\n",
                When, (UINT32)mLogPartitionBytes, (UINT32)mLogIo->Media->BlockSize);
    }
  }

  LogPrint ("\r\n===== KettleLog: %a =====\r\n", When);
  DumpBootStage (When);
  DumpBootOptions (When);
  DumpDebugRing (When);
  DumpDisplayDeps (When);
  DumpFramebufferRegion ();
  DumpGop ();
  LogPrint ("===== end %a =====\r\n", When);

  //
  // Only once storage exists: a ramoops read is worthless until it can be persisted,
  // and this driver's RAM copy is unreadable from EDL.
  //
  if (mLogIo != NULL) {
    DumpRamoops ();
  }

  LogFlush ();
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  mReachedReadyToBoot = TRUE;
  WriteReport ("ready-to-boot");
}

/**
  Poll for logfs while the boot proceeds.

  KettleLogDxe is dispatched before UFS initialises, so the entry report can only reach
  RAM. This keeps trying until the partition shows up and then persists the report, which
  means the log survives a later hang or reset instead of being lost with memory.
**/
STATIC
VOID
EFIAPI
OnRetryTimer (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  if (mLogIo != NULL) {
    //
    // Storage is up, so the log is persistent. Count ticks at 100 ms: the highest tick in
    // a dump is a lower bound on how long the firmware survived after this driver ran.
    //
    mTicks++;

    if (!mGopSeen) {
      VOID  *Gop = NULL;

      if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, &Gop))) {
        mGopSeen = TRUE;
        LogPrint ("[t=%u.%us] GOP APPEARED\r\n", (UINT32)(mTicks / 10), (UINT32)(mTicks % 10));
        DumpGop ();
      } else if ((mTicks % 10) == 0) {
        //
        // One line per second keeps the partition small but still dates the death.
        //
        LogPrint ("[t=%us] alive, no GOP\r\n", (UINT32)(mTicks / 10));
      }
    } else if ((mTicks % 10) == 0) {
      LogPrint ("[t=%us] alive, GOP up; ready-to-boot=%a\r\n",
                (UINT32)(mTicks / 10), mReachedReadyToBoot ? "yes" : "no");
    }

    return;
  }

  if (EFI_ERROR (FindLogPartition ())) {
    return;
  }

  if (!mStorageNoted) {
    mStorageNoted = TRUE;
    LogPrint ("\r\nlogfs appeared during boot: %u bytes, block size %u\r\n",
              (UINT32)mLogPartitionBytes, (UINT32)mLogIo->Media->BlockSize);
    DumpDisplayDeps ("logfs appeared");
    DumpGop ();
    //
    // Earliest moment the ramoops dump can be made persistent.
    //
    DumpRamoops ();
  } else {
    LogPrint ("logfs still present; refreshing report\r\n");
  }

  LogFlush ();
}

/**
  Entry point.
**/
EFI_STATUS
EFIAPI
KettleLogDriverEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   ReadyEvent;
  UINTN       Index;
  VOID       *Interface;

  mBufCap  = 32 * 1024;
  mBufUsed = 0;
  mBuf     = AllocateZeroPool (mBufCap);
  if (mBuf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Whether storage came up is itself a result worth recording, so a missing logfs is
  // reported rather than treated as a reason to skip logging.
  //
  Status = FindLogPartition ();

  LogPrint ("\r\n===== KettleLog: driver entry =====\r\n");
  LogPrint ("%a", mBuildStamp);
  if (EFI_ERROR (Status)) {
    LogPrint ("logfs: NOT FOUND (%r) -- no UFS block I/O yet\r\n", Status);
  } else {
    LogPrint ("logfs: found, %u bytes, block size %u\r\n",
              (UINT32)mLogPartitionBytes, (UINT32)mLogIo->Media->BlockSize);
  }

  for (Index = 0; Index < ARRAY_SIZE (mDisplayDeps); Index++) {
    if (!EFI_ERROR (gBS->LocateProtocol (&mDisplayDeps[Index], NULL, &Interface))) {
      LogPrint ("  entry: dep %u already present\r\n", (UINT32)Index);
    }
  }
  DumpGop ();
  DumpFramebufferRegion ();
  //
  // At entry this variable still holds the PREVIOUS boot's final stage, because
  // BootManager has not run for this boot yet. That is the value that says whether the
  // last attempt ever handed off to the loader.
  //
  DumpBootStage ("driver entry = previous boot");
  //
  // By now the ring holds this boot's PEI and early-DXE DEBUG output -- the memory map and
  // CPU bring-up. Harvest it before it is cleared, and before anything else fills it.
  //
  DumpDebugRing ("driver entry = PEI + early DXE");
  LogFlush ();

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &ReadyEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "KettleLog: no ReadyToBoot event: %r\n", Status));
  }

  //
  // Retry the storage lookup on a timer: run at callback TPL so the block writes happen
  // outside the timer's TPL_NOTIFY context.
  //
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             OnRetryTimer, NULL, &mRetryTimer);
  if (!EFI_ERROR (Status)) {
    gBS->SetTimer (mRetryTimer, TimerPeriodic, EFI_TIMER_PERIOD_MILLISECONDS (100));
  } else {
    DEBUG ((DEBUG_ERROR, "KettleLog: no retry timer: %r\n", Status));
  }

  return EFI_SUCCESS;
}
