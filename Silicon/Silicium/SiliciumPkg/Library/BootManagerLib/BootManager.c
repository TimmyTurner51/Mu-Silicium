/**
  Copyright (C) DuoWoA authors. All rights reserved.
  Copyright (C) Microsoft Corporation. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
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
#include <Library/DevicePathLib.h>
#include <Library/UefiBootManagerLib.h>

#include <Protocol/SimpleFileSystem.h>
#include <Protocol/HiiFont.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>

#include <Guid/Fdt.h>
#include <Guid/FileInfo.h>

#include <Configuration/BootDevices.h>

#include "BootManager.h"

//
// Global Variables
//
STATIC UINTN XPos  = 0;
STATIC UINTN YPos  = 0;
STATIC UINTN XPos2 = 0;
STATIC UINTN YPos2 = 0;

//
// BDS Combo Message
//
STATIC CONST CHAR8 *ComboMessage = "[Volume Up] Boot Manager";

//
// Second hint, so the settings screen is discoverable rather than folklore.
//
STATIC CONST CHAR8 *SettingsMessage = "[Volume Down] Settings";

//
// Cell size for the hints. Follows the persisted console scale, so the hints
// stay the same size as the boot menu text instead of being pinned to one
// setting.
//
//
// Initialised to the 13x38 default rather than the KETTLE_HINT_CELL_* macros,
// which are defined further down. KettleLoadSettings overwrites both from the
// persisted scale before anything is drawn.
//
STATIC UINTN mKettleHintCellW = 13;
STATIC UINTN mKettleHintCellH = 38;

//
// Console cell size, in pixels. Must match CONSOLE_GLYPH_WIDTH/HEIGHT in
// MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsole.h so the
// hint is the same size as the boot menu text.
//
#define KETTLE_HINT_CELL_WIDTH   13
#define KETTLE_HINT_CELL_HEIGHT  38

STATIC EFI_HII_FONT_PROTOCOL        *mHiiFont = NULL;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop     = NULL;

STATIC
VOID
DrawScaledString (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN UINTN                          X,
  IN UINTN                          Y,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Foreground,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Background,
  IN CHAR16                         *String,
  IN UINTN                          CellWidth,
  IN UINTN                          CellHeight
  );

//
// Persistent settings.
//
// Held in an NVRAM variable so they survive a reset, and refreshed from
// \EFI\kettle\boot.txt on the ESP. That file is the only thing that has to
// change to alter behaviour, which keeps the device drivable without buttons.
//
#define KETTLE_SETTINGS_SIGNATURE  SIGNATURE_32 ('K', 'T', 'L', 'S')
#define KETTLE_SETTINGS_VERSION    1

typedef struct {
  UINT32    Signature;
  UINT32    Version;
  UINT32    ForceMenu;
  UINT32    ConsoleScale;
} KETTLE_SETTINGS;

//
// Console cell size in pixels, matching the table GraphicsConsoleDxe builds its
// text modes from.
//
typedef struct {
  UINT16    Width;
  UINT16    Height;
} KETTLE_CELL_SIZE;

STATIC EFI_GUID mKettleSettingsGuid = {
  0x2a7d61f4, 0x8c35, 0x4b92, { 0x9e, 0x14, 0x6d, 0x08, 0x53, 0xab, 0x7f, 0x21 }
};

STATIC KETTLE_SETTINGS mKettleSettings = { 0 };

//
// Boot stages, written to NVRAM as the boot proceeds.
//
// SteamOS bring-up needs to distinguish "never reached the loader" from
// "loader ran and the kernel hung", and the panel cannot be trusted to say
// which. Because the value survives a reset, the *next* boot can report how
// the previous one ended -- no host tooling, no working display, no keys.
//
#define KETTLE_STAGE_IDLE       0
#define KETTLE_STAGE_CONSOLE    1
#define KETTLE_STAGE_ESP_FOUND  2
#define KETTLE_STAGE_BOOTING    3
#define KETTLE_STAGE_RETURNED   4

STATIC CONST CHAR8 *mKettleStageNames[] = {
  "idle",
  "after console",
  "Kettle ESP found",
  "handing off to loader",
  "loader returned",
};

/**
  Kettle: record how far this boot got.

  @param[in] Stage  One of the KETTLE_STAGE_* values.
**/
STATIC
VOID
KettleSetStage (
  IN UINT32  Stage
  )
{
  EFI_STATUS  Status;

  Status = gRT->SetVariable (
                  L"KettleBootStage",
                  &mKettleSettingsGuid,
                  EFI_VARIABLE_NON_VOLATILE |
                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof (Stage),
                  &Stage
                  );
  DEBUG ((DEBUG_INFO, "%a: stage %d (%r)\n", __FUNCTION__, Stage, Status));
}

/**
  Kettle: report how the previous boot ended, when it did not finish.

  A boot that reached KETTLE_STAGE_BOOTING but never reached
  KETTLE_STAGE_RETURNED means control passed to the loader and did not come
  back -- the kernel path, not UEFI. Anything lower means UEFI never got that
  far.
**/
STATIC
VOID
KettleReportLastStage (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Stage;
  UINTN       Size;

  Size   = sizeof (Stage);
  Status = gRT->GetVariable (
                  L"KettleBootStage",
                  &mKettleSettingsGuid,
                  NULL,
                  &Size,
                  &Stage
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (Stage))) {
    return;
  }

  if ((Stage < KETTLE_STAGE_BOOTING) || (Stage > KETTLE_STAGE_RETURNED)) {
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: previous boot ended at stage %d (%s)\n",
    __FUNCTION__,
    Stage,
    (Stage < ARRAY_SIZE (mKettleStageNames)) ? mKettleStageNames[Stage] : "?"
    ));

  //
  // Only draw when the previous boot failed to come back from the loader,
  // which is the case worth surfacing without a log pull.
  //
  if ((Stage == KETTLE_STAGE_BOOTING) && (mGop != NULL) && (mHiiFont != NULL)) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Text = { 0xFF, 0xC0, 0x40, 0 };
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Bg   = { 0x10, 0x10, 0x14, 0 };

    DrawScaledString (
      mGop,
      24,
      mGop->Mode->Info->VerticalResolution - (mKettleHintCellH * 7),
      &Text,
      &Bg,
      L"Previous boot stopped in the loader",
      mKettleHintCellW,
      mKettleHintCellH
      );
  }
}

/**
  Kettle: should the boot manager be shown without a key press?

  @retval TRUE   \EFI\kettle\boot.txt asked for it, or it was set previously.
  @retval FALSE  Otherwise.
**/
STATIC
BOOLEAN
KettleForceMenu (
  VOID
  )
{
  return (BOOLEAN)((mKettleSettings.Signature == KETTLE_SETTINGS_SIGNATURE) &&
                   (mKettleSettings.ForceMenu != 0));
}


/**
  Kettle: does this volume carry the given file?

  @param[in] Fs    Volume to inspect.
  @param[in] Path  Absolute path, e.g. L"\\EFI\\BOOT\\BOOTAA64.EFI".

  @retval TRUE  The file opens for reading.
  @retval FALSE Otherwise.
**/
STATIC
BOOLEAN
KettleVolumeHasFile (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs,
  IN CHAR16                           *Path
  )
{
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *File;
  BOOLEAN            Found;

  Found = FALSE;

  if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
    return FALSE;
  }

  if (!EFI_ERROR (Root->Open (Root, &File, Path, EFI_FILE_MODE_READ, 0))) {
    File->Close (File);
    Found = TRUE;
  }

  Root->Close (Root);
  return Found;
}

/**
  Kettle: read a small text file from the Kettle ESP.

  Only the first ESP that carries \EFI\BOOT\BOOTAA64.EFI is considered, so an
  unrelated FAT volume cannot supply the settings.

  @param[in]  Path        Absolute path, e.g. L"\\EFI\\kettle\\boot.txt".
  @param[out] Buffer      Destination for the file contents, NUL terminated.
  @param[in]  BufferSize  Size of Buffer in bytes.

  @retval EFI_SUCCESS    The file was read.
  @retval EFI_NOT_FOUND  No Kettle ESP, or no such file.
**/
STATIC
EFI_STATUS
KettleReadEspFile (
  IN  CHAR16  *Path,
  OUT CHAR8   *Buffer,
  IN  UINTN    BufferSize
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  UINTN                            Size;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;

  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (
          gBS->HandleProtocol (
                 Handles[Index],
                 &gEfiSimpleFileSystemProtocolGuid,
                 (VOID **)&Fs
                 )
          ))
    {
      continue;
    }

    if (!KettleVolumeHasFile (Fs, L"\\EFI\\BOOT\\BOOTAA64.EFI")) {
      continue;
    }

    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    if (EFI_ERROR (Root->Open (Root, &File, Path, EFI_FILE_MODE_READ, 0))) {
      Root->Close (Root);
      continue;
    }

    Size = BufferSize - 1;
    if (EFI_ERROR (File->Read (File, &Size, Buffer))) {
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    Buffer[Size] = '\0';
    File->Close (File);
    Root->Close (Root);
    Status = EFI_SUCCESS;
    break;
  }

  FreePool (Handles);
  return Status;
}

