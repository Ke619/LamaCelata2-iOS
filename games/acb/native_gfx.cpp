// Native replacements for hot Brotherhood render-thread code that calls into
// the GL layer (so not in native_math.cpp, which native_test_acb links
// without it). Found by profiling Rome at the highest settings.
#include "../common/native_constants.h"

namespace scimitar::overrides {

// update_shader_constants<Vector4, &SetVSCEnv> at 0x00b6fef0: runs to
// glProgramEnvParameters4fvEXT(GL_VERTEX_PROGRAM_ARB, ...).
void ov__ZN8scimitar23update_shader_constantsIN4Gear7Vector4IfEEXadL_ZNS_9SetVSCEnvEjPKS3_jEEEEvjPT_PKS6_j(
    scimitar::Context& c) {
    native::update_shader_constants(c, [&](uint32_t index, uint32_t run, uint32_t n) {
        native::imp_glProgramEnvParameters4fvEXT_call(c, 0x8620, index, n, run);
    });
}

// update_shader_constants<Vector4, &SetPSCEnv> at 0x00b6fc90: runs to
// scimitar::SetPSCEnv(index, run, n) (0x00b2d250).
void ov__ZN8scimitar23update_shader_constantsIN4Gear7Vector4IfEEXadL_ZNS_9SetPSCEnvEjPKS3_jEEEEvjPT_PKS6_j(
    scimitar::Context& c) {
    native::update_shader_constants(c, [&](uint32_t index, uint32_t run, uint32_t n) {
        call_guest(c, 0x00b2d250, {index, run, n});
    });
}

}  // namespace scimitar::overrides
