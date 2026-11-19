// legacy-webos addition (not part of webOS OSE's luna-service2).
//
// In OSE's call.hpp, LS::Call's constructor, destructor and move operations
// are inline. LG's webOS 26 embedder imports them from libluna-service2++
// (_ZN2LS4CallC1Ev, _ZN2LS4CallD1Ev, _ZN2LS4CallaSEOS0_), so this file,
// compiled with -O0 (so they are neither inlined nor turned into local
// clones), makes the library emit and export them. The
// embedder's inlined code uses the same LS::Call layout as this header
// (checked in its machine code: _sh at 4, _single at 8, _context at 20,
// sizeof 184).
#include "call.hpp"

#include <utility>

namespace {

__attribute__((used)) void webos3_emit_call_members()
{
    LS::Call a;
    LS::Call b{std::move(a)};
    a = std::move(b);
}

} // namespace