/**
  Kettle: load persistent settings.

  The NVRAM variable is authoritative because it is what survives a reset. The
  ESP file is an override: if it is present and asks for something different,
  the variable is rewritten so the change persists even after the file is
  removed.
**/
STATIC
VOID
KettleLoadSettings (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Size;
  CHAR8       File[256];
  CHAR8       *Menu;

  Size   = sizeof (mKettleSettings);
  Status = gRT->GetVariable (
                  L"KettleSettings",
                  &mKettleSettingsGuid,
                  NULL,
                  &Size,
                  &mKettleSettings
                  );
  if (EFI_ERROR (Status) || (Size != sizeof (mKettleSettings)) ||
      (mKettleSettings.Signature != KETTLE_SETTINGS_SIGNATURE))
  {
    ZeroMem (&mKettleSettings, sizeof (mKettleSettings));
    mKettleSettings.Signature = KETTLE_SETTINGS_SIGNATURE;
    mKettleSettings.Version   = KETTLE_SETTINGS_VERSION;
  }

  ZeroMem (File, sizeof (File));
  if (EFI_ERROR (KettleReadEspFile (L"\\EFI\\kettle\\boot.txt", File, sizeof (File)))) {
    return;
  }

  //
  // Match the hint cell to the chosen console scale. Index 0 is the stock
  // 8x19 cell; the rest mirror GraphicsConsoleDxe's table.
  //
  switch (mKettleSettings.ConsoleScale) {
    case 0:
      mKettleHintCellW = 8;
      mKettleHintCellH = 19;
      break;
    case 2:
      mKettleHintCellW = 16;
      mKettleHintCellH = 38;
      break;
    case 3:
      mKettleHintCellW = 19;
      mKettleHintCellH = 38;
      break;
    default:
      mKettleHintCellW = KETTLE_HINT_CELL_WIDTH;
      mKettleHintCellH = KETTLE_HINT_CELL_HEIGHT;
      break;
  }

  Menu = AsciiStrStr (File, "menu=1");
  if ((Menu != NULL) && !KettleForceMenu ()) {
    mKettleSettings.ForceMenu = 1;
    Status                    = gRT->SetVariable (
                                       L"KettleSettings",
                                       &mKettleSettingsGuid,
                                       EFI_VARIABLE_NON_VOLATILE |
                                       EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                       EFI_VARIABLE_RUNTIME_ACCESS,
                                       sizeof (mKettleSettings),
                                       &mKettleSettings
                                       );
    DEBUG ((
      DEBUG_INFO,
      "%a: boot manager forced by ESP config (%r)\n",
      __FUNCTION__,
      Status
      ));
  }
}

/**
  Kettle: boot the SteamOS/Kettle ESP.

  Mu's default boot order prefers its "Internal Storage" policy, which on this
  device chainloads boot_a. boot_a is where Mu itself lives (it is the Android
  boot image that ABL loads), so that path is self-referential and ends in a
  black screen. Find the Kettle ESP by its own marker files and boot its loader
  first; if anything is missing or the boot returns, BDS carries on with the
  normal boot order.
**/
STATIC
VOID
KettleRegisterEspBootOption (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_DEVICE_PATH_PROTOCOL         *FilePath;
  EFI_BOOT_MANAGER_LOAD_OPTION     LoadOption;
  BOOLEAN                          DirectBoot;
  CHAR8                            CmdLine[1024];
  CHAR16                           CmdLineW[1024];

  //
  // Kettle: BDS installs the UFS FAT stack only when it starts booting a device, which is
  // AFTER AfterConsole -- where this runs. Enumerating SimpleFileSystem before that finds
  // nothing, no boot option is added, and BDS falls through to the platform's "Internal
  // Storage" policy, which chainloads boot_a (Mu itself) and loops forever. Connect
  // first so the KETTLE volume exists by the time it is searched for.
  //
  EfiBootManagerConnectAll ();

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: no SimpleFileSystem handles at all (%r)\n",
      __FUNCTION__,
      Status
      ));
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: %u SimpleFileSystem handle(s) after ConnectAll\n",
    __FUNCTION__,
    (UINT32)HandleCount
    ));

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // Require both the Kettle marker and the loader itself, so that an
    // unrelated FAT volume cannot take over the boot.
    //
    if (!KettleVolumeHasFile (Fs, L"\\SteamOS\\conf\\A.conf") ||
        !KettleVolumeHasFile (Fs, L"\\EFI\\BOOT\\BOOTAA64.EFI"))
    {
      DEBUG ((
        DEBUG_INFO,
        "%a: volume %u rejected (A.conf=%a loader=%a)\n",
        __FUNCTION__,
        (UINT32)Index,
        KettleVolumeHasFile (Fs, L"\\SteamOS\\conf\\A.conf") ? "yes" : "no",
        KettleVolumeHasFile (Fs, L"\\EFI\\BOOT\\BOOTAA64.EFI") ? "yes" : "no"
        ));
      continue;
    }

    //
    // Kettle: direct kernel boot. When \kettle\cmdline.txt is present, hand
    // \kettle\Image to BDS with that text as the load options. The arm64 EFI stub
    // reads the load options as the kernel command line, so this bypasses steamcl and
    // GRUB entirely: if the boot fails from here on it is a kernel or userspace fault,
    // and the kernel's pstore console records it in the ramoops carveout.
    //
    DirectBoot = FALSE;
    if (KettleVolumeHasFile (Fs, L"\\kettle\\cmdline.txt") &&
        KettleVolumeHasFile (Fs, L"\\kettle\\Image"))
    {
      ZeroMem (CmdLine, sizeof (CmdLine));
      if (!EFI_ERROR (
            KettleReadEspFile (L"\\kettle\\cmdline.txt", CmdLine, sizeof (CmdLine))
            ))
      {
        UINTN Len;

        Len = AsciiStrLen (CmdLine);
        while ((Len > 0) &&
               ((CmdLine[Len - 1] == '\n') || (CmdLine[Len - 1] == '\r')))
        {
          CmdLine[--Len] = '\0';
        }

        AsciiStrToUnicodeStrS (CmdLine, CmdLineW, ARRAY_SIZE (CmdLineW));
        DirectBoot = TRUE;
        DEBUG ((
          DEBUG_INFO,
          "%a: direct boot, path \\kettle\\Image, cmdline (%u chars) \"%a\"\n",
          __FUNCTION__,
          (UINT32)Len,
          CmdLine
          ));
      }
    }

    FilePath = FileDevicePath (
                 Handles[Index],
                 DirectBoot ? L"\\kettle\\Image" : L"\\EFI\\BOOT\\BOOTAA64.EFI"
                 );
    if (FilePath == NULL) {
      continue;
    }

    Status = EfiBootManagerInitializeLoadOption (
               &LoadOption,
               LoadOptionNumberUnassigned,
               LoadOptionTypeBoot,
               LOAD_OPTION_ACTIVE,
               DirectBoot ? L"Kettle Linux (direct)" : L"Kettle Linux",
               FilePath,
               DirectBoot ? (UINT8 *)CmdLineW : NULL,
               DirectBoot ? (StrLen (CmdLineW) + 1) * sizeof (CHAR16) : 0
               );
    if (!EFI_ERROR (Status)) {
      //
      // Put Kettle at the front of the boot order rather than booting it here.
      // Booting directly from AfterConsole would run before BDS reaches the
      // [Volume Up] handler and the menu timeout, which would make the boot
      // manager menu unreachable.
      //
      KettleSetStage (KETTLE_STAGE_ESP_FOUND);

      Status = EfiBootManagerAddLoadOptionVariable (&LoadOption, 0);
      DEBUG ((
        DEBUG_INFO,
        "%a: Kettle ESP registered at boot position 0 (%r)\n",
        __FUNCTION__,
        Status
        ));
      EfiBootManagerFreeLoadOption (&LoadOption);
    }

    FreePool (FilePath);
    break;
  }

  if (Index == HandleCount) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: no volume carried the Kettle markers -- no boot option added\n",
      __FUNCTION__
      ));
  }

  FreePool (Handles);
}

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
  EfiBootManagerUpdateConsoleVariable (ConIn,  (EFI_DEVICE_PATH_PROTOCOL *)&UsbKeyboardDevicePath, NULL);
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

/**
  Kettle: draw a string at the console cell size.

  AsciiPrintXY renders at the native 8x19 glyph, which is unreadable next to a
  13x38 console. HII has no scaling of its own, so the run is rendered once at
  the native size and then resampled into the console cell -- the same
  approach GraphicsConsoleDxe uses.

  @param[in] Gop         Graphics output to draw into.
  @param[in] X           Left edge in pixels.
  @param[in] Y           Top edge in pixels.
  @param[in] Foreground  Text colour.
  @param[in] Background  Cell background colour.
  @param[in] String      NUL terminated Unicode string.
**/
STATIC
VOID
DrawScaledString (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN UINTN                          X,
  IN UINTN                          Y,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Foreground,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Background,
  IN CHAR16                         *String,
  IN UINTN                          CellWidth,
  IN UINTN                          CellHeight
  )
{
  EFI_STATUS                     Status;
  EFI_IMAGE_OUTPUT               *Blt;
  EFI_FONT_DISPLAY_INFO          FontInfo;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Scaled;
  UINTN                          Len;
  UINTN                          SrcWidth;
  UINTN                          DstWidth;
  UINTN                          DstHeight;
  UINTN                          SrcX;
  UINTN                          SrcY;
  UINTN                          X1;
  UINTN                          Y1;

  Len = StrLen (String);
  if ((Len == 0) || (Gop == NULL) || (mHiiFont == NULL)) {
    return;
  }

  SrcWidth = Len * EFI_GLYPH_WIDTH;

  Blt = AllocateZeroPool (sizeof (EFI_IMAGE_OUTPUT));
  if (Blt == NULL) {
    return;
  }

  Blt->Width        = (UINT16)SrcWidth;
  Blt->Height       = (UINT16)EFI_GLYPH_HEIGHT;
  Blt->Image.Bitmap = AllocateZeroPool (
                        SrcWidth * EFI_GLYPH_HEIGHT * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
                        );
  if (Blt->Image.Bitmap == NULL) {
    FreePool (Blt);
    return;
  }

  ZeroMem (&FontInfo, sizeof (FontInfo));
  CopyMem (&FontInfo.ForegroundColor, Foreground, sizeof (*Foreground));
  CopyMem (&FontInfo.BackgroundColor, Background, sizeof (*Background));

  Status = mHiiFont->StringToImage (
                       mHiiFont,
                       EFI_HII_IGNORE_IF_NO_GLYPH | EFI_HII_IGNORE_LINE_BREAK,
                       String,
                       &FontInfo,
                       &Blt,
                       0,
                       0,
                       NULL,
                       NULL,
                       NULL
                       );
  if (EFI_ERROR (Status)) {
    FreePool (Blt->Image.Bitmap);
    FreePool (Blt);
    return;
  }

  DstWidth  = Len * CellWidth;
  DstHeight = CellHeight;

  Scaled = AllocatePool (DstWidth * DstHeight * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));
  if (Scaled == NULL) {
    FreePool (Blt->Image.Bitmap);
    FreePool (Blt);
    return;
  }

  for (Y1 = 0; Y1 < DstHeight; Y1++) {
    SrcY = (Y1 * EFI_GLYPH_HEIGHT) / DstHeight;
    for (X1 = 0; X1 < DstWidth; X1++) {
      SrcX = ((X1 % CellWidth) * EFI_GLYPH_WIDTH) / CellWidth;

      Scaled[(Y1 * DstWidth) + X1] =
        Blt->Image.Bitmap[(SrcY * SrcWidth) +
                          ((X1 / CellWidth) * EFI_GLYPH_WIDTH) + SrcX];
    }
  }

  Gop->Blt (
         Gop,
         Scaled,
         EfiBltBufferToVideo,
         0,
         0,
         X,
         Y,
         DstWidth,
         DstHeight,
         DstWidth * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)
         );

  FreePool (Scaled);
  FreePool (Blt->Image.Bitmap);
  FreePool (Blt);
}

