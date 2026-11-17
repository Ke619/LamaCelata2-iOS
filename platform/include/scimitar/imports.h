#pragma once
// Native implementations of functions the game imports from macOS. Each shim
// follows i386 cdecl on the guest stack (see scimitar/runtime.h) and is named
// imp_<dyld name with non-identifier characters folded to '_'>.
#include <string>
#include <vector>

#include "scimitar/runtime.h"

namespace scimitar::imports {

void imp_read_UNIX2003(Context& c);  // _read$UNIX2003
void imp_lseek(Context& c);          // _lseek
void imp_aio_read(Context& c);       // _aio_read (asynchronous reads, done at once)
void imp_aio_error(Context& c);
void imp_aio_return(Context& c);
void imp_memcpy(Context& c);         // _memcpy
void imp_memmove(Context& c);        // _memmove
void imp_free(Context& c);           // _free

}  // namespace scimitar::imports

namespace scimitar {

struct ImportShim {
    const char* dyld_name;
    GuestFn fn;
};

// Looks up a shim by its dyld name (e.g. "_lseek"); nullptr if none exists.
const ImportShim* find_import_shim(const std::string& dyld_name);

// Each shim file registers its table at startup.
struct ShimRegistrar {
    ShimRegistrar(const ImportShim* shims, size_t count);
};
#define SCIMITAR_REGISTER_SHIMS(table) \
    static const ::scimitar::ShimRegistrar kShimRegistrar_##table(table, sizeof table / sizeof *table)

// The process arguments the game sees (NSProcessInfo arguments): argv[0] and
// the game's options. Default: just "scimitar_sf".
// A guest path as the host should open it. The game names files as on a Mac's
// case-insensitive disk ("sounddata/mac" for "sounddata/Mac"); on iOS, whose
// file systems are case-sensitive, a path that does not exist as written is
// resolved one component at a time, ignoring case. Elsewhere: the path as is.
std::string host_path(const char* path);

void set_process_arguments(const std::vector<std::string>& args);
const std::vector<std::string>& process_arguments();

// Fills the executable's pointer slots for imported symbols (data_imports.cpp).
struct LoadedImage;
void bind_imports(GuestMemory& m, LoadedImage& image);

// Sets the calling guest thread's errno (libc_process.cpp).
void set_guest_errno(Context& c, int e);

// printf-style formatting of a guest format string with arguments from guest
// va_list `ap` (libc_printf.cpp); the result is UTF-8.
std::string guest_format(GuestMemory& m, uint32_t fmt, uint32_t ap, bool wide);
// How %@ (CoreFoundation/Foundation formats) prints an object.
using ObjectText = std::string (*)(GuestMemory& m, uint32_t object);
void set_object_formatter(ObjectText f);

// When enabled, every shim call appends a line describing its arguments and
// result. Tests compare these traces between execution engines.
void set_import_trace(bool on);
bool import_trace_enabled();
std::vector<std::string>& import_trace();

}  // namespace scimitar
