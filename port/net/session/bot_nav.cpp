// port/net: A* on the bot's map (bot.hpp), the map's parts, and walking a route. Four ways per cell, each step
// costs 1, Manhattan distance as the estimate. No game code: tested by port/net/tests/test_bot.cpp.
#include "bot.hpp"
#include <math.h>
#include <queue>
#include <stdlib.h>
#include <string.h>

namespace
{
const int N = net::engine::NavMap::kSize;
const int kDx[4] = {0, 0, -1, 1}, kDy[4] = {-1, 1, 0, 0};
const uint8_t kWay[4] = {net::engine::NavMap::kNorth, net::engine::NavMap::kSouth, net::engine::NavMap::kWest, net::engine::NavMap::kEast};
const float kPi = 3.14159265f;

int TurnTo(int rot, float dx, float dy)
{
    int err = (int)(atan2f(dx, dy) * 720.0f / kPi) - rot;
    while (err > 720)
    {
        err -= 1440;
    }
    while (err < -720)
    {
        err += 1440;
    }
    return err;
}

// 20 degrees either way is straight on (bot.cpp's Steer)
uint32_t Steer(int err)
{
    return err > 80 ? net::kBtnLeft : err < -80 ? net::kBtnRight : 0;
}

// One step from cell a to b next to it (diagonally too: both ways round open, no squeezing past a corner)
bool Step(const net::engine::NavMap& m, int a, int b)
{
    const int dx = b % N - a % N, dy = b / N - a / N;
    if (dx && dy)
    {
        const int side1 = a + dx, side2 = a + dy * N;
        return Step(m, a, side1) && Step(m, side1, b) && Step(m, a, side2) && Step(m, side2, b);
    }
    const uint8_t way = dx > 0 ? kWay[3] : dx < 0 ? kWay[2] : dy > 0 ? kWay[1] : kWay[0];
    return (m.ways[a] & way) && !m.water[b];
}

bool Inside(float x, float y)
{
    return x >= 0 && y >= 0 && x < N && y < N;
}

// A car's box, widened by <margin>: its centre, the unit ways across and along it, and the half sizes
struct CarBox
{
    float cx, cy, ax, ay, fx, fy, hw, hl;
    CarBox(const net::BotView::Seen& s, float margin)
    {
        const float a = s.rot * kPi / 720.0f;
        cx = s.x, cy = s.y, fx = sinf(a), fy = cosf(a), ax = fy, ay = -fx;
        hw = s.width * 0.5f + margin, hl = s.length * 0.5f + margin;
    }
    // Where along (x0, y0) to (x1, y1) it enters the box (0..1; 0 when it starts in it); -1 if it doesn't
    float Enters(float x0, float y0, float x1, float y1) const
    {
        const float p[2] = {(x0 - cx) * ax + (y0 - cy) * ay, (x0 - cx) * fx + (y0 - cy) * fy};
        const float d[2] = {(x1 - x0) * ax + (y1 - y0) * ay, (x1 - x0) * fx + (y1 - y0) * fy};
        const float h[2] = {hw, hl};
        float t0 = 0, t1 = 1;
        for (int k = 0; k < 2; k++)
        {
            if (fabsf(d[k]) < 1e-6f)
            {
                if (fabsf(p[k]) > h[k])
                {
                    return -1;
                }
                continue;
            }
            float a = (-h[k] - p[k]) / d[k], b = (h[k] - p[k]) / d[k];
            if (a > b)
            {
                const float t = a;
                a = b, b = t;
            }
            t0 = a > t0 ? a : t0, t1 = b < t1 ? b : t1;
            if (t0 > t1)
            {
                return -1;
            }
        }
        return t0;
    }
    void Corner(int k, float out, float* pX, float* pY) const // k 0..3, <out> blocks beyond the box
    {
        const float i = k & 1 ? 1.0f : -1.0f, j = k & 2 ? 1.0f : -1.0f;
        *pX = cx + ax * i * (hw + out) + fx * j * (hl + out);
        *pY = cy + ay * i * (hw + out) + fy * j * (hl + out);
    }
};
} // namespace