//
// Settings screen.
//
STATIC CONST KETTLE_CELL_SIZE  mKettleUiCells[] = {
  { 8,  19 },
  { 13, 38 },
  { 16, 38 },
  { 19, 38 },
};

#define KETTLE_UI_SCALE_COUNT  (sizeof (mKettleUiCells) / sizeof (mKettleUiCells[0]))
#define KETTLE_UI_ROW_COUNT    3

STATIC CONST CHAR16  *mKettleUiScaleNames[] = {
  L"Stock   8 x 19",
  L"Large   13 x 38",
  L"Larger  16 x 38",
  L"Huge    19 x 38",
};

/**
  Kettle: installed DRAM, in MiB.

  Sums every boot services memory descriptor that describes real system memory.
  Under the UEFI boot chain nothing patches the device tree's /memory node the
  way ABL does on the stock chain, so the memory map is the only place the true
  size is available. The settings screen is where it gets read off the panel.

  @return Total installed memory in MiB, or 0 if the map could not be read.
**/
STATIC
UINT64
KettleTotalMemoryMb (
  VOID
  )
{
  EFI_STATUS             Status;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  Count;
  UINTN                  Index;
  UINT64                 TotalBytes;

  MapSize = 0;
  Status  = gBS->GetMemoryMap (
                   &MapSize,
                   NULL,
                   &MapKey,
                   &DescriptorSize,
                   &DescriptorVersion
                   );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return 0;
  }

  //
  // GetMemoryMap can grow the map while servicing the request, so ask for room
  // above the size it just reported.
  //
  MapSize += DescriptorSize * 16;
  Map = AllocatePool (MapSize);
  if (Map == NULL) {
    return 0;
  }

  Status = gBS->GetMemoryMap (
                  &MapSize,
                  Map,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Map);
    return 0;
  }

  TotalBytes = 0;
  Count      = MapSize / DescriptorSize;
  for (Index = 0; Index < Count; Index++) {
    EFI_MEMORY_DESCRIPTOR  *Descriptor;

    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + (Index * DescriptorSize));
    //
    // Everything up to and including EfiPersistentMemory is real DRAM. The
    // memory-mapped IO and PAL ranges above it are not.
    //
    if (Descriptor->Type <= EfiPersistentMemory) {
      TotalBytes += Descriptor->NumberOfPages * EFI_PAGE_SIZE;
    }
  }

  FreePool (Map);

  return TotalBytes / (1024ULL * 1024ULL);
}

/**
  Kettle: fill a rectangle on the panel.

  @param[in] Gop     Graphics output to draw into.
  @param[in] X       Left edge.
  @param[in] Y       Top edge.
  @param[in] Width   Width in pixels.
  @param[in] Height  Height in pixels.
  @param[in] Color   Fill colour.
**/
STATIC
VOID
KettleFill (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN UINTN                          X,
  IN UINTN                          Y,
  IN UINTN                          Width,
  IN UINTN                          Height,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Color
  )
{
  if ((Gop == NULL) || (Width == 0) || (Height == 0)) {
    return;
  }

  Gop->Blt (
         Gop,
         Color,
         EfiBltVideoFill,
         0,
         0,
         X,
         Y,
         Width,
         Height,
         0
         );
}

/**
  Kettle: settings screen.

  Drawn straight to GOP rather than through the console, so the chosen scale can
  be previewed live: this whole screen re-renders at the new cell size the
  moment it is changed. The choice is written to NVRAM, and GraphicsConsoleDxe
  picks it up on the next boot, when the console's mode table is built.

  Keys: Volume Up/Down move, Power (SCAN_ESC) or Enter changes or saves. The
  screen saves and leaves on its own after a period with no input, so a device
  with no usable key cannot be stranded here.

  @retval TRUE   Left deliberately ("Save and exit"), so the boot manager menu
                 should be shown next -- that is where a boot option can be
                 changed, and it is what leaving a settings screen is expected
                 to return to.
  @retval FALSE  Timed out. Boot normally rather than surprising the user with
                 a menu they did not ask for.
**/
STATIC
BOOLEAN
KettleSettingsUi (
  VOID
  )
{
  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EFI_EVENT                      Timer;
  EFI_EVENT                      Events[2];
  UINTN                          EventCount;
  UINTN                          Index;
  UINTN                          Selection;
  BOOLEAN                        ShowMenu;
  UINT32                         ScaleIndex;
  UINT32                         ForceMenu;
  UINTN                          CellW;
  UINTN                          CellH;
  UINTN                          PanelX;
  UINTN                          PanelY;
  UINTN                          PanelW;
  UINTN                          PanelH;
  UINTN                          Row;
  UINTN                          ScreenW;
  UINTN                          ScreenH;
  UINT64                         InstalledMb;
  CHAR16                         Buffer[96];
  EFI_INPUT_KEY                  InputKey;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Text  = { 0xF0, 0xF0, 0xF0, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Dim   = { 0x90, 0x90, 0x9C, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Bg    = { 0x10, 0x10, 0x14, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Panel = { 0x1E, 0x1E, 0x24, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Sel   = { 0x2E, 0x5A, 0x8A, 0 };

  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&Gop
                  );
  if (EFI_ERROR (Status) || (Gop == NULL)) {
    return FALSE;
  }

  if (mHiiFont == NULL) {
    Status = gBS->LocateProtocol (
                    &gEfiHiiFontProtocolGuid,
                    NULL,
                    (VOID **)&mHiiFont
                    );
    if (EFI_ERROR (Status) || (mHiiFont == NULL)) {
      return FALSE;
    }
  }

  ScreenW = Gop->Mode->Info->HorizontalResolution;
  ScreenH = Gop->Mode->Info->VerticalResolution;

  Selection  = 0;
  ShowMenu   = FALSE;
  ScaleIndex = mKettleSettings.ConsoleScale;
  if (ScaleIndex >= KETTLE_UI_SCALE_COUNT) {
    ScaleIndex = 1;
  }

  ForceMenu   = KettleForceMenu () ? 1 : 0;
  InstalledMb = KettleTotalMemoryMb ();

  Timer      = NULL;
  EventCount = 1;
  Events[0]  = gST->ConIn->WaitForKey;
  Events[1]  = NULL;
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER, 0, NULL, NULL, &Timer))) {
    Events[1]  = Timer;
    EventCount = 2;
  }

  for ( ; ;) {
    CellW = mKettleUiCells[ScaleIndex].Width;
    CellH = mKettleUiCells[ScaleIndex].Height;

    KettleFill (Gop, 0, 0, ScreenW, ScreenH, &Bg);

    PanelX = ScreenW / 16;
    PanelW = ScreenW - (PanelX * 2);
    PanelY = ScreenH / 12;
    PanelH = ScreenH - (PanelY * 2);
    KettleFill (Gop, PanelX, PanelY, PanelW, PanelH, &Panel);

    Row = PanelY + CellH;

    DrawScaledString (
      Gop,
      PanelX + (CellW * 2),
      Row,
      &Text,
      &Panel,
      L"Kettle Settings",
      CellW,
      CellH
      );
    Row += CellH * 2;

    //
    // Console text size
    //
    if (Selection == 0) {
      KettleFill (
        Gop,
        PanelX + CellW,
        Row - (CellH / 4),
        PanelW - (CellW * 2),
        CellH + (CellH / 2),
        &Sel
        );
    }

    UnicodeSPrint (
      Buffer,
      sizeof (Buffer),
      L"Console text    %s",
      mKettleUiScaleNames[ScaleIndex]
      );
    DrawScaledString (
      Gop,
      PanelX + (CellW * 2),
      Row,
      &Text,
      (Selection == 0) ? &Sel : &Panel,
      Buffer,
      CellW,
      CellH
      );
    Row += CellH * 2;

    //
    // Startup menu
    //
    if (Selection == 1) {
      KettleFill (
        Gop,
        PanelX + CellW,
        Row - (CellH / 4),
        PanelW - (CellW * 2),
        CellH + (CellH / 2),
        &Sel
        );
    }

    UnicodeSPrint (
      Buffer,
      sizeof (Buffer),
      L"Startup menu    %s",
      (ForceMenu != 0) ? L"Always show" : L"Hold Vol Up"
      );
    DrawScaledString (
      Gop,
      PanelX + (CellW * 2),
      Row,
      &Text,
      (Selection == 1) ? &Sel : &Panel,
      Buffer,
      CellW,
      CellH
      );
    Row += CellH * 2;

    //
    // Read-only system information. Not selectable: it reports facts only the
    // firmware knows, and it is how the panel's real memory size is read off a
    // device whose bootloader reports nothing over fastboot.
    //
    UnicodeSPrint (
      Buffer,
      sizeof (Buffer),
      L"Memory          %Lu MiB",
      InstalledMb
      );
    DrawScaledString (
      Gop,
      PanelX + (CellW * 2),
      Row,
      &Dim,
      &Panel,
      Buffer,
      CellW,
      CellH
      );
    Row += CellH * 2;

    //
    // Save and exit
    //
    if (Selection == 2) {
      KettleFill (
        Gop,
        PanelX + CellW,
        Row - (CellH / 4),
        PanelW - (CellW * 2),
        CellH + (CellH / 2),
        &Sel
        );
    }

    DrawScaledString (
      Gop,
      PanelX + (CellW * 2),
      Row,
      &Text,
      (Selection == 2) ? &Sel : &Panel,
      L"Save and exit",
      CellW,
      CellH
      );
    Row += CellH * 3;

    DrawScaledString (
      Gop,
      PanelX + (CellW * 2),
      Row,
      &Dim,
      &Panel,
      L"Vol Up / Vol Down: move      Power: change or save",
      CellW,
      CellH
      );

    //
    // Wait for a key, or time out so the screen cannot trap the boot.
    //
    if (Timer != NULL) {
      gBS->SetTimer (Timer, TimerRelative, EFI_TIMER_PERIOD_SECONDS (30));
    }

    gBS->WaitForEvent (EventCount, Events, &Index);
    if (Index != 0) {
      break;
    }

    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &InputKey);
    if (EFI_ERROR (Status)) {
      continue;
    }

    if ((InputKey.ScanCode == SCAN_UP) || (InputKey.ScanCode == SCAN_LEFT)) {
      Selection = (Selection == 0) ? (KETTLE_UI_ROW_COUNT - 1) : (Selection - 1);
    } else if ((InputKey.ScanCode == SCAN_DOWN) || (InputKey.ScanCode == SCAN_RIGHT)) {
      Selection = (Selection + 1) % KETTLE_UI_ROW_COUNT;
    } else if ((InputKey.ScanCode == SCAN_ESC) ||
               (InputKey.UnicodeChar == CHAR_CARRIAGE_RETURN) ||
               (InputKey.UnicodeChar == L' '))
    {
      if (Selection == 0) {
        ScaleIndex = (ScaleIndex + 1) % KETTLE_UI_SCALE_COUNT;
      } else if (Selection == 1) {
        ForceMenu = (ForceMenu == 0) ? 1 : 0;
      } else {
        //
        // Deliberate exit: hand over to the boot manager menu so the boot
        // option can be changed, rather than booting whatever is first.
        //
        ShowMenu = TRUE;
        break;
      }
    }
  }

  if (Timer != NULL) {
    gBS->SetTimer (Timer, TimerCancel, 0);
    gBS->CloseEvent (Timer);
  }

  //
  // Persist. The console keeps its current cell size until the next boot,
  // because the mode table is built when GraphicsConsoleDxe starts.
  //
  mKettleSettings.Signature    = KETTLE_SETTINGS_SIGNATURE;
  mKettleSettings.Version      = 1;
  mKettleSettings.ForceMenu    = ForceMenu;
  mKettleSettings.ConsoleScale = ScaleIndex;

  Status = gRT->SetVariable (
                  L"KettleSettings",
                  &mKettleSettingsGuid,
                  EFI_VARIABLE_NON_VOLATILE |
                  EFI_VARIABLE_BOOTSERVICE_ACCESS |
                  EFI_VARIABLE_RUNTIME_ACCESS,
                  sizeof (mKettleSettings),
                  &mKettleSettings
                  );
  DEBUG ((
    DEBUG_INFO,
    "%a: scale %d, menu %d, show menu %d (%r)\n",
    __FUNCTION__,
    ScaleIndex,
    ForceMenu,
    ShowMenu,
    Status
    ));

  return ShowMenu;
}

