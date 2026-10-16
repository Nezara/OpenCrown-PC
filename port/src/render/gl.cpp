#include "gl.h"

namespace dcp::gl {
#define DCP_GL_DEF(ret, name, args) ret(DCP_GLAPI* name) args = nullptr;
DCP_GL_FUNCS(DCP_GL_DEF)
#undef DCP_GL_DEF

const char* load(void* (*get_proc)(const char*)) {
#define DCP_GL_LOAD(ret, name, args)                                   \
    name = reinterpret_cast<ret(DCP_GLAPI*) args>(get_proc(#name));    \
    if (!name) return #name;
    DCP_GL_FUNCS(DCP_GL_LOAD)
#undef DCP_GL_LOAD
    return nullptr;
}
}  // namespace dcp::gl