bool net::FindPath(const engine::NavMap& map, int sx, int sy, int gx, int gy, std::vector<int>& path, int maxExpand, int* pExpanded,
                   const std::vector<BotRoute::Blocked>* blocked, uint32_t frame)
{
    path.clear();
    if (pExpanded)
    {
        *pExpanded = 0;
    }
    if (sx < 0 || sy < 0 || gx < 0 || gy < 0 || sx >= N || sy >= N || gx >= N || gy >= N)
    {
        return false;
    }
    const int start = sy * N + sx, goal = gy * N + gx;
    if (start == goal)
    {
        return true;
    }
    if (map.part[start] && map.part[goal] && map.part[start] != map.part[goal])
    {
        return false; // no way joins them
    }
    static int cameFrom[N * N];
    static int cost[N * N];
    static uint32_t stamp[N * N]; // which search wrote a cell: no clearing per search
    static uint32_t search = 0;
    search++;
    typedef std::pair<int, int> Open; // (estimate, cell), smallest first
    std::priority_queue<Open, std::vector<Open>, std::greater<Open> > open;
    stamp[start] = search;
    cost[start] = 0;
    cameFrom[start] = -1;
    open.push(Open(0, start));
    int expanded = 0;
    bool found = false;
    while (!open.empty() && expanded < maxExpand)
    {
        const int cell = open.top().second;
        open.pop();
        if (cell == goal)
        {
            for (int c = goal; c != start; c = cameFrom[c])
            {
                path.push_back(c);
            }
            std::vector<int>(path.rbegin(), path.rend()).swap(path);
            found = true;
            break;
        }
        expanded++;
        const int x = cell % N, y = cell / N;
        for (int d = 0; d < 4; d++)
        {
            if (!(map.ways[cell] & kWay[d]))
            {
                continue;
            }
            const int next = (y + kDy[d]) * N + (x + kDx[d]);
            bool closed = map.water[next] && next != goal; // drowning: only when it is where it wants to go
            for (size_t i = 0; blocked && !closed && i < blocked->size(); i++)
            {
                const BotRoute::Blocked& b = (*blocked)[i];
                closed = b.cell == cell && b.way == kWay[d] && frame < b.until;
            }
            const int c = cost[cell] + 1;
            if (!closed && (stamp[next] != search || c < cost[next]))
            {
                stamp[next] = search;
                cost[next] = c;
                cameFrom[next] = cell;
                const int nx = next % N, ny = next / N;
                const int h = (nx > gx ? nx - gx : gx - nx) + (ny > gy ? ny - gy : gy - ny);
                open.push(Open(c + h, next));
            }
        }
    }
    if (pExpanded)
    {
        *pExpanded = expanded;
    }
    return found;
}

void net::LabelNavParts(engine::NavMap& map)
{
    memset(map.part, 0, sizeof(map.part));
    std::vector<int> todo;
    uint16_t next = 0;
    for (int c = 0; c < N * N; c++)
    {
        if (map.part[c] || map.height[c] == 255 || map.water[c] || next == 0xFFFF)
        {
            continue;
        }
        map.part[c] = ++next;
        todo.assign(1, c);
        while (!todo.empty())
        {
            const int cell = todo.back();
            todo.pop_back();
            for (int d = 0; d < 4; d++)
            {
                const int n = cell + kDy[d] * N + kDx[d];
                if ((map.ways[cell] & kWay[d]) && !map.water[n] && !map.part[n])
                {
                    map.part[n] = next;
                    todo.push_back(n);
                }
            }
        }
    }
}

int net::NavCell(const engine::NavMap& map, float x, float y, float z)
{
    // Nearest first, in a fixed order
    static const int kAround[][2] = {{0, 0},   {0, -1}, {0, 1},  {-1, 0},  {1, 0},  {-1, -1}, {1, -1}, {-1, 1},  {1, 1},
                                     {0, -2},  {0, 2},  {-2, 0}, {2, 0},   {-1, -2}, {1, -2}, {-1, 2},  {1, 2},  {-2, -1},
                                     {2, -1},  {-2, 1}, {2, 1},  {-2, -2}, {2, -2}, {-2, 2},  {2, 2}};
    if (!Inside(x, y))
    {
        return -1;
    }
    for (const int* o : kAround)
    {
        const int cx = (int)x + o[0], cy = (int)y + o[1];
        if (cx < 0 || cy < 0 || cx >= N || cy >= N)
        {
            continue;
        }
        const int h = map.height[cy * N + cx];
        if (h != 255 && fabsf(h + 1 - z) <= 1.0f) // a body stands on top of its block
        {
            return cy * N + cx;
        }
    }
    return -1;
}