//
// Kettle: publish installed DRAM into the device tree.
//
// The mainline DTS this platform ships declares /memory@a0000000 with a zero
// length, because on the stock chain ABL fills the size in before the kernel
// ever sees it. Nothing does that under a UEFI boot, and arm64 Linux handed a
// zero-sized /memory has no pages to run in: it faults before it can bring up a
// console, so the failure presents as a device that simply never comes back
// (no panel, no USB gadget, nothing in pstore).
//
// Fill the node in here, from the firmware's own memory map, so the kernel gets
// the true amount of RAM on any SKU instead of a size baked in at build time.
//
/**
  Kettle: derive the DDR window the kernel should be told about.

  Mu's own map is deliberately sparse: it lists the carveouts it cares about and
  leaves the usable gaps between them undescribed. Taking each descriptor
  literally would hand Linux a fragment of RAM while the rest of the memory the
  device actually has goes unused, so the window is the span from the lowest to
  the highest DDR descriptor instead.

  That is only sound if the span describes memory throughout, so every
  memory-mapped IO descriptor that falls inside it is reported. On this SoC the
  only such window is "DBI Dump" at 0xA6100000..0xA7000000, which the DTB
  reserves with no-map; the rest of the IO regions live below 0x80000000, well
  outside DDR. The report exists so a register block can never start being
  described as RAM on another platform without saying so.

  @param[out] Base  Lowest DDR address.
  @param[out] Size  Length of the DDR window.

  @return TRUE when a DDR window was found.
**/
STATIC
BOOLEAN
KettleCollectDdrSpan (
  OUT UINT64  *Base,
  OUT UINT64  *Size
  )
{
  EFI_STATUS             Status;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  Count;
  UINTN                  Index;
  UINT64                 Low;
  UINT64                 High;
  UINT64                 UnboundedHigh;
  EFI_MEMORY_DESCRIPTOR  *Desc2;
  UINT64                 Start2;
  UINT64                 End2;
  UINTN                  Logged;
  BOOLEAN                Changed;
  BOOLEAN                Found;

  Low   = MAX_UINT64;
  High  = 0;
  UnboundedHigh = 0;
  Logged = 0;
  Found = FALSE;

  MapSize = 0;
  Status  = gBS->GetMemoryMap (
                   &MapSize,
                   NULL,
                   &MapKey,
                   &DescriptorSize,
                   &DescriptorVersion
                   );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return FALSE;
  }

  MapSize += DescriptorSize * 32;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    return FALSE;
  }

  Status = gBS->GetMemoryMap (
                  &MapSize,
                  Map,
                  &MapKey,
                  &DescriptorSize,
                  &DescriptorVersion
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Map);
    return FALSE;
  }

  Count = MapSize / DescriptorSize;
  for (Index = 0; Index < Count; Index++) {
    EFI_MEMORY_DESCRIPTOR  *Descriptor;
    UINT64                 Start;
    UINT64                 End;

    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + (Index * DescriptorSize));
    if (Descriptor->NumberOfPages == 0) {
      continue;
    }

    Start = Descriptor->PhysicalStart;
    End   = Start + (Descriptor->NumberOfPages * EFI_PAGE_SIZE);

    if ((Descriptor->Type == EfiMemoryMappedIO) ||
        (Descriptor->Type == EfiMemoryMappedIOPortSpace) ||
        (Descriptor->Type == EfiPalCode))
    {
      //
      // An IO window inside the DDR range is a hole in the span, and a single
      // 16-byte reg cannot express a hole. This first shipped as a hard failure
      // and that was wrong: SM8550 has one such window, "DBI Dump" at
      // 0xA6100000..0xA7000000 (15 MiB, MMAP_IO, uncacheable), which sits
      // between the demura and FD carveouts -- squarely inside DDR. Aborting
      // there meant /memory was never patched at all, so the kernel still got a
      // zero-sized memory node and the boot died exactly as before. A fix that
      // silently does nothing is worse than the defect it was meant to cure.
      //
      // The window is now named in the log and the span is still published. The
      // blob must reserve it with no-map so Linux does not hand it out as RAM;
      // sm8550-zte-nx729j.dtb carries "dbi-dump-region@a6100000" for exactly the
      // one this firmware has, and this message is how any future one is found.
      //
      if ((Start < 0x10000000000ULL) && (End > 0x80000000ULL)) {
        DEBUG ((
          DEBUG_WARN,
          "%a: IO window 0x%lx..0x%lx is inside DDR; it must be no-map reserved in the DTB\n",
          __FUNCTION__,
          (UINT64)Start,
          (UINT64)End
          ));
      }

      continue;
    }

    //
    // DDR on SM8550 starts at the HYP carveout; below that is MMIO. Cap the
    // window so a bogus descriptor cannot describe the whole address space.
    //
    if ((Start < 0x80000000ULL) || (Start >= 0x10000000000ULL) ||
        (End <= Start) || (End > 0x10000000000ULL))
    {
      continue;
    }

    if (Start < Low) {
      Low = Start;
    }

    if (End > High) {
      High = End;
    }

    //
    // Kettle: record the descriptors themselves, not just their extremes.
    //
    // This is how the phantom-range bug was finally identified, and it is cheap enough to keep:
    // type 7 is EfiConventionalMemory, 4 is EfiBootServicesData, 3 is EfiBootServicesCode, 2 is
    // EfiLoaderData. Any address that appears here but is not backed by real DRAM is what the
    // gap walk below has to reject.
    //
    if (Logged < 48) {
      DEBUG ((
        DEBUG_WARN,
        "%a: ddr desc type=%u 0x%lx..0x%lx\n",
        __FUNCTION__,
        (UINT32)Descriptor->Type,
        (UINT64)Start,
        (UINT64)End
        ));
      Logged++;
    }

    Found = TRUE;
  }

  //
  // Kettle: reject RAM ranges that are not reachable from real DDR.
  //
  // The first boot with a published initrd produced:
  //     /memory 0x80000000..0xC00000000 (47104 MiB)
  // on a 12288 MiB handset. ABL exports RAM descriptors up at 0xA00000000..0xC00000000, and
  // taking min/max across every descriptor swallowed that phantom space. The kernel then built
  // page metadata for ~12 million pages out of it and overwrote the initramfs Mu had just
  // placed at 0xC100B000 -- which is why the initramfs never executed, why no last-boot.log was
  // written and why pstore stayed empty.
  //
  // Real DDR is contiguous apart from carveouts of a few tens of MiB; the phantom ranges are
  // separated from it by tens of GiB. So grow the window from the lowest descriptor only while
  // the next range is within KETTLE_DDR_MAX_GAP, and stop at the first chasm.
  //
  UnboundedHigh = High;
  High          = Low;
  Changed       = TRUE;
  while (Changed) {
    Changed = FALSE;
    for (Index = 0; Index < Count; Index++) {
      Desc2 = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + (Index * DescriptorSize));
      if (Desc2->NumberOfPages == 0) {
        continue;
      }

      if ((Desc2->Type == EfiMemoryMappedIO) ||
          (Desc2->Type == EfiMemoryMappedIOPortSpace) ||
          (Desc2->Type == EfiPalCode) ||
          (Desc2->Type == EfiUnusableMemory))
      {
        continue;
      }

      Start2 = Desc2->PhysicalStart;
      End2   = Start2 + (Desc2->NumberOfPages * EFI_PAGE_SIZE);
      if ((Start2 < 0x80000000ULL) || (Start2 >= 0x10000000000ULL) || (End2 <= Start2)) {
        continue;
      }

      if (((Start2 <= High) || (Start2 - High <= (UINT64)0x40000000ULL)) && (End2 > High)) {
        High    = End2;
        Changed = TRUE;
      }
    }
  }

  DEBUG ((
    DEBUG_WARN,
    "%a: ddr window low=0x%lx bounded=0x%lx unbounded=0x%lx (%u descriptor(s))\n",
    __FUNCTION__,
    (UINT64)Low,
    (UINT64)High,
    (UINT64)UnboundedHigh,
    (UINT32)Count
    ));

  FreePool (Map);

  if (!Found || (High <= Low)) {
    return FALSE;
  }

  *Base = Low;
  *Size = High - Low;
  return TRUE;
}

