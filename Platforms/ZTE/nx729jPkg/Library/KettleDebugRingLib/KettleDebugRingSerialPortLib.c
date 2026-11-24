/** @file
  SerialPortLib that appends everything written to it into a fixed DRAM ring, so the
  firmware's DEBUG() output survives a reset and can be harvested into logfs.

  Layout at KETTLE_RING_BASE:

    +0x00  magic   'KDBG'
    +0x04  used    bytes of text written
    +0x08  size    text capacity
    +0x0C  flags   bit0 = full (further writes dropped)
    +0x40  text

  Linear, not circular, on purpose: a wrapped ring loses the ordering that makes the
  early boot output (memory map, CPU init) readable. When the buffer fills the tail is
  dropped and the full flag is set, keeping the front matter that matters most.

  Copyright (c) 2026, Kettle. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/SerialPortLib.h>

//
// 0x87000000 is the 1 MiB immediately above the ramoops carveout (0x86E00000..0x87000000),
// inside the free window between SMEM (ends 0x81F00000) and the PIL reserved region
// (starts 0x8A800000). MemoryMapLib describes it as reserved system memory, which is what
// makes it mapped and readable by the time KettleLogDxe runs -- the same reason ramoops
// needed a descriptor there.
//
#define KETTLE_RING_BASE   0x87000000ULL
#define KETTLE_RING_SIZE   0x00100000U
#define KETTLE_RING_DATA   0x00000040U
#define KETTLE_RING_MAGIC  SIGNATURE_32 ('K', 'D', 'B', 'G')
#define KETTLE_RING_FULL   0x00000001U

typedef struct {
  UINT32    Magic;
  UINT32    Used;
  UINT32    Size;
  UINT32    Flags;
} KETTLE_RING_HEADER;

STATIC
volatile KETTLE_RING_HEADER *
RingHeader (
  VOID
  )
{
  return (volatile KETTLE_RING_HEADER *)(UINTN)KETTLE_RING_BASE;
}

STATIC
UINT8 *
RingText (
  VOID
  )
{
  return (UINT8 *)(UINTN)(KETTLE_RING_BASE + KETTLE_RING_DATA);
}

STATIC
UINT32
RingCapacity (
  VOID
  )
{
  return KETTLE_RING_SIZE - KETTLE_RING_DATA;
}

/**
  Initialize the ring.

  Existing content is preserved: if the magic already matches, this is either a previous
  boot that died before its text was harvested, or this boot continuing. Either way the
  text is worth keeping, so only a garbage window is re-initialised.
**/
UINTN
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  volatile KETTLE_RING_HEADER  *Header;

  Header = RingHeader ();
  if (Header->Magic != KETTLE_RING_MAGIC) {
    Header->Size  = RingCapacity ();
    Header->Used  = 0;
    Header->Flags = 0;
    Header->Magic = KETTLE_RING_MAGIC;
  }

  return RETURN_SUCCESS;
}

/**
  Append debug text to the ring.

  Reporting the full count back even when truncating is intentional: BaseDebugLibSerialPort
  would otherwise believe its output was short-written and could loop or pad, and firmware
  diagnostics must never stall the boot.
**/
UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  volatile KETTLE_RING_HEADER  *Header;
  UINTN                        Space;
  UINTN                        Count;

  if ((Buffer == NULL) || (NumberOfBytes == 0)) {
    return 0;
  }

  Header = RingHeader ();
  if (Header->Magic != KETTLE_RING_MAGIC) {
    SerialPortInitialize ();
  }

  //
  // Re-check Size: the window is DRAM and a previous owner of this address may have
  // left a magic that matched with a nonsense size.
  //
  if (Header->Size != RingCapacity ()) {
    Header->Size = RingCapacity ();
  }

  if (Header->Used >= RingCapacity ()) {
    Header->Flags |= KETTLE_RING_FULL;
    return NumberOfBytes;
  }

  Space = RingCapacity () - Header->Used;
  Count = (NumberOfBytes < Space) ? NumberOfBytes : Space;
  CopyMem (RingText () + Header->Used, Buffer, Count);
  Header->Used += (UINT32)Count;
  if (Count < NumberOfBytes) {
    Header->Flags |= KETTLE_RING_FULL;
  }

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  return 0;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return FALSE;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  return RETURN_UNSUPPORTED;
}
