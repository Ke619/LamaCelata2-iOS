// Native replacements for hot AC2 render-thread code that calls into the GL
// layer (so not in native_math.cpp, which native_test links without it).
// Brotherhood's counterparts were found by profiling Rome; AC2's are the same
// functions.
#include "../common/native_constants.h"

namespace scimitar::overrides {

// update_shader_constants<Vector4, &SetVSCEnv> at 0x00c6ed30 and <&SetPSCEnv>
// at 0x00c6ee10: runs to glProgramEnvParameters4fvEXT with
// GL_VERTEX_PROGRAM_ARB and GL_FRAGMENT_PROGRAM_ARB (AC2 inlines SetPSCEnv).
void ov__ZN8scimitar23update_shader_constantsIN4Gear7Vector4IfEEXadL_ZNS_9SetVSCEnvEjPKS3_jEEEEvjPT_PKS6_j(
    scimitar::Context& c) {
    native::update_shader_constants(c, [&](uint32_t index, uint32_t run, uint32_t n) {
        native::imp_glProgramEnvParameters4fvEXT_call(c, 0x8620, index, n, run);
    });
}
void ov__ZN8scimitar23update_shader_constantsIN4Gear7Vector4IfEEXadL_ZNS_9SetPSCEnvEjPKS3_jEEEEvjPT_PKS6_j(
    scimitar::Context& c) {
    native::update_shader_constants(c, [&](uint32_t index, uint32_t run, uint32_t n) {
        native::imp_glProgramEnvParameters4fvEXT_call(c, 0x8804, index, n, run);
    });
}

}  // namespace scimitar::overrides