/**
  Kettle: set /memory in the published FDT to the real DDR layout.

  The blob DtPlatformDxe publishes is a pool copy, so it is writable in place.
  A single coalesced range reuses the property slot exactly, which means
  replacing it never needs room the blob does not have.
**/
STATIC
VOID
KettlePatchFdtMemory (
  VOID
  )
{
  VOID     *Fdt;
  UINT64   DdrBase;
  UINT64   DdrSize;
  UINT32   Cells[4];
  INT32    Root;
  INT32    Node;
  INT32    Result;
  UINTN    Index;
  INT32    RegLen;
  VOID     *OldReg;

  Fdt = NULL;
  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gFdtTableGuid)) {
      Fdt = gST->ConfigurationTable[Index].VendorTable;
      break;
    }
  }

  if (Fdt == NULL) {
    DEBUG ((DEBUG_WARN, "%a: no FDT in the configuration table\n", __FUNCTION__));
    return;
  }

  if (FdtCheckHeader (Fdt) != 0) {
    DEBUG ((DEBUG_WARN, "%a: published blob is not an FDT\n", __FUNCTION__));
    return;
  }

  Root = 0;
  if ((FdtAddressCells (Fdt, Root) != 2) || (FdtSizeCells (Fdt, Root) != 2)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: root is %d/%d cells, not 2/2 -- leaving /memory alone\n",
      __FUNCTION__,
      FdtAddressCells (Fdt, Root),
      FdtSizeCells (Fdt, Root)
      ));
    return;
  }

  Node = FdtPathOffset (Fdt, "/memory@a0000000");
  if (Node < 0) {
    INT32       Candidate;
    CONST VOID  *Type;
    INT32       TypeLen;

    for (Candidate = FdtFirstSubnode (Fdt, Root);
         Candidate >= 0;
         Candidate = FdtNextSubnode (Fdt, Candidate))
    {
      Type = FdtGetProp (Fdt, Candidate, "device_type", &TypeLen);
      if ((Type != NULL) && (TypeLen >= 7) &&
          (AsciiStrnCmp (Type, "memory", 6) == 0))
      {
        Node = Candidate;
        break;
      }
    }
  }

  if (Node < 0) {
    DEBUG ((DEBUG_WARN, "%a: no /memory node to fill in\n", __FUNCTION__));
    return;
  }

  if (!KettleCollectDdrSpan (&DdrBase, &DdrSize)) {
    DEBUG ((DEBUG_WARN, "%a: could not derive a DDR window from the memory map\n", __FUNCTION__));
    return;
  }

  //
  // The DTB ships this node with a 16-byte reg, and the published blob has no
  // slack to grow it, so only replace the value when it is the same size. A
  // one-range window is, which is why the span is used above.
  //
  OldReg = (VOID *)FdtGetProp (Fdt, Node, "reg", &RegLen);
  if ((OldReg == NULL) || (RegLen != sizeof (Cells))) {
    DEBUG ((
      DEBUG_WARN,
      "%a: /memory reg is %d bytes, expected %u -- not overwriting\n",
      __FUNCTION__,
      RegLen,
      (UINT32)sizeof (Cells)
      ));
    return;
  }

  //
  // Kettle: FDT integers are big-endian and FdtSetProp copies the buffer verbatim, so the cells
  // must already be in FDT order. Handing it a native array published a byte-swapped /memory --
  // a 0x80000000 base/size arrived in Linux as base 0x80, size 0x80, which leaves the kernel
  // with 128 bytes of RAM and kills it just after earlycon.
  //
  Cells[0] = SwapBytes32 ((UINT32)(DdrBase >> 32));
  Cells[1] = SwapBytes32 ((UINT32)(DdrBase & 0xFFFFFFFFULL));
  Cells[2] = SwapBytes32 ((UINT32)(DdrSize >> 32));
  Cells[3] = SwapBytes32 ((UINT32)(DdrSize & 0xFFFFFFFFULL));

  Result = FdtSetProp (Fdt, Node, "reg", Cells, (UINT32)sizeof (Cells));
  if (Result != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: FdtSetProp(reg) failed (%d); /memory left at its shipped size\n",
      __FUNCTION__,
      Result
      ));
    return;
  }

  DEBUG ((
    DEBUG_WARN,
    "%a: /memory 0x%lx..0x%lx (%lu MiB) patched into the published FDT\n",
    __FUNCTION__,
    (UINT64)DdrBase,
    (UINT64)(DdrBase + DdrSize),
    (UINT64)(DdrSize >> 20)
    ));

  //
  // Kettle: confirm the framebuffer carve-out survived the resize.
  //
  // The splash region at 0xB8000000 is what the DPU is scanning while the kernel boots. If this
  // range ends up covered by the /memory window AND the reserved-memory entry is missing, the
  // kernel will allocate over live scanout memory -- which is the other candidate for the garbled
  // screen. Printing both makes that a measurement instead of a guess.
  //
  {
    INT32        Splash;
    CONST VOID   *SplashReg;
    INT32        SplashLen;
    CONST UINT32 *SplashCells;
    UINT64       SplashStart;
    UINT64       SplashSize;

    Splash = FdtPathOffset (Fdt, "/reserved-memory/splash-region@b8000000");
    if (Splash < 0) {
      DEBUG ((DEBUG_WARN, "%a: no /reserved-memory/splash-region@b8000000 in the published FDT\n",
              __FUNCTION__));
    } else {
      SplashReg   = FdtGetProp (Fdt, Splash, "reg", &SplashLen);
      SplashCells = (CONST UINT32 *)SplashReg;
      if ((SplashReg != NULL) && (SplashLen == 16)) {
        //
        // Decode into named locals first. The earlier version repeated the shift-and-or inline
        // in all three arguments and the log came back as "0xB8..0xB0BA", which is not a real
        // address and made a working check look broken.
        //
        //
        // Decode big-endian as well. Reading the raw cells natively is what made this line
        // report "0xB8 + 0xB002" for 0xB8000000 + 0x2B00000, and made a covered splash look
        // uncovered.
        //
        SplashStart = ((UINT64)SwapBytes32 (SplashCells[0]) << 32) | (UINT64)SwapBytes32 (SplashCells[1]);
        SplashSize  = ((UINT64)SwapBytes32 (SplashCells[2]) << 32) | (UINT64)SwapBytes32 (SplashCells[3]);

        DEBUG ((
          DEBUG_WARN,
          "%a: splash reserved 0x%lx + 0x%lx (covered by /memory: %u)\n",
          __FUNCTION__,
          (UINT64)SplashStart,
          (UINT64)SplashSize,
          (UINT32)((SplashStart >= DdrBase) && (SplashStart < DdrBase + DdrSize))
          ));
      } else {
        DEBUG ((DEBUG_WARN, "%a: splash reg is %d bytes, unexpected\n", __FUNCTION__, SplashLen));
      }
    }
  }
}

//
// Kettle: the initramfs.
//
// The installed image ships /boot/initramfs-linux.img, and its own fstab says /, /var and
// the /etc overlay are the initramfs' job -- including the SteamOS slot selection that
// reads /dev/disk/by-partsets. Loading only \kettle\Image therefore silently skips all of
// it and boots the root filesystem directly, which is not the system that was built.
//
// The command line cannot express this. This kernel's EFI stub has no initrd= parser: the
// literal "initrd=" appears zero times in Image, while "dtb=" does (0x2870588), so the
// stub's load-option table simply does not have that entry. What the kernel does honour is
// the device tree form -- early_init_dt_check_for_initrd, linux,initrd-start and
// linux,initrd-end are all present -- so the initrd is loaded here and announced in
// /chosen, exactly as a bootloader would do it.
//
// The switch is the file itself: present \kettle\initramfs.img on the ESP and it is used,
// remove it and the boot goes back to the old behaviour. No firmware change either way.
//
#define KETTLE_INITRD_PATH  L"\\kettle\\initramfs.img"

