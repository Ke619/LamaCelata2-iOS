#pragma once
// How the guest's 32-bit address space sits in the host's (GuestMemory):
// - Full: the whole 4 GB is reserved (64-bit hosts; the default on the Mac).
// - Compact: its low 2 GB, holding the image, the imported data and the heap
//   moved down (guest_heap.h), so an iOS app may reserve it without extended
//   virtual addressing (the default on iOS; SCIMITAR_COMPACT_GUEST=1).
// - Window: a small window from guest address 0, for 32-bit hosts (the PS
//   Vita, where it is the default; SCIMITAR_GUEST_WINDOW_MB=N elsewhere, to
//   try it). The image and the imported data sit below 0x08000000 and the heap
//   fills the rest. Guest addresses beyond the window are not mapped.
// GuestMemory::size() is the reservation's size in each.
#include <cstdint>

namespace scimitar {

enum class GuestLayout { Full, Compact, Window };
GuestLayout guest_layout();

// The window's size (512 MB unless SCIMITAR_GUEST_WINDOW_MB says otherwise),
// rounded up to whole 4 MB; set it before the first GuestMemory is made.
void set_guest_window_size(uint64_t bytes);

}  // namespace scimitar