bool net::NavClear(const engine::NavMap& map, float x0, float y0, float x1, float y1)
{
    const float dx = x1 - x0, dy = y1 - y0, length = sqrtf(dx * dx + dy * dy);
    if (!Inside(x0, y0) || !Inside(x1, y1))
    {
        return false;
    }
    const int steps = (int)(length / 0.2f) + 1;
    const float px = length > 0 ? -dy / length * 0.3f : 0, py = length > 0 ? dx / length * 0.3f : 0;
    const int from = (int)y0 * N + (int)x0;
    for (int side = -1; side <= 1; side++)
    {
        int cell = from;
        for (int i = 0; i <= steps; i++)
        {
            const float t = (float)i / steps, x = x0 + dx * t + px * side, y = y0 + dy * t + py * side;
            if (!Inside(x, y))
            {
                return false;
            }
            const int next = (int)y * N + (int)x;
            if (next != cell)
            {
                const int ox = next % N - cell % N, oy = next / N - cell / N;
                if (ox < -1 || ox > 1 || oy < -1 || oy > 1 || !Step(map, cell, next))
                {
                    return false;
                }
                cell = next;
            }
        }
    }
    return true;
}

const net::BotView::Seen* net::ShotBlocker(const BotView& v, float x, float y, const BotView::Seen& target)
{
    const BotView::Seen* first = NULL;
    float firstT = 1;
    const float reach = hypotf(target.x - x, target.y - y);
    for (const BotView::Seen& s : v.seen)
    {
        if (s.kind != BotView::Seen::kCar || fabsf(s.z - v.z) > 1.0f || hypotf(s.x - x, s.y - y) > reach + s.length)
        {
            continue;
        }
        const CarBox box(s, 0.1f);
        const float t = box.Enters(x, y, target.x, target.y);
        if (t >= 0 && t < firstT && box.Enters(target.x, target.y, target.x, target.y) < 0)
        {
            firstT = t, first = &s;
        }
    }
    return first;
}

void net::BotRoute::Next(const BotView& v, float goalX, float goalY, float* pX, float* pY, float goalZ)
{
    *pX = goalX, *pY = goalY;
    if (!v.map || !Inside(v.x, v.y) || !Inside(goalX, goalY))
    {
        return;
    }
    const engine::NavMap& map = *v.map;
    const int raw = (int)v.y * N + (int)v.x;
    const int start = NavCell(map, v.x, v.y, v.z), at = start >= 0 ? start : raw;
    const int found = goalZ >= 0 ? NavCell(map, goalX, goalY, goalZ) : -1;
    const int target = found >= 0 ? found : (int)goalY * N + (int)goalX;
    if ((path.empty() && !none) || v.frame >= replanAt || target != goal)
    {
        goal = target;
        replanAt = v.frame + 30;
        size_t kept = 0;
        for (size_t i = 0; i < blocked.size(); i++)
        {
            if (v.frame < blocked[i].until)
            {
                blocked[kept++] = blocked[i];
            }
        }
        blocked.resize(kept);
        int expanded = 0;
        none = !FindPath(map, at % N, at / N, target % N, target / N, path, 40000, &expanded, &blocked, v.frame);
        if (v.stats)
        {
            v.stats->searches++;
            v.stats->failed += none;
            v.stats->expanded += (uint64_t)expanded;
        }
    }
    // Drop the waypoints up to the one we are on, if it is among the next few (cutting a corner skips some)
    for (size_t i = 0; i < path.size() && i < 6; i++)
    {
        if (path[i] == raw || path[i] == at)
        {
            path.erase(path.begin(), path.begin() + i + 1);
            break;
        }
    }
    if (!path.empty())
    {
        // The farthest of the next ten it walks to in a straight line; the next one otherwise
        size_t ahead = 0;
        for (size_t i = path.size() < 10 ? path.size() - 1 : 9; i > 0; i--)
        {
            if (NavClear(map, v.x, v.y, path[i] % N + 0.5f, path[i] / N + 0.5f))
            {
                ahead = i;
                break;
            }
        }
        *pX = path[ahead] % N + 0.5f;
        *pY = path[ahead] / N + 0.5f;
    }
}

