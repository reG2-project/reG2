#include "guard.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
PortPool::Info* gLastRefused;

double Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int LimitFor(const char* name)
{
    const char* env = getenv("GTA2_POOL_LIMIT");
    const size_t len = strlen(name);
    for (const char* p = env; p && *p;)
    {
        if (!strncmp(p, name, len) && p[len] == '=')
        {
            return atoi(p + len + 1);
        }
        p = strchr(p, ',');
        p = p ? p + 1 : NULL;
    }
    return 0;
}
} // namespace

int PortPool::gEssential;

PortPool::Info* PortPool::Register(const char* prettyFunction, int cap)
{
    Info* info = new Info();
    const char* type = strstr(prettyFunction, "PoolType = ");
    type = type ? type + strlen("PoolType = ") : "?";
    size_t len = strcspn(type, ",;]");
    len = len < sizeof(info->name) - 1 ? len : sizeof(info->name) - 1;
    memcpy(info->name, type, len);
    info->cap = cap;
    info->limit = LimitFor(info->name);
    info->lastLine = -1e9;
    if (info->limit)
    {
        fprintf(stderr, "reg2: pool %s capped at %d of %d (GTA2_POOL_LIMIT)\n", info->name, info->limit, cap);
    }
    return info;
}

void PortPool::Refused(Info* info, int used)
{
    info->used = used;
    gLastRefused = info;
}

void PortPool::Skipped(const char* what)
{
    Info* info = gLastRefused;
    if (!info)
    {
        return;
    }
    info->skips++;
    const double now = Now();
    if (now - info->lastLine >= 60.0)
    {
        fprintf(stderr, "reg2: WARNING pool %s full (%d of %d used), skipped %s (%u skips since last line)\n", info->name,
                info->used, info->limit ? info->limit : info->cap, what, info->skips);
        fflush(stderr);
        info->lastLine = now;
        info->skips = 0;
    }
    gLastRefused = NULL;
}
