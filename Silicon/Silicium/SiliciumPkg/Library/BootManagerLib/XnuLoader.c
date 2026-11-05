/**
  XNU loader for ingres, diagnostics stage (ported from HakiOS's redwood port).

  Runs when BDS finds nothing to boot. Prints, step by step, what the real
  loader will need: exception level, CPU registers, framebuffer, memory map,
  payload.  Every stage announces itself before it runs, and every line goes
  straight to the GOP console, so the last line on screen names the stage that
  failed if the platform aborts.  The system is not reset: the screen is left
  up for the operator.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PartitionInfo.h>

#include "XnuLoader.h"

#define LOG_SIZE  SIZE_256KB

STATIC CHAR8  *mLog;
STATIC UINTN   mLogLen;

STATIC
VOID
EmitToConsole (
  IN CONST CHAR8 *Text)
{
  CHAR16 Ch[2];

  if (gST->ConOut == NULL) {
    return;
  }

  Ch[1] = L'\0';

  for (; *Text != '\0'; Text++) {
    if (*Text == '\n') {
      Ch[0] = L'\r';
      gST->ConOut->OutputString (gST->ConOut, Ch);
      Ch[0] = L'\n';
      gST->ConOut->OutputString (gST->ConOut, Ch);
    } else {
      Ch[0] = (CHAR16)(UINT8)*Text;
      gST->ConOut->OutputString (gST->ConOut, Ch);
    }
  }
}

STATIC
VOID
EFIAPI
Log (
  IN CONST CHAR8 *Format,
  ...)
{
  VA_LIST Marker;
  UINTN   OldLen;

  if (mLog == NULL || mLogLen >= LOG_SIZE - 512) {
    return;
  }

  OldLen = mLogLen;

  VA_START (Marker, Format);
  mLogLen += AsciiVSPrint (mLog + mLogLen, LOG_SIZE - mLogLen, Format, Marker);
  VA_END (Marker);

  if (mLogLen > OldLen) {
    EmitToConsole (mLog + OldLen);
  }
}

#define READ_SYSREG(Name) ({ UINT64 _Value; __asm__ volatile ("mrs %0, " #Name : "=r" (_Value)); _Value; })

STATIC
EFI_BLOCK_IO_PROTOCOL *
FindPartition (
  IN CONST CHAR16 *Name)
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles;
  UINTN       Count;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  for (UINTN i = 0; i < Count; i++) {
    EFI_PARTITION_INFO_PROTOCOL *Info;
    EFI_BLOCK_IO_PROTOCOL       *BlockIo;

    Status = gBS->HandleProtocol (Handles[i], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (EFI_ERROR (Status) || Info->Type != PARTITION_TYPE_GPT) {
      continue;
    }

    if (StrnCmp (Info->Info.Gpt.PartitionName, Name, 36) != 0) {
      continue;
    }

    Status = gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (!EFI_ERROR (Status)) {
      return BlockIo;
    }
  }

  return NULL;
}

STATIC
VOID
LogCpu ()
{
  Log ("S3 cpu: CurrentEL=%lx\n", READ_SYSREG (CurrentEL) >> 2);
  Log ("   MIDR_EL1=%016lx\n", READ_SYSREG (MIDR_EL1));
  Log ("   ID_AA64PFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64PFR0_EL1));
  Log ("   ID_AA64MMFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR0_EL1));
  Log ("   ID_AA64MMFR1_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR1_EL1));
  Log ("   SCTLR_EL1=%016lx\n", READ_SYSREG (SCTLR_EL1));
  Log ("   TCR_EL1=%016lx\n", READ_SYSREG (TCR_EL1));
  Log ("   MAIR_EL1=%016lx\n", READ_SYSREG (MAIR_EL1));
  Log ("   TTBR0_EL1=%016lx\n", READ_SYSREG (TTBR0_EL1));
  Log ("   VBAR_EL1=%016lx\n", READ_SYSREG (VBAR_EL1));
  Log ("   MPIDR_EL1=%016lx\n", READ_SYSREG (MPIDR_EL1));
  Log ("   CNTFRQ_EL0=%ld\n", READ_SYSREG (CNTFRQ_EL0));
  Log ("S3 cpu done\n");
}

STATIC
VOID
LogDisplay ()
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

  Log ("S4 display: locate gop\n");

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Log ("   gop=missing status=%r\n", Status);
    return;
  }

  Log ("   fb_base=%lx fb_size=%lx\n", Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);
  Log ("   width=%d height=%d stride=%d format=%d\n",
       Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
       Gop->Mode->Info->PixelsPerScanLine, Gop->Mode->Info->PixelFormat);
  Log ("S4 display done\n");
}

STATIC
VOID
LogMemoryMap ()
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR *Map;
  UINTN                  MapSize = 0;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  UINTN                  Count;

  Log ("S5 memmap: get size\n");

  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Log ("   getmemorymap status=%r\n", Status);
    return;
  }

  Log ("   size=%ld desc=%ld\n", MapSize, DescSize);

  MapSize += SIZE_4KB;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    Log ("   alloc failed\n");
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    Log ("   getmemorymap status=%r\n", Status);
    return;
  }

  Count = MapSize / DescSize;
  Log ("   entries=%ld\n", Count);

  for (UINTN i = 0; i < Count; i++) {
    EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + i * DescSize);

    Log ("   %016lx-%016lx t=%d a=%lx\n", Desc->PhysicalStart,
         Desc->PhysicalStart + EFI_PAGES_TO_SIZE (Desc->NumberOfPages), Desc->Type, Desc->Attribute);
  }

  FreePool (Map);
  Log ("S5 memmap done\n");
}

STATIC
VOID
LogPayload ()
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;

  Log ("S6 payload: read %lx\n", (UINT64)XNU_PAYLOAD_BASE);
  Log ("   magic=%08x size=%x\n", Header->Magic, Header->Size);
  Log ("S6 payload done\n");
}

STATIC
VOID
LogPersistence ()
{
  UINT32 *Ram = (UINT32 *)(UINTN)(XNU_PAYLOAD_BASE + XNU_PAYLOAD_MAX_SIZE - SIZE_4KB);

  Log ("S7 persistence: read %lx\n", (UINT64)(UINTN)Ram);
  Log ("   before=%08x %08x\n", Ram[0], Ram[1]);

  Ram[1] = (Ram[0] == XNU_MARKER_MAGIC) ? Ram[1] + 1 : 1;
  Ram[0] = XNU_MARKER_MAGIC;

  Log ("   after=%08x %08x\n", Ram[0], Ram[1]);
  Log ("S7 persistence done\n");
}

STATIC
VOID
StartPayloadLoader ()
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;
  UINT8              *Cursor;
  UINT8              *End;

  Log ("S8 loader: check payload header\n");

  if (Header->Magic != XNU_PAYLOAD_MAGIC || Header->Size > XNU_PAYLOAD_MAX_SIZE) {
    Log ("   no loader in payload\n");
    return;
  }

  Cursor = (UINT8 *)(Header + 1);
  End    = Cursor + Header->Size;

  while (Cursor + sizeof (XNU_PAYLOAD_ENTRY) <= End) {
    XNU_PAYLOAD_ENTRY *Entry = (XNU_PAYLOAD_ENTRY *)Cursor;

    if (Entry->Name[0] == 0) {
      break;
    }

    if (AsciiStrnCmp (Entry->Name, "loader.efi", sizeof (Entry->Name)) == 0) {
      EFI_HANDLE Handle;

      Log ("   starting loader.efi size=%x\n", Entry->Size);
      if (!EFI_ERROR (gBS->LoadImage (FALSE, gImageHandle, NULL, Entry + 1, Entry->Size, &Handle))) {
        gBS->StartImage (Handle, NULL, NULL);
      }
      Log ("   loader returned\n");
      return;
    }

    Cursor += sizeof (XNU_PAYLOAD_ENTRY) + ALIGN_VALUE (Entry->Size, 16);
  }
}

VOID
XnuLoaderMain ()
{
  EFI_TIME Time;

  Log ("S1 enter XnuLoaderMain\n");

  mLog = AllocateZeroPool (LOG_SIZE);
  if (mLog == NULL) {
    CpuDeadLoop ();
  }

  Log ("S2 banner: == XNULOADER LOG v2 (ingres) ==\n");

  if (!EFI_ERROR (gRT->GetTime (&Time, NULL))) {
    Log ("   time=%04d-%02d-%02d %02d:%02d:%02d\n", Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second);
  } else {
    Log ("   time=unavailable\n");
  }

  LogCpu ();
  LogDisplay ();
  LogMemoryMap ();
  LogPayload ();
  LogPersistence ();
  StartPayloadLoader ();

  // Diagnostics are on the screen: leave them there instead of resetting.
  Log ("S9 END. Screen left up; hold Power+VolUp to reboot.\n");
  CpuDeadLoop ();
}