//
// A tiny file is not an initramfs and a huge one will not fit alongside the kernel in the
// usable window; either way it is better to boot without one and say so than to run out of
// memory inside the boot manager.
//
#define KETTLE_INITRD_MIN_SIZE   (1 * 1024 * 1024)
#define KETTLE_INITRD_MAX_SIZE   (128 * 1024 * 1024)

//
// The buffer must land where the device tree leaves memory usable. Anything inside a
// no-map carveout is rejected by the kernel with "INITRD: ... is not a memory region" and
// the initrd is dropped, so the request is capped at the end of the firmware's DXE heap --
// a range the blob does not reserve -- and the result is checked to be inside it. This
// costs nothing when it succeeds and prevents a mystery failure when it does not.
//
#define KETTLE_INITRD_ALLOC_TOP  0xC7CC0000ULL   // end of "DXE Heap"
#define KETTLE_INITRD_ALLOC_LOW  0xBAB00000ULL   // start of "DXE Heap"

/**
  Kettle: the FDT the kernel will be handed.

  @return The published blob, or NULL when the platform published none.
**/
STATIC
VOID *
KettleGetFdt (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gFdtTableGuid)) {
      return gST->ConfigurationTable[Index].VendorTable;
    }
  }

  return NULL;
}

/**
  Kettle: read \kettle\initramfs.img off the Kettle ESP into loader memory.

  The buffer is deliberately never freed: it stays in the memory map under the address
  announced to the kernel, so nothing else can be handed the same pages between here and
  the handoff.

  @param[out] Base  Physical address of the loaded initramfs.
  @param[out] Size  Its length in bytes.

  @retval EFI_SUCCESS    Loaded.
  @retval EFI_NOT_FOUND  No Kettle ESP, or no \kettle\initramfs.img on it.
**/
STATIC
EFI_STATUS
KettleLoadInitrd (
  OUT EFI_PHYSICAL_ADDRESS  *Base,
  OUT UINTN                 *Size
  )
{
  EFI_STATUS                       Status;
  EFI_STATUS                       OpenStatus;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize;
  UINT8                            InfoBuffer[sizeof (EFI_FILE_INFO) + 256];
  UINTN                            FileSize;
  UINTN                            Read;
  UINTN                            Pages;
  EFI_PHYSICAL_ADDRESS             Address;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    //
    // WARN, not silent: an empty volume list here is the difference between "no initramfs" and
    // "the ESP was never connected", and that ambiguity already cost a build cycle.
    //
    DEBUG ((
      DEBUG_WARN,
      "%a: no SimpleFileSystem protocol at all (%r) -- storage stack not connected\n",
      __FUNCTION__,
      Status
      ));
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;

  DEBUG ((
    DEBUG_WARN,
    "%a: scanning %u simple-file-system volume(s) for initramfs\n",
    __FUNCTION__,
    (UINT32)HandleCount
    ));

  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (
          gBS->HandleProtocol (
                 Handles[Index],
                 &gEfiSimpleFileSystemProtocolGuid,
                 (VOID **)&Fs
                 )
          ))
    {
      continue;
    }

    //
    // Same marker test the boot option uses, so an unrelated FAT volume cannot supply an
    // initramfs for a different system.
    //
    if (!KettleVolumeHasFile (Fs, L"\\EFI\\BOOT\\BOOTAA64.EFI")) {
      DEBUG ((
        DEBUG_WARN,
        "%a: volume %u has no BOOTAA64.EFI marker\n",
        __FUNCTION__,
        (UINT32)Index
        ));
      continue;
    }

    OpenStatus = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (OpenStatus)) {
      DEBUG ((
        DEBUG_WARN,
        "%a: volume %u OpenVolume failed (%r)\n",
        __FUNCTION__,
        (UINT32)Index,
        OpenStatus
        ));
      continue;
    }

    //
    // Kettle: try each candidate name and report every status.
    //
    // Everything Mu reads successfully elsewhere on this device is a native 8.3 name:
    // \\EFI\\BOOT\\BOOTAA64.EFI, \\kettle\\cmdline.txt, \\SteamOS\\conf\\A.conf and
    // \\kettle\\Image. The initramfs is the only path that needs a Long File Name entry
    // (13 characters), so a FAT driver that cannot resolve LFNs would open all of those and
    // fail this one with NOT_FOUND -- which is exactly what logfs reported.
    //
    // The short-name aliases below are what a Linux vfat mount creates for the same file, so
    // if LFN resolution is the problem one of them opens. Printing each status means this
    // cannot be mistaken for a silent failure again, whichever way it goes.
    //
    {
      CONST CHAR16  *Candidates[4];
      UINTN         Candidate;
      BOOLEAN       Opened;

      Candidates[0] = L"\\kettle\\initramfs.img";
      Candidates[1] = L"\\kettle\\init.img";
      Candidates[2] = L"\\kettle\\initrd.img";
      Candidates[3] = L"\\kettle\\INITRA~1.IMG";

      Opened = FALSE;
      for (Candidate = 0; Candidate < 4; Candidate++) {
        OpenStatus = Root->Open (
                       Root,
                       &File,
                       (CHAR16 *)Candidates[Candidate],
                       EFI_FILE_MODE_READ,
                       0
                       );
        DEBUG ((
          DEBUG_WARN,
          "%a: volume %u open %s -> %r\n",
          __FUNCTION__,
          (UINT32)Index,
          Candidates[Candidate],
          OpenStatus
          ));
        if (!EFI_ERROR (OpenStatus)) {
          Opened = TRUE;
          break;
        }
      }

      if (!Opened) {
        Root->Close (Root);
        continue;
      }
    }

    Info     = (EFI_FILE_INFO *)InfoBuffer;
    InfoSize = sizeof (InfoBuffer);
    if (EFI_ERROR (File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info))) {
      DEBUG ((
        DEBUG_WARN,
        "%a: GetInfo failed on %s (%r)\n",
        __FUNCTION__,
        "\\kettle\\initramfs.img",
        Status
        ));
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    FileSize = (UINTN)Info->FileSize;
    if ((FileSize < KETTLE_INITRD_MIN_SIZE) || (FileSize > KETTLE_INITRD_MAX_SIZE)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: %s is %lu bytes, outside 1..128 MiB -- not using it\n",
        __FUNCTION__,
        "\\kettle\\initramfs.img",
        (UINT64)FileSize
        ));
      File->Close (File);
      Root->Close (Root);
      continue;
    }

    Pages   = EFI_SIZE_TO_PAGES (FileSize);
    Address = KETTLE_INITRD_ALLOC_TOP;
    Status  = gBS->AllocatePages (AllocateMaxAddress, EfiLoaderData, Pages, &Address);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: no memory for %u pages below 0x%lx (%r)\n",
        __FUNCTION__,
        (UINT32)Pages,
        (UINT64)KETTLE_INITRD_ALLOC_TOP,
        Status
        ));
      File->Close (File);
      Root->Close (Root);
      break;
    }

    if ((UINT64)Address < KETTLE_INITRD_ALLOC_LOW) {
      //
      // Below the DXE heap is memory the blob may well reserve; handing it over as the
      // initrd would only be rejected by the kernel, so give it back and boot without.
      //
      DEBUG ((
        DEBUG_ERROR,
        "%a: initramfs would sit at 0x%lx, below the usable window 0x%lx -- not using it\n",
        __FUNCTION__,
        (UINT64)Address,
        (UINT64)KETTLE_INITRD_ALLOC_LOW
        ));
      gBS->FreePages (Address, Pages);
      File->Close (File);
      Root->Close (Root);
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }

    Read = FileSize;
    Status = File->Read (File, &Read, (VOID *)(UINTN)Address);
    File->Close (File);
    Root->Close (Root);

    if (EFI_ERROR (Status) || (Read != FileSize)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: read %lu of %lu bytes (%r)\n",
        __FUNCTION__,
        (UINT64)Read,
        (UINT64)FileSize,
        Status
        ));
      gBS->FreePages (Address, Pages);
      Status = EFI_DEVICE_ERROR;
      break;
    }

    *Base  = Address;
    *Size  = FileSize;
    Status = EFI_SUCCESS;
    break;
  }

  FreePool (Handles);
  return Status;
}

/**
  Kettle: announce the initramfs to the kernel through /chosen.

  linux,initrd-start and linux,initrd-end are read by early_init_dt_check_for_initrd with
  of_read_number(prop, len/4), so each is written as one 64-bit big-endian cell pair.

  This is a no-op when \kettle\initramfs.img is absent, which is what makes the feature
  switchable from the ESP alone.
**/
STATIC
VOID
KettlePublishInitrd (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Base;
  UINTN                 Size;
  VOID                  *Fdt;
  INT32                 Chosen;
  INT32                 Result;

  Fdt = KettleGetFdt ();
  if (Fdt == NULL) {
    DEBUG ((DEBUG_WARN, "%a: no FDT in the configuration table\n", __FUNCTION__));
    return;
  }

  //
  // Kettle: connect the storage stack BEFORE looking for the ESP.
  //
  // BDS only installs the UFS FAT stack when it starts booting a device, and this runs earlier
  // than that. Without connecting here, \kettle cannot be opened however the file is named:
  // the failure is an empty volume list, not a long-filename problem.
  //
  EfiBootManagerConnectAll ();

  Status = KettleLoadInitrd (&Base, &Size);
  if (EFI_ERROR (Status)) {
    //
    // INFO, not an error: the normal case when the ESP deliberately carries no initramfs.
    //
    DEBUG ((DEBUG_WARN, "%a: no initramfs to publish (%r)\n", __FUNCTION__, Status));
    return;
  }

  if (FdtCheckHeader (Fdt) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: published blob is not an FDT\n", __FUNCTION__));
    return;
  }

  Chosen = FdtSubnodeOffset (Fdt, 0, "chosen");
  if (Chosen < 0) {
    //
    // A blob without /chosen would take a real root path as well; create it rather than
    // dropping the initrd on the floor.
    //
    Chosen = FdtAddSubnode (Fdt, 0, "chosen");
    if (Chosen < 0) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: no /chosen and could not add one (%d)\n",
        __FUNCTION__,
        Chosen
        ));
      return;
    }
  }

  Result = FdtSetPropU64 (Fdt, Chosen, "linux,initrd-start", (UINT64)Base);
  if (Result != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: FdtSetPropU64(linux,initrd-start) failed (%d); booting without the initrd\n",
      __FUNCTION__,
      Result
      ));
    return;
  }

  Result = FdtSetPropU64 (Fdt, Chosen, "linux,initrd-end", (UINT64)(Base + Size));
  if (Result != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: FdtSetPropU64(linux,initrd-end) failed (%d); booting without the initrd\n",
      __FUNCTION__,
      Result
      ));
    return;
  }

  DEBUG ((
    DEBUG_WARN,
    "%a: /chosen initrd 0x%lx..0x%lx (%lu KiB)\n",
    __FUNCTION__,
    (UINT64)Base,
    (UINT64)(Base + Size),
    (UINT64)(Size >> 10)
    ));
}

