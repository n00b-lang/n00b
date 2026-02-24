#pragma once

#if defined(_WIN32)

#include <stddef.h>
#include <stdint.h>

typedef int                BOOL;
typedef unsigned long      DWORD;
typedef unsigned long      ULONG;
typedef long               LONG;
typedef long               NTSTATUS;
typedef unsigned short     WORD;
typedef short              SHORT;
typedef unsigned char      BYTE;
typedef uint64_t           SIZE_T;
typedef uint64_t           ULONG_PTR;
typedef void              *PVOID;
typedef const void        *LPCVOID;
typedef void              *LPVOID;
typedef void              *HANDLE;
typedef void              *HMODULE;
typedef BYTE              *PUCHAR;

typedef struct {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    DWORD  AllocationProtect;
    WORD   PartitionId;
    SIZE_T RegionSize;
    DWORD  State;
    DWORD  Protect;
    DWORD  Type;
} MEMORY_BASIC_INFORMATION;

typedef struct {
    LPVOID lpBaseOfDll;
    DWORD  SizeOfImage;
    LPVOID EntryPoint;
} MODULEINFO;

typedef struct {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        } s;
    } u;
    DWORD     dwPageSize;
    LPVOID    lpMinimumApplicationAddress;
    LPVOID    lpMaximumApplicationAddress;
    ULONG_PTR dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors;
    DWORD     dwProcessorType;
    DWORD     dwAllocationGranularity;
    WORD      wProcessorLevel;
    WORD      wProcessorRevision;
} SYSTEM_INFO;

typedef struct {
    SHORT X;
    SHORT Y;
} COORD;

typedef struct {
    SHORT Left;
    SHORT Top;
    SHORT Right;
    SHORT Bottom;
} SMALL_RECT;

typedef struct {
    COORD      dwSize;
    COORD      dwCursorPosition;
    WORD       wAttributes;
    SMALL_RECT srWindow;
    COORD      dwMaximumWindowSize;
} CONSOLE_SCREEN_BUFFER_INFO;

#define INFINITE 0xffffffffUL

#define ERROR_INVALID_PARAMETER 87UL
#define ERROR_TIMEOUT           1460UL

#define MEM_COMMIT  0x00001000UL
#define MEM_RESERVE 0x00002000UL
#define MEM_RELEASE 0x00008000UL

#define PAGE_NOACCESS          0x01UL
#define PAGE_READONLY          0x02UL
#define PAGE_READWRITE         0x04UL
#define PAGE_WRITECOPY         0x08UL
#define PAGE_EXECUTE_READ      0x20UL
#define PAGE_EXECUTE_READWRITE 0x40UL
#define PAGE_EXECUTE_WRITECOPY 0x80UL
#define PAGE_GUARD             0x100UL

#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002UL
#define STD_OUTPUT_HANDLE       ((DWORD)-11)

extern BOOL     WaitOnAddress(volatile void *address,
                              const void    *compare_address,
                              SIZE_T         address_size,
                              DWORD          milliseconds);
extern void     WakeByAddressSingle(void *address);
extern void     WakeByAddressAll(void *address);
extern DWORD    GetLastError(void);
extern HANDLE   GetStdHandle(DWORD std_handle);
extern BOOL     GetConsoleScreenBufferInfo(HANDLE                       console_output,
                                           CONSOLE_SCREEN_BUFFER_INFO *console_screen_buffer_info);
extern HMODULE  LoadLibraryA(const char *library_name);
extern void    *GetProcAddress(HMODULE module, const char *proc_name);
extern BOOL     FreeLibrary(HMODULE module);
extern DWORD    GetCurrentThreadId(void);
extern void     GetCurrentThreadStackLimits(ULONG_PTR *low_limit, ULONG_PTR *high_limit);
extern void     GetSystemInfo(SYSTEM_INFO *system_info);
extern HANDLE   GetCurrentProcess(void);
extern BOOL     EnumProcessModules(HANDLE process,
                                   HMODULE *modules,
                                   DWORD    cb,
                                   DWORD   *needed);
extern BOOL     GetModuleInformation(HANDLE      process,
                                     HMODULE     module,
                                     MODULEINFO *module_info,
                                     DWORD       cb);
extern LPVOID   VirtualAlloc(LPVOID address,
                             SIZE_T size,
                             DWORD  allocation_type,
                             DWORD  protect);
extern BOOL     VirtualFree(LPVOID address, SIZE_T size, DWORD free_type);
extern SIZE_T   VirtualQuery(LPCVOID address, MEMORY_BASIC_INFORMATION *buffer, SIZE_T length);
extern NTSTATUS BCryptGenRandom(void   *algorithm,
                                PUCHAR  buffer,
                                ULONG   cb_buffer,
                                ULONG   flags);

#endif
