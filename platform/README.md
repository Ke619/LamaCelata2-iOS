# platform

Native implementations of the functions the game imports, plus native code the
game reaches through pointers (host functions), and the local replacement for the
Ubisoft launcher library.

Shims follow i386 cdecl on the guest stack (`scimitar/runtime.h`: `arg32`, `arg64`,
`shim_return`, `shim_return64`) and are named after the dyld symbol:
`_read$UNIX2003` → `scimitar::imports::imp_read_UNIX2003` (leading underscores
folded: `___cxa_atexit` → `imp_cxa_atexit`). Each file registers its table with
`SCIMITAR_REGISTER_SHIMS`; `find_import_shim` lets the Unicorn oracle run the same
shims; `set_import_trace` records calls so tests can compare engines.

Generated code built with `scimitar-recomp --weak-imports` has a weak stub for every
import that calls `fatal("import X is not implemented (called from ...)")`; link the
platform library with `-force_load` so its shims replace the stubs. Running more of
the game and implementing what the stubs report is how this layer grows.

| File | Imports |
|---|---|
| `src/posix_io.cpp` | `read`, `lseek` (guest file descriptors are host descriptors), `aio_read`/`aio_error`/`aio_return` (Brotherhood; done at once); the shim registry |
| `src/libc_mem.cpp` | `memcpy`, `memmove` (each copy is shown to the ARB program store) |
| `src/arb_store.cpp` | not an import: the ARB program store (`scimitar/arb_store.h`), on with `SCIMITAR_ARB_STORE=FILE` or the window layout: shader program text the loader copies into guest memory goes to a file, the guest block keeps a reference and is shrunk through the game's allocator (`games/common/main.cpp`), and `glProgramStringARB` reads the text back. 110 MB less guest memory in Florence |
| `src/guest_heap.cpp` | `malloc`, `calloc`, `realloc`, `free`, `malloc_size` on the guest heap (`scimitar/guest_heap.h`: 1.5 GB at `0x80000000`; with the compact layout at `0x08000000`, with the window layout from there to the window's end; size classes plus a merging best-fit map, thread-safe); `SCIMITAR_HEAP_REPORT=<seconds>` prints the live blocks by origin (guest call stack or host function) that long after the first allocation, with how much of each is still all zero and the most of one block that is not |
| `src/libc_string.cpp` | `mem*`, `bzero`, `str*` (incl. `strlcpy`, `strdup`), `__tolower`/`__toupper`, `wcs*`/`wmem*` on 32-bit `wchar_t` |
| `src/libc_printf.cpp` | `printf`, `sprintf`, `snprintf`, `vsprintf`, `vsnprintf`, `swprintf`, `vswprintf`: formats parsed with arguments from the guest `va_list` (`guest_format`) |
| `src/pthread.cpp` | guest threads (`pthread_create`, `pthread_create_suspended_np` + `thread_resume`, join, exit, attributes): host threads with their own `Context` and a guest stack (in the window layout at most 256 KB); mutexes, condition variables (incl. timed and relative waits), rwlocks, `pthread_once`, thread-specific data, `pthread_self` and Mach thread ports |
| `src/mach_sync.cpp` | `OSAtomicAdd32/64`, `OSAtomicCompareAndSwap32Barrier`, `OSSpinLock*` as host atomics; Mach semaphores; `mach_absolute_time`, `mach_timebase_info`, `gettimeofday`, `time` |
| `src/libc_misc.cpp` | `usleep`, `__cxa_atexit` (kept), `__stack_chk_fail`, `abort` (fatal), `asl_*` (the engine's logger: errors to stderr, all with `SCIMITAR_LOG=1`) |
| `src/bridge.cpp` | the object model Foundation and CoreFoundation share (toll-free): strings, arrays, dictionaries, numbers, booleans, data, URLs, bundles, dates...; the game's constant `CFSTR` strings (`__DATA,__cfstring`) are recognised by address; property lists are parsed with the host's CoreFoundation and converted. Objects are never freed (the game's CF/ObjC reference counting is not tracked) |
| `src/objc.cpp` | `objc_msgSend`, `objc_msgSend_fpret`: the game's Objective-C 1 class and selector references are name strings, so messages dispatch on (class or bridge object, selector). Foundation without UI: `NSString`, `NSArray`/`NSMutableArray`, `NSDictionary` (incl. plist files), `NSNumber`, `NSDate`, `NSBundle`, `NSFileManager`, `NSProcessInfo` (arguments from `set_process_arguments`), `NSLocale`, `NSRecursiveLock`, `NSNotificationCenter`, `NSTask` (not started), `NSThread sleepForTimeInterval:`, `NSAutoreleasePool`; `NSSearchPathForDirectoriesInDomains`, `NSLog`. The game sends 123 selectors in all; the application, window, OpenGL view and event ones are never sent, because the shells replace the game's `Mac*` functions that send them |
| `src/cf.cpp` | the 50 CoreFoundation functions the game imports (bundles and Info.plist values, strings in any encoding, URLs as paths, dictionaries, arrays, numbers, data, preferences, locale, alerts to stderr) and Carbon `FSPathMakeRef`, `GetCurrentProcess`, `GetProcessBundleLocation` |
| `src/libc_math.cpp` | libm, float/double results in x87 ST0; libgcc's 64-bit division (`__divdi3`, `__udivdi3`, `__moddi3`, `__umoddi3`) |
| `src/libc_conv.cpp` | `strto*`/`ato*`, ctype/locale ("C"), multibyte conversions, `sscanf`/`swscanf` storing through guest pointers, `strftime`/`wcsftime` |
| `src/libc_process.cpp` | `getenv`, `errno` (per thread), `_NSGetExecutablePath`, time (`gmtime`, `localtime`, `mktime`... on the 44-byte i386 `struct tm`), `qsort` with a guest comparator, `mach_vm_allocate`, `assert`, signals ignored, no `system`/`dlopen`; `popen` only answers Brotherhood's `system_profiler SPDisplaysDataType -xml` (this machine's GPU with 2 GB of video memory, which the game maps to its high settings; an unknown card gets the lowest and a warning); sockets, interface and name lookups fail (`ENETDOWN`: no online play) |
| `src/stdio.cpp` | `fopen` & co. (guest `FILE*` handles for host `FILE*`s), `fprintf` |
| `src/posix_fs.cpp` | `open`, `close`, `write`, `writev`, `stat` family in both i386 layouts (legacy and `$INODE64`), directories in both `dirent` layouts, `mkdir`, `unlink`, `getcwd`, `copyfile`... |
| `src/data_imports.cpp` | binds the pointer slots of imported symbols: functions passed by address get host functions for their shims; `stdin`/`stdout`/`stderr`, `_DefaultRuneLocale` (the ctype tables, i386 layout), `__mb_cur_max`, `mach_task_self_`, the stack guard and the CF/Cocoa constants get values |
| `src/opengl.cpp` | OpenGL, CGL and GLU (103 imports) passed to the host's legacy OpenGL 2.1: buffer offsets instead of pointers when a VBO/IBO/PBO is bound, mapped buffers through a guest copy, ARB vertex programs cleaned of `vertex.attrib[N].secondary`; `SCIMITAR_TRACE_GL` counts draws and checks programs, `SCIMITAR_TRACE_GL_FRAME=N` (or `A-B`, 0 for from startup) logs every call of those frames; with `SCIMITAR_RENDERER=metal` each GL shim forwards to the Metal backend. Brotherhood also uses `APPLE_vertex_array_object` (the tracked element buffer follows the bound VAO) and `glDrawBuffers` |
| `src/opengl_metal.cpp` | the GL, CGL and GLU imports on iOS, where there is no desktop GL: each goes to the Metal backend; CGL's pixel format and second context are handles for nothing |
| `src/arb_msl.cpp` | not an import: the Metal backend's ARB vertex/fragment program → Metal Shading Language translator (`scimitar/arb_msl.h`, [docs/METAL.md](../docs/METAL.md)); its own library, `scimitar_arb_msl`, tested by `tests/metal/arb_msl_test.mm` |
| `src/d3d9_arb.cpp` | not an import: Direct3D 9 shader bytecode → ARB programs, for AC2 on the Windows release's data ([docs/PC_DATA.md](../docs/PC_DATA.md)) and the Windows ports' renderer |
| `src/metal/` | the Metal rendering backend, `SCIMITAR_RENDERER=metal` ([docs/METAL.md](../docs/METAL.md)): each GL shim in `opengl.cpp` forwards to its `metal::m_*` twin (`metal/shims.h` lists them); `shims.mm` decodes guest arguments, `backend.mm` holds the state, guest-memory buffers, textures, render passes, pipelines, queries and presentation; the shell drives it through `scimitar/metal_gl.h`. Vertex array objects swap the arrays in and out of the state; `glDrawBuffers` adds colour attachments 1-3 to the pass (`PassTargets::more`), and every pipeline names all attachments of its pass. `shader_cache.mm`: the shader cache and precompile (`scimitar_shader_precompile`); `dump.mm`: frame dumps |
| `src/audio.cpp` | AUGraph, AudioUnit and AudioObject: Wwise's output graph as a host AUGraph whose render callback calls the guest's on a per-thread guest context; on iOS (no CoreAudio HAL) the device it asks about is a fixed 48 kHz one and its output unit is RemoteIO |
| `src/iokit.cpp` | IOKit HID: one virtual keyboard fed by the shell (`hid_keyboard_event`, HID usages) and a silent virtual mouse; the game needs both to read the keyboard. One virtual game pad (`scimitar/gamepad.h`): the joystick service the game looks for once at startup, present when the shell's probe reports a controller or `SCIMITAR_PAD` is set, with the Xbox 360 controller's HID layout (buttons 1-15, X/Y/Rx/Ry sticks, Z/Rz triggers); each drain of its queue hands out every element's current value, because the game's Xbox 360 pad type rebuilds its state from the queue every frame. `SCIMITAR_TRACE_INPUT` logs the lookups and every change in what the game reads. It also holds which button art the UI should show (`set_gamepad_kind`) and whether the pad is the selected input (`set_gamepad_selected`), for the AC2 overrides |
| `src/coregraphics.cpp` | the pre-10.6 display mode dictionaries (with the safe-for-hardware flag the engine filters on); mode switches are reported by the shell (`set_display_mode`) instead of changing the display |
| `src/carbon_fork.cpp` | Carbon `FSOpenFork`, `PBReadForkSync/Async`, `FSCloseFork`: async reads on a worker thread that calls the guest completion routine |
| `src/bink.cpp` | the 14 Bink functions on FFmpeg (bink demuxer, binkvideo/binkaudio decoders, swscale to the 32-bit BGRA the game copies into its texture, swresample to a host output unit per video); the BINK structure the game reads lives in guest memory; frames are paced by the wall clock. Without FFmpeg (found by pkg-config) `BinkOpen` fails and videos are skipped |
| `src/orbit.cpp` | the 22 `MgOrbitdll*` functions of `ubiorbitapi_r2.dylib` (`scimitar/orbit.h`), an offline session and saves as local files, installed by AC2's override of `LibraryLoader::PrivLoadLibrary` in `games/common/overrides.cpp`; and, as imports, the 21 methods of the C++ client Brotherhood links as a dylib (`libubiorbitapi_r2_loader.dylib`: `mg::orbitclient::OrbitClient`, `SavegameReader`/`Writer`/`Info`), a thin layer over the same functions |

Native functions the game calls through pointers (vtables, callbacks, the Orbit
functions) are registered with `register_host_function` (`scimitar/runtime.h`),
which gives each a guest address in `kHostFnBase`. Host code calls back into guest
code with `call_guest` and `call_virtual`.

`games/common/main.cpp` is the Mac and iOS ports' entry point (`ac2`, `acb`, built
with `-DSCIMITAR_FULL_IMAGE=ON`): it loads the image, binds the imports, runs the
static initializers (3,652 in AC2) and calls the game's `main`. The Windows ports
have their own (`games/<id>/main.cpp`).

The shells are game code in `games/common`: `shell.mm` (Cocoa: application, window,
GL or Metal view, event pump, display modes), `shell_ios.mm` (UIKit, Metal) and
`shell_common.mm` (shared: the GameController pad feeding the virtual HID pad,
scripted input). Each replaces the game's `Mac*` functions.

Other folders: `win32/`, the Windows ports' platform layer (KERNEL32, USER32,
Direct3D 9 on Metal, DirectInput and XInput, DirectSound, Bink, Uplay); `vita/`,
PS Vita experiments ([vita/README.md](vita/README.md)).
