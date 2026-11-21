// See build_id.hpp. reg2_commit.h is made at every build (port/git_commit.cmake): only this file depends on it.
#include "build_id.hpp"
#include "reg2_commit.h"
#include "reg2_version.h"

const char* port_BuildId()
{
    return GTA2_COMMIT[0] ? GTA2_VERSION " " GTA2_COMMIT : GTA2_VERSION;
}