//
// Kettle: the one logging mechanism.
//
// Mu is the only component that runs both BEFORE the kernel (so it can inspect what the previous
// boot left in memory) and with working storage (so the result survives a power cycle). Every
// other channel was tried and failed on this device:
//
//   ramoops cmdline    record is in DRAM; a power cycle or entering EDL wipes it
//   pstore_blk cmdline writes to flash but never registered here
//   last-boot.log      needs the initramfs premount hook, which is never reached
//   EDL peek           rejected by this loader: "Peek command isn't supported by edl loader"
//
// So Mu collects, and the kernel needs no logging configuration at all.
//
#define KETTLE_SCREEN_BMP   L"\\kettle\\last-screen.bmp"
#define KETTLE_RAMOOPS_BIN  L"\\kettle\\last-ramoops.bin"
#define KETTLE_BOOT_TXT     L"\\kettle\\last-boot.txt"

//
// Whether each capture actually reached the disk. Recorded to KETTLE_BOOT_TXT so a boot that
// ran but failed to capture is distinguishable from a boot that never happened.
//
STATIC BOOLEAN  mKettleScreenOk = FALSE;
STATIC BOOLEAN  mKettleDramOk   = FALSE;

//
// The ramoops reservation declared by the DTS. Reading this window is what makes the kernel's own
// console log reachable: the kernel keeps writing it, and Mu copies it to flash on the next boot
// before anything overwrites DRAM.
//
#define KETTLE_RAMOOPS_BASE  0x86E00000ULL
#define KETTLE_RAMOOPS_SIZE  0x00200000ULL

/**
  Kettle: open a file for writing on the volume that already carries \kettle.

  Requiring the \kettle directory to exist means a stray FAT partition elsewhere on the device
  cannot receive the capture. Returns NULL rather than failing the boot; logging must never stop
  a boot from happening.

  @param[in]  Path  Absolute path on the ESP, e.g. L"\\kettle\\last-screen.bmp".
  @param[out] File  Receives the open file handle on success.

  @retval EFI_SUCCESS  Open and ready to write.
  @retval Other        Not opened; the caller carries on.
**/
STATIC
EFI_STATUS
KettleOpenEspFile (
  IN  CONST CHAR16      *Path,
  OUT EFI_FILE_PROTOCOL **File
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *Dir;
  EFI_HANDLE                       *Handles;
  UINTN                             HandleCount;
  UINTN                             Index;

  *File = NULL;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (
                     Handles[Index],
                     &gEfiSimpleFileSystemProtocolGuid,
                     (VOID **)&Fs
                     )))
    {
      continue;
    }

    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    if (EFI_ERROR (Root->Open (Root, &Dir, L"\\kettle", EFI_FILE_MODE_READ, 0))) {
      Root->Close (Root);
      continue;
    }

    Dir->Close (Dir);

    Status = Root->Open (
                     Root,
                     File,
                     (CHAR16 *)Path,
                     EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ,
                     0
                     );
    Root->Close (Root);
    break;
  }

  FreePool (Handles);
  return Status;
}

/**
  Kettle: save the framebuffer as a BMP.

  Runs before the boot logo is drawn, so what it records is what the PREVIOUS boot left on screen.
  32bpp BI_RGB bottom-up is byte-identical to GOP's PixelBlueGreenRedReserved8BitPerColor, so rows
  are streamed last-to-first with no conversion and no multi-megabyte allocation.
**/
STATIC
VOID
KettleCaptureScreen (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  EFI_FILE_PROTOCOL            *File;
  UINT8                        *Header;
  UINT8                        *Row;
  UINTN                         RowBytes;
  UINTN                         RowStride;
  UINTN                         Stride;
  UINTN                         Size;
  UINT32                        Width;
  UINT32                        Height;
  UINT32                        Y;
  UINT32                        FileSize;
  UINT32                        Value32;
  UINT16                        Value16;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    return;
  }

  if ((Gop->Mode->FrameBufferBase == 0) || (Gop->Mode->FrameBufferSize == 0)) {
    return;
  }

  //
  // Only BGRA maps byte-for-byte onto a 32bpp BMP; anything else would produce a misleading image.
  //
  if (Gop->Mode->Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) {
    DEBUG ((
      DEBUG_WARN,
      "%a: pixel format %d is not BGRA; not capturing\n",
      __FUNCTION__,
      Gop->Mode->Info->PixelFormat
      ));
    return;
  }

  Width  = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution;
  if ((Width == 0) || (Height == 0)) {
    return;
  }

  //
  // Two different widths, deliberately: the pitch to walk in the framebuffer, and the row size a
  // BMP expects. The GOP keeps the visible width and the row pitch as separate fields, and
  // display controllers normally pad the pitch to a 64/128/256-byte boundary, so walking with
  // Width * 4 shifted every captured row by the padding -- which reads as banding and points the
  // diagnosis at the panel instead of at this loop. The BMP row size must stay Width * 4, because
  // BMP rows are padded to a 4-byte boundary and Width * 4 always satisfies that; writing the
  // padded pitch instead would emit a malformed header/row combination.
  //
  Stride = Gop->Mode->Info->PixelsPerScanLine;
  if ((Stride == 0) || (Stride < Width)) {
    Stride = Width;
  }

  RowBytes  = (UINTN)Width * 4;
  RowStride = (UINTN)Stride * 4;
  FileSize  = (UINT32)(54 + RowBytes * Height);

  Header = AllocateZeroPool (54);
  Row    = AllocatePool (RowStride);
  if ((Header == NULL) || (Row == NULL)) {
    if (Header != NULL) {
      FreePool (Header);
    }

    if (Row != NULL) {
      FreePool (Row);
    }

    return;
  }

  //
  // CopyMem rather than a cast: the header sits at an arbitrary stack address and an unaligned
  // UINT32 store is not something to depend on in firmware.
  //
  Header[0] = 'B';
  Header[1] = 'M';
  Value32 = FileSize;
  CopyMem (&Header[2], &Value32, sizeof (Value32));
  Value32 = 54;
  CopyMem (&Header[10], &Value32, sizeof (Value32));
  Value32 = 40;
  CopyMem (&Header[14], &Value32, sizeof (Value32));
  Value32 = Width;
  CopyMem (&Header[18], &Value32, sizeof (Value32));
  Value32 = Height;
  CopyMem (&Header[22], &Value32, sizeof (Value32));
  Value16 = 1;
  CopyMem (&Header[26], &Value16, sizeof (Value16));
  Value16 = 32;
  CopyMem (&Header[28], &Value16, sizeof (Value16));
  Value32 = (UINT32)(RowBytes * Height);
  CopyMem (&Header[34], &Value32, sizeof (Value32));

  if (EFI_ERROR (KettleOpenEspFile (KETTLE_SCREEN_BMP, &File))) {
    DEBUG ((DEBUG_WARN, "%a: cannot open %s\n", __FUNCTION__, KETTLE_SCREEN_BMP));
    FreePool (Row);
    FreePool (Header);
    return;
  }

  Size = 54;
  File->Write (File, &Size, Header);

  //
  // BMP rows are bottom-up, so the final framebuffer row goes first.
  //
  for (Y = Height; Y > 0; Y--) {
    //
    // Copy the visible part of the row: the framebuffer row is RowStride bytes, the BMP row is
    // RowBytes. A short read here is the padding, and it must not reach the file.
    //
    CopyMem (Row, (UINT8 *)(UINTN)Gop->Mode->FrameBufferBase + (UINTN)(Y - 1) * RowStride, RowBytes);
    Size = RowBytes;
    if (EFI_ERROR (File->Write (File, &Size, Row))) {
      break;
    }
  }

  File->Flush (File);
  File->Close (File);
  FreePool (Row);
  FreePool (Header);
  mKettleScreenOk = TRUE;

  DEBUG ((
    DEBUG_WARN,
    "%a: %ux%u screen -> %s (%u bytes)\n",
    __FUNCTION__,
    Width,
    Height,
    KETTLE_SCREEN_BMP,
    FileSize
    ));
}

