/* orly32.h - everything orlyfix knows about the inside of ORLY32.EXE (634,880 bytes,
 * Broderbund 1997; image base 0x400000, no relocations, so these addresses are fixed).
 * No other source file contains a game address or instruction bytes. Imported Windows
 * functions need no addresses here: hook_import() finds them in the EXE's import table.
 *
 * Every name says what kind of thing it is:
 *   VAR_<name>      address of a game variable
 *   FN_<name>       address of a game function that orlyfix calls
 *   RET_<name>      return address inside game code, used to recognise a caller
 *   PATCH_<name>    address of an instruction orlyfix rewrites (checked against BYTES_ first)
 *   BYTES_<name>    instruction bytes, as an initializer list: what is expected or written
 *   OP_<name>       x86 opcode bytes that orlyfix builds instructions from
 *   CODE_<name>     game code referred to in comments, recorded here for reference
 */
#pragma once

/* ---- x86 opcodes ---- */
#define OP_CALL_REL32        0xE8           /* call rel32 (5 bytes) */
#define OP_JMP_REL32         0xE9           /* jmp rel32 (5 bytes) */
#define OP_NOP               0x90
#define REL32_INSN_SIZE      5              /* call/jmp rel32: opcode + 4-byte offset */

/* ---- game variables ---- */
#define VAR_MAIN_HWND        0x4b1638u      /* HWND: the game's main window */
#define VAR_SAVED_FILES      0x4a2194u      /* BYTE *: table of saved stories/drawings, 0x1f8
                                               bytes per entry; NULL once the menu frees it */

/* ---- game functions (Borland C++ runtime) ---- */
#define FN_RTL_MEMCPY        0x48cde0u      /* void *memcpy(void *, const void *, size_t), cdecl */
#define FN_RTL_PRINT_ERROR   0x492970u      /* prints a runtime error message, cdecl(const char *) */
#define FN_RTL_EXIT          0x492b50u      /* exit(int), cdecl */

/* ---- return addresses ---- */
#define RET_MAIN_LOOP_PEEK   0x4635acu      /* after the main loop's PeekMessageA call (the loop
                                               itself is CODE_MAIN_LOOP) */

/* ---- instructions orlyfix rewrites, and their bytes ---- */
/* Engine task stack sizes: each task creation pushes its stack size. */
#define PATCH_SOUND_TASK_STACK   0x457febu  /* push 2000h -> push 0F000h */
#define PATCH_TASK_STACK_2       0x419fadu  /* push 1000h -> push 0F000h (drawing-load task) */
#define PATCH_TASK_STACK_3       0x46927bu  /* push 1000h -> push 0F000h */
#define PATCH_TASK_STACK_4       0x48a5a3u  /* push 1000h -> push 0F000h */
#define BYTES_PUSH_1000H         { 0x68, 0x00, 0x10, 0x00, 0x00 }   /* push 1000h  (4 KB)  */
#define BYTES_PUSH_2000H         { 0x68, 0x00, 0x20, 0x00, 0x00 }   /* push 2000h  (8 KB)  */
#define BYTES_PUSH_F000H         { 0x68, 0x00, 0xF0, 0x00, 0x00 }   /* push 0F000h (60 KB, the
                                               most that fits the 64 KB reservation) */

/* Copy of a saved drawing's name out of VAR_SAVED_FILES during a load: call FN_RTL_MEMCPY. */
#define PATCH_SAVED_NAME_COPY    0x41892bu

/* Entry of the runtime's error exit (prints the message, then exit(1); used for "Pure virtual
   function called" and others), replaced by a jump in -DCRASHDUMP builds. */
#define PATCH_RTL_FATAL          0x492a30u
#define BYTES_RTL_FATAL_ENTRY    { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08 }  /* push ebp / mov ebp,esp /
                                                                           mov eax,[ebp+8] */

/* ---- game code referred to in comments ---- */
#define CODE_MAIN_LOOP           0x46359bu  /* WinMain's message/frame loop - never waits */
#define CODE_DISK_FREE_MULTIPLY  0x474341u  /* free space = clusters * sectors * bytes, signed
                                               32-bit: overflows above 2 GB */
#define CODE_TASK_CREATE         0x4861e4u  /* creates a cooperative engine task */
#define CODE_TASK_STACK_ALLOC    0x48763eu  /* allocates a task stack in a 64 KB reservation */
#define CODE_PRINTER_PORT_INIT   0x480e8du  /* printer port set-up: LoadLibraryA(driver ".DRV") */
#define CODE_SAVED_DRAWING_LOAD  0x4187f2u  /* engine task that loads a saved drawing */
#define CODE_FREE_SAVED_FILES    0x417350u  /* frees VAR_SAVED_FILES and its lists */
#define CODE_MENU_LEAVE_SCREEN   0x41f88fu  /* menu code that calls CODE_FREE_SAVED_FILES */
#define CODE_FILE_WORKER         0x48526du  /* engine file worker thread (THREAD_PRIORITY_HIGHEST) */
#define CODE_CHANGE_DISPLAY      0x47e954u  /* wrapper: ChangeDisplaySettingsA via GetProcAddress */
#define CODE_ENUM_DISPLAY        0x47e9e9u  /* wrapper: EnumDisplaySettingsA via GetProcAddress */
#define CODE_DIRECTDRAW_SETUP    0x47a1e9u  /* DirectDraw set-up: DirectDrawCreate via GetProcAddress */
#define CODE_PALETTE_MODE        0x47eab4u  /* sets system palette use, system colours, broadcasts */