uint32_t net::BotWalker::Walk(const BotView& v, uint32_t id, float x, float y, float z)
{
    if (v.frame > mLastFrame + kBotDecisionTicks || v.frame < mLastFrame)
    {
        mCheckAt = v.frame + 45, mCheckX = v.x, mCheckY = v.y;
    }
    mLastFrame = v.frame;
    if (id != mGoal)
    {
        mGoal = id, mGoalSince = v.frame, mBest = (size_t)-1;
    }
    if (v.frame < mRecoverUntil)
    {
        return mRecoverButtons;
    }
    float wx = x, wy = y;
    route.Next(v, x, y, &wx, &wy, z);
    bool jump = false;
    AvoidCars(v, id, x, y, &wx, &wy, &jump);
    // Progress: the route two cells shorter than its shortest yet (no route at all is none). A player with a route
    // to it moves as it is chased: kept, else the bot left the one it fought for someone far.
    bool player = false;
    for (const BotView::Seen& s : v.seen)
    {
        player |= s.id == id && s.kind == BotView::Seen::kPlayer;
    }
    const size_t left = route.none ? (size_t)-2 : route.path.size();
    if (mBest == (size_t)-1 || left + 2 <= mBest || (player && !route.none))
    {
        mBest = left, mGoalSince = v.frame;
    }
    if (id && v.frame - mGoalSince > (route.none ? 90u : 300u))
    {
        GiveUp(v, id);
        return 0;
    }
    if (v.frame >= mCheckAt)
    {
        const bool stuck = hypotf(v.x - mCheckX, v.y - mCheckY) < 0.3f;
        mCheckAt = v.frame + 45, mCheckX = v.x, mCheckY = v.y;
        if (stuck)
        {
            Recover(v);
            return mRecoverButtons;
        }
    }
    const int err = TurnTo(v.rot, wx - v.x, wy - v.y);
    return Steer(err) | (abs(err) < 300 ? kBtnForward : 0) | (jump && abs(err) < 100 ? kBtnJump : 0); // walk while roughly facing the way
}

// A car in the way (parked, or traffic stopped) within the next 3 blocks: round it by the corner of its box that makes
// the way shortest and is clear (the one it went by last preferred); none clear and up against it: over it, jumping
void net::BotWalker::AvoidCars(const BotView& v, uint32_t id, float goalX, float goalY, float* pX, float* pY, bool* pJump)
{
    const float dx = *pX - v.x, dy = *pY - v.y, dist = hypotf(dx, dy);
    if (dist < 0.05f || v.car)
    {
        return;
    }
    const float look = dist < 3 ? dist : 3, ex = v.x + dx / dist * look, ey = v.y + dy / dist * look;
    const BotView::Seen* block = NULL;
    float first = 2;
    for (const BotView::Seen& s : v.seen)
    {
        if (s.kind != BotView::Seen::kCar || s.id == id || hypotf(s.x - goalX, s.y - goalY) < 1.0f || fabsf(s.z - v.z) > 1.0f ||
            hypotf(s.x - v.x, s.y - v.y) > look + s.length)
        {
            continue;
        }
        const float t = CarBox(s, 0.3f).Enters(v.x, v.y, ex, ey);
        if (t >= 0 && t < first)
        {
            first = t, block = &s;
        }
    }
    if (!block)
    {
        return;
    }
    const CarBox around(*block, 0.3f), body(*block, 0.15f);
    int best = -1;
    float bestCost = 1e9f;
    for (int k = 0; k < 4; k++)
    {
        float cx = 0, cy = 0;
        around.Corner(k, 0.35f, &cx, &cy);
        const float cost = hypotf(cx - v.x, cy - v.y) + hypotf(*pX - cx, *pY - cy) - (k == mAvoidCorner && block->id == mAvoidCar ? 1.0f : 0);
        // A corner it stands at is passed: heading for it, it only turned on the spot
        if (cost < bestCost && hypotf(cx - v.x, cy - v.y) > 0.3f && body.Enters(v.x, v.y, cx, cy) < 0 &&
            (!v.map || NavClear(*v.map, v.x, v.y, cx, cy)))
        {
            best = k, bestCost = cost;
        }
    }
    if (best >= 0)
    {
        mAvoidCar = block->id, mAvoidCorner = best;
        around.Corner(best, 0.35f, pX, pY);
        if (v.stats)
        {
            v.stats->avoided++;
        }
        return;
    }
    *pJump = first * look < 0.6f; // nothing clear round it: over it
}