/**
  Kettle: save the ramoops DRAM window so the kernel's own console log survives.

  The kernel writes its console into this reservation continuously (pstore_console with
  CON_PRINTBUFFER, registered at postcore_initcall). That memory is DRAM, so a power cycle would
  lose it -- but the NEXT boot's firmware runs before any kernel wipes it, which is exactly the
  window this capture uses. The result is a normal capture file the kernel itself produced, with
  no cmdline tokens and no pstore_blk involvement.
**/
STATIC
VOID
KettleCaptureDram (
  VOID
  )
{
  EFI_FILE_PROTOCOL  *File;
  VOID               *Buffer;
  UINTN               Size;
  UINTN               Written;

  Buffer = AllocatePool ((UINTN)KETTLE_RAMOOPS_SIZE);
  if (Buffer == NULL) {
    DEBUG ((DEBUG_WARN, "%a: no pool for the ramoops window\n", __FUNCTION__));
    return;
  }

  CopyMem (Buffer, (VOID *)(UINTN)KETTLE_RAMOOPS_BASE, (UINTN)KETTLE_RAMOOPS_SIZE);

  if (EFI_ERROR (KettleOpenEspFile (KETTLE_RAMOOPS_BIN, &File))) {
    DEBUG ((DEBUG_WARN, "%a: cannot open %s\n", __FUNCTION__, KETTLE_RAMOOPS_BIN));
    FreePool (Buffer);
    return;
  }

  Written = (UINTN)KETTLE_RAMOOPS_SIZE;
  Size    = Written;
  File->Write (File, &Size, Buffer);
  File->Flush (File);
  File->Close (File);
  FreePool (Buffer);
  mKettleDramOk = TRUE;

  DEBUG ((
    DEBUG_WARN,
    "%a: ramoops 0x%lx + 0x%lx -> %s (%lu bytes)\n",
    __FUNCTION__,
    (UINT64)KETTLE_RAMOOPS_BASE,
    (UINT64)KETTLE_RAMOOPS_SIZE,
    KETTLE_RAMOOPS_BIN,
    (UINT64)Written
    ));
}

/**
  Kettle: leave a readable trace of this boot on the ESP.

  Reuses the KettleSetStage value that already survives a reset, so the file says how far the boot
  got as well as whether the captures worked. Written last, after both captures, because that is
  the only point at which their outcome is known.

  A missing file means Mu never ran; a present file with screen=0 means it ran but could not save
  the framebuffer. Those two cases used to look identical on the ESP.
**/
STATIC
VOID
KettleWriteBootRecord (
  VOID
  )
{
  CHAR8        Text[160];
  UINTN        Length;
  UINT32       Stage;
  UINTN        Size;
  EFI_FILE_PROTOCOL  *File;

  Stage  = KETTLE_STAGE_IDLE;
  Size   = sizeof (Stage);
  if (EFI_ERROR (gRT->GetVariable (
                        L"KettleBootStage",
                        &mKettleSettingsGuid,
                        NULL,
                        &Size,
                        &Stage
                        )))
  {
    Stage = KETTLE_STAGE_IDLE;
  }

  Length = (UINTN)AsciiSPrint (
                     Text,
                     sizeof (Text),
                     "stage=%u screen=%u ramoops=%u\r\n",
                     Stage,
                     mKettleScreenOk ? 1 : 0,
                     mKettleDramOk ? 1 : 0
                     );

  if (EFI_ERROR (KettleOpenEspFile (KETTLE_BOOT_TXT, &File))) {
    return;
  }

  if (!EFI_ERROR (File->Write (File, &Length, Text))) {
    File->Flush (File);
  }

  File->Close (File);

  DEBUG ((DEBUG_WARN, "%a: %a", __FUNCTION__, Text));
}

VOID
EFIAPI
PlatformBootManagerAfterConsole ()
{
  EFI_STATUS                    Status      = EFI_SUCCESS;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *GopProtocol = NULL;

  //
  // Kettle: connect the storage stack before touching the ESP.
  //
  // BDS only installs the UFS FAT stack when it starts booting a device, so at this point
  // there are no SimpleFileSystem handles and every ESP write here would return silently.
  // KettlePublishInitrd documents the same trap; the captures predate that finding, which is
  // why several boots left the ESP byte-identical to the image that was flashed.
  //
  EfiBootManagerConnectAll ();

  //
  // Kettle: collect what the previous boot left behind, before anything draws over it.
  // The screen shows what the user saw; the ramoops window holds the kernel's console.
  // Both are read here because Mu is the last code that runs while that memory is still
  // intact, and the only one that can put the result on flash.
  //
  KettleCaptureScreen ();
  KettleCaptureDram ();
  KettleWriteBootRecord ();

  // Post Setup GOP
  PostGopSetup (&GopProtocol);

  // Display Boot Logo
  Status = DisplayBootGraphic (BG_SYSTEM_LOGO);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display Boot Logo! Status = %r\n", __FUNCTION__, Status));
  }

  // Kettle: load persistent settings and the font used by the sized hint
  KettleLoadSettings ();
  gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL, (VOID **)&mHiiFont);
  KettleSetStage (KETTLE_STAGE_CONSOLE);

  // Register Key Callback
  Status = RegisterKeyCallback ((EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath);
  if (GopProtocol != NULL) {
    // Set Screen Resolution
    UINT32 ScreenWidth  = GopProtocol->Mode->Info->HorizontalResolution;
    UINT32 ScreenHeight = GopProtocol->Mode->Info->VerticalResolution;

    mGop = GopProtocol;

    //
    // Kettle: position both hints for the console cell size rather than the
    // native glyph, stacked just above the bottom edge.
    //
    XPos  = (ScreenWidth - AsciiStrLen (ComboMessage) * mKettleHintCellW) / 2;
    YPos  = ScreenHeight - (mKettleHintCellH * 5) / 2;
    XPos2 = (ScreenWidth - AsciiStrLen (SettingsMessage) * mKettleHintCellW) / 2;
    YPos2 = ScreenHeight - (mKettleHintCellH * 3) / 2;

    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: no key callback (%r); forcing the boot manager via \\EFI\\kettle\\boot.txt\n",
        __FUNCTION__,
        Status
        ));
    }
  }

  // Kettle: report how the last boot ended, before anything else draws
  KettleReportLastStage ();

  // Execute Secondary After Console
  DeviceBootManagerAfterConsole ();

  //
  // Kettle: give the device tree the RAM size the kernel needs. Must happen
  // before BDS hands control to a loader, since the kernel reads /memory from
  // the published FDT and never sees the UEFI memory map.
  //
  KettlePatchFdtMemory ();

  //
  // Kettle: load \kettle\initramfs.img, if the ESP has one, and announce it in
  // /chosen. The kernel's EFI stub cannot do this itself -- it has no initrd=
  // load option -- and the initramfs is what performs SteamOS slot selection
  // and builds the /etc overlay. Absent the file this is a no-op.
  //
  KettlePublishInitrd ();

  //
  // Kettle: try the Kettle/SteamOS ESP before BDS falls through to the
  // platform's own boot targets (see KettleRegisterEspBootOption).
  //
  KettleRegisterEspBootOption ();

  //
  // Last point under Mu's control before BDS transfers to a loader. If the
  // next boot still sees this stage, control never came back.
  //
  KettleSetStage (KETTLE_STAGE_BOOTING);
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
  //
  // Kettle: a boot option returned, so the loader handed control back rather
  // than taking the machine down. Recorded so the next boot can tell the two
  // apart without the panel.
  //
  KettleSetStage (KETTLE_STAGE_RETURNED);
  return;
}

VOID
EFIAPI
PlatformBootManagerPriorityBoot (IN OUT UINT16 **BootNext)
{
  EFI_STATUS                   Status;
  EFI_BOOT_MANAGER_LOAD_OPTION LoadOption;
  BOOLEAN                      BootManagerFlag;
  BOOLEAN                      ShowMenu;

  // Unregister Key Callback
  UnregisterKeyCallback ();

  //
  // Kettle: the settings screen takes priority over the boot menu.
  //
  ShowMenu = FALSE;
  if (EnterSettings ()) {
    ShowMenu = KettleSettingsUi ();
    if (!ShowMenu) {
      //
      // Timed out with no input: carry on booting normally.
      //
      return;
    }
  }

  // Get Boot Manager Flag
  BootManagerFlag = ShowMenu || EnterBootManager ();
  if (BootManagerFlag == FALSE) {
    //
    // Kettle: honour the persisted request from \\EFI\\kettle\\boot.txt so the
    // boot manager is reachable without a key press.
    //
    if (!KettleForceMenu ()) {
      return;
    }
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
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Color;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Background;
  CHAR16                         Message[64];
  UINT16                         Timeout;
  UINT8                          DrawColor;

  // Verify Combo Message Position
  if ((XPos == 0 && YPos == 0) || (mGop == NULL) || (mHiiFont == NULL)) {
    return;
  }

  // Get Timeout Time
  Timeout = PcdGet16 (PcdPlatformBootTimeOut);

  // Check Remaining Timeout
  if (TimeoutRemain == Timeout || TimeoutRemain == 0) {
    // Set Draw Color: light on first paint, black to erase on the last call
    DrawColor = (TimeoutRemain == Timeout) ? 0xFF : 0x00;

    Color.Red      = DrawColor;
    Color.Green    = DrawColor;
    Color.Blue     = DrawColor;
    Color.Reserved = 0;

    Background.Red      = 0;
    Background.Green    = 0;
    Background.Blue     = 0;
    Background.Reserved = 0;

    // Print Combo Message at the console cell size
    AsciiStrToUnicodeStrS (ComboMessage, Message, ARRAY_SIZE (Message));
    DrawScaledString (
      mGop,
      XPos,
      YPos,
      &Color,
      &Background,
      Message,
      mKettleHintCellW,
      mKettleHintCellH
      );

    AsciiStrToUnicodeStrS (SettingsMessage, Message, ARRAY_SIZE (Message));
    DrawScaledString (
      mGop,
      XPos2,
      YPos2,
      &Color,
      &Background,
      Message,
      mKettleHintCellW,
      mKettleHintCellH
      );
  }
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot ()
{
  EFI_STATUS Status;

  // Execute Secondary Unable To Boot
  DeviceBootManagerUnableToBoot ();

  // Display No Boot OS Logo
  Status = DisplayBootGraphic (BG_NO_BOOT_OS);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display No Boot OS Logo! Status = %r\n", __FUNCTION__, Status));
    Print (L"No Operating System Found! Please make sure that an OS is Installed.\n");
  }

  // Wait 10s
  gBS->Stall (10000000);

  // Shutdown
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);

  // Do Cpu Dead Loop
  CpuDeadLoop ();
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
