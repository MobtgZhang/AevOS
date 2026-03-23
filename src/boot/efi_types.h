#pragma once

/*
 * Self-contained UEFI type definitions.
 * No dependency on gnu-efi or EDK2 headers.
 */

typedef unsigned char       BOOLEAN;
typedef signed char         INT8;
typedef unsigned char       UINT8;
typedef signed short        INT16;
typedef unsigned short      UINT16;
typedef signed int          INT32;
typedef unsigned int        UINT32;
typedef signed long long    INT64;
typedef unsigned long long  UINT64;
typedef unsigned long long  UINTN;
typedef signed long long    INTN;
typedef UINT16              CHAR16;
typedef void                VOID;

typedef UINTN               EFI_STATUS;
typedef VOID               *EFI_HANDLE;
typedef VOID               *EFI_EVENT;
typedef UINT64              EFI_PHYSICAL_ADDRESS;
typedef UINT64              EFI_VIRTUAL_ADDRESS;
typedef UINTN               EFI_TPL;

#define IN
#define OUT
#define OPTIONAL
#define CONST const

/* Calling convention: MS ABI on x86-64, standard on others */
#if defined(__x86_64__)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

/* Status codes */
#define EFI_SUCCESS              0ULL
#define EFI_ERROR_BIT            (1ULL << 63)
#define EFI_ERROR(s)             ((s) | EFI_ERROR_BIT)
#define EFI_LOAD_ERROR           EFI_ERROR(1)
#define EFI_INVALID_PARAMETER    EFI_ERROR(2)
#define EFI_UNSUPPORTED          EFI_ERROR(3)
#define EFI_BUFFER_TOO_SMALL     EFI_ERROR(5)
#define EFI_OUT_OF_RESOURCES     EFI_ERROR(9)
#define EFI_NOT_FOUND            EFI_ERROR(14)

#define NULL_HANDLE ((EFI_HANDLE)0)

/* ── EFI_GUID ─────────────────────────────────────────── */

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* ── EFI_TABLE_HEADER ─────────────────────────────────── */

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ── Memory map types ─────────────────────────────────── */

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32                Type;
    EFI_PHYSICAL_ADDRESS  PhysicalStart;
    EFI_VIRTUAL_ADDRESS   VirtualStart;
    UINT64                NumberOfPages;
    UINT64                Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ── Graphics Output Protocol ─────────────────────────── */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber,
    UINTN *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32 ModeNumber);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    VOID *BltBuffer, UINTN BltOperation,
    UINTN SourceX, UINTN SourceY,
    UINTN DestinationX, UINTN DestinationY,
    UINTN Width, UINTN Height, UINTN Delta);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT        Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ── Simple File System / File Protocol ───────────────── */

struct _EFI_FILE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    struct _EFI_FILE_PROTOCOL *This,
    struct _EFI_FILE_PROTOCOL **NewHandle,
    CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);

typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(
    struct _EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    struct _EFI_FILE_PROTOCOL *This,
    UINTN *BufferSize, VOID *Buffer);

typedef struct _EFI_FILE_PROTOCOL {
    UINT64          Revision;
    EFI_FILE_OPEN   Open;
    EFI_FILE_CLOSE  Close;
    VOID           *Delete;
    EFI_FILE_READ   Read;
    VOID           *Write;
    VOID           *GetPosition;
    VOID           *SetPosition;
    VOID           *GetInfo;
    VOID           *SetInfo;
    VOID           *Flush;
} EFI_FILE_PROTOCOL;

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
    struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL **Root);

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64                                        Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME   OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_FILE_MODE_READ  0x0000000000000001ULL

/* ── Simple Text Output ───────────────────────────────── */

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16 *String);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    VOID                    *Reset;
    EFI_TEXT_STRING          OutputString;
    VOID                    *TestString;
    VOID                    *QueryMode;
    VOID                    *SetMode;
    VOID                    *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN    ClearScreen;
    VOID                    *SetCursorPosition;
    VOID                    *EnableCursor;
    VOID                    *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ── Boot Services ────────────────────────────────────── */

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
    UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID *Protocol, VOID *Registration, VOID **Interface);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle, UINTN MapKey);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);

typedef struct {
    EFI_TABLE_HEADER        Hdr;
    VOID                   *RaiseTPL;
    VOID                   *RestoreTPL;
    EFI_ALLOCATE_PAGES      AllocatePages;
    EFI_FREE_PAGES          FreePages;
    EFI_GET_MEMORY_MAP      GetMemoryMap;
    EFI_ALLOCATE_POOL       AllocatePool;
    EFI_FREE_POOL           FreePool;
    VOID                   *CreateEvent;
    VOID                   *SetTimer;
    VOID                   *WaitForEvent;
    VOID                   *SignalEvent;
    VOID                   *CloseEvent;
    VOID                   *CheckEvent;
    VOID                   *InstallProtocolInterface;
    VOID                   *ReinstallProtocolInterface;
    VOID                   *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL     HandleProtocol;
    VOID                   *Reserved;
    VOID                   *RegisterProtocolNotify;
    VOID                   *LocateHandle;
    VOID                   *LocateDevicePath;
    VOID                   *InstallConfigurationTable;
    VOID                   *LoadImage;
    VOID                   *StartImage;
    VOID                   *Exit;
    VOID                   *UnloadImage;
    EFI_EXIT_BOOT_SERVICES  ExitBootServices;
    VOID                   *GetNextMonotonicCount;
    VOID                   *Stall;
    VOID                   *SetWatchdogTimer;
    VOID                   *ConnectController;
    VOID                   *DisconnectController;
    VOID                   *OpenProtocol;
    VOID                   *CloseProtocol;
    VOID                   *OpenProtocolInformation;
    VOID                   *ProtocolsPerHandle;
    VOID                   *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL     LocateProtocol;
    VOID                   *InstallMultipleProtocolInterfaces;
    VOID                   *UninstallMultipleProtocolInterfaces;
} EFI_BOOT_SERVICES;

/* ── Configuration Table ──────────────────────────────── */

typedef struct {
    EFI_GUID    VendorGuid;
    VOID       *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ── System Table ─────────────────────────────────────── */

typedef struct {
    EFI_TABLE_HEADER                    Hdr;
    CHAR16                             *FirmwareVendor;
    UINT32                              FirmwareRevision;
    EFI_HANDLE                          ConsoleInHandle;
    VOID                               *ConIn;
    EFI_HANDLE                          ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL     *ConOut;
    EFI_HANDLE                          StandardErrorHandle;
    VOID                               *StdErr;
    VOID                               *RuntimeServices;
    EFI_BOOT_SERVICES                  *BootServices;
    UINTN                               NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE            *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ── Well-known GUIDs ─────────────────────────────────── */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, \
      { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, \
      { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, \
      { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