bool net::BotWalker::Skipped(uint32_t id, uint32_t frame) const
{
    for (const Skip& s : mSkips)
    {
        if (s.id == id && frame < s.until)
        {
            return true;
        }
    }
    return false;
}

bool net::BotWalker::Reachable(const BotView& v, float x, float y, float z) const
{
    if (!v.map)
    {
        return true;
    }
    const int from = NavCell(*v.map, v.x, v.y, v.z);
    if (from < 0 || !v.map->part[from])
    {
        return true;
    }
    const int to = NavCell(*v.map, x, y, z);
    return to >= 0 && v.map->part[to] == v.map->part[from];
}

void net::BotWalker::GiveUp(const BotView& v, uint32_t id)
{
    size_t kept = 0;
    for (size_t i = 0; i < mSkips.size(); i++)
    {
        if (v.frame < mSkips[i].until)
        {
            mSkips[kept++] = mSkips[i];
        }
    }
    mSkips.resize(kept);
    mSkips.push_back({id, v.frame + 900});
    mGoal = 0;
    route.path.clear();
    if (v.stats)
    {
        v.stats->giveUps++;
    }
}

uint32_t net::BotWalker::Enter(const BotView& v, uint32_t id, uint32_t press)
{
    if (id != mEnterId || v.frame - mEnterLast > 30 || v.frame < mEnterLast)
    {
        mEnterId = id, mEnterSince = v.frame;
    }
    mEnterLast = v.frame;
    if (v.frame - mEnterSince > 120)
    {
        GiveUp(v, id);
        mEnterId = 0;
        return 0;
    }
    return press;
}

// Stuck: the next way out of the ones it tried here in the last 10 s
void net::BotWalker::Recover(const BotView& v)
{
    const int cell = (int)v.y * N + (int)v.x;
    size_t kept = 0;
    for (size_t i = 0; i < mSpots.size(); i++)
    {
        if (v.frame - mSpots[i].at < 300)
        {
            mSpots[kept++] = mSpots[i];
        }
    }
    mSpots.resize(kept);
    Spot* spot = NULL;
    for (Spot& s : mSpots)
    {
        spot = s.cell == cell ? &s : spot;
    }
    if (!spot)
    {
        mSpots.push_back({cell, -1, v.frame});
        spot = &mSpots.back();
    }
    spot->level++;
    spot->at = v.frame;
    if (v.stats)
    {
        v.stats->recoveries++;
    }
    mRecoverUntil = v.frame + 15;
    bool carNear = false;
    for (const BotView::Seen& s : v.seen)
    {
        carNear |= s.kind == BotView::Seen::kCar && hypotf(s.x - v.x, s.y - v.y) < 0.5f + s.length * 0.5f && fabsf(s.z - v.z) < 1.0f;
    }
    switch (spot->level)
    {
    case 0: // back off and turn; against a car, over it
        mRecoverButtons = carNear ? kBtnForward | kBtnJump : kBtnBack | kBtnLeft;
        mRecoverUntil = carNear ? v.frame + 10 : mRecoverUntil;
        break;
    case 1: // the other way, with the way it was crossing left out of its route
        if (!route.path.empty())
        {
            const int next = route.path[0], dx = next % N - cell % N, dy = next / N - cell / N;
            for (int d = 0; d < 4; d++)
            {
                if (dx == kDx[d] && dy == kDy[d])
                {
                    route.blocked.push_back({cell, kWay[d], v.frame + 600});
                }
            }
        }
        mRecoverButtons = kBtnBack | kBtnRight;
        break;
    case 2: // over it (a kerb, a body)
        mRecoverButtons = kBtnForward | kBtnJump;
        mRecoverUntil = v.frame + 10;
        break;
    default:
        mRecoverButtons = kBtnBack;
        if (mGoal)
        {
            GiveUp(v, mGoal);
        }
        spot->level = -1;
        break;
    }
    route.path.clear(); // planned again
}
