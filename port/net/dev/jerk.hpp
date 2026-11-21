#pragma once
// port/net (GTA2_NET_JERKLOG): how jerky what is shown moves. Per entity, frame to frame, the change of its step
// (|v - v_prev|, in blocks per tick per tick), the way it moves against the way it faces and how fast it turns; counted
// per kind (players, cars, peds) over a size. The server measures the same on its own state: what it has is the game's
// own (collisions, turns), the client's excess is the netcode's.
#include "../core/snapshot.hpp"
#include "../engine/engine.hpp"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <unordered_map>
#include <vector>

namespace net
{
class JerkMeter
{
  public:
    void PrintJumps(bool on) // each change over 0.4 blocks, as it happens
    {
        mPrintJumps = on;
    }
    void Feed(uint32_t frame, const std::vector<engine::Pose>& poses, uint32_t localId = 0) // localId: the client's own player
    {
        std::unordered_map<uint32_t, Track> next;
        for (const engine::Pose& p : poses)
        {
            Track t = {engine::Blocks(p.x), engine::Blocks(p.y), 0, 0, p.rot, frame, false};
            auto it = mTracks.find(p.id);
            if (it != mTracks.end() && it->second.frame + 1 == frame)
            {
                const Track& o = it->second;
                t.vx = t.x - o.x;
                t.vy = t.y - o.y;
                t.moving = true;
                const float step = sqrtf(t.vx * t.vx + t.vy * t.vy);
                Kind& k = mKinds[p.id == localId ? 3 : engine::IsPlayerId(p.id) ? 0 : engine::IsCarId(p.id) ? 1 : 2];
                if (step > 2)
                {
                    k.jumps++; // a respawn, a car entered: not motion
                    t.moving = false;
                }
                else
                {
                    Facing(k, p.id, o, t, step);
                }
                if (t.moving && o.moving)
                {
                    const float ax = t.vx - o.vx, ay = t.vy - o.vy, a = sqrtf(ax * ax + ay * ay);
                    k.frames++;
                    k.over[0] += a > 0.05f;
                    k.over[1] += a > 0.15f;
                    k.over[2] += a > 0.4f;
                    if (mPrintJumps && a > 0.4f)
                    {
                        printf("net: frame %u, jerk of %u: %.2f, step (%.2f, %.2f) after (%.2f, %.2f) at (%.2f, %.2f)\n", frame, p.id, a, t.vx, t.vy,
                               o.vx, o.vy, t.x, t.y);
                    }
                    k.sum += a;
                }
            }
            next[p.id] = t;
        }
        mTracks.swap(next);
    }
    // The server's newest two snapshots: whether each entity moved there within 45 degrees of its facing (the facing
    // counts then compare what is shown with that)
    void Reference(const Snapshot& newest, const Snapshot& before)
    {
        mReference.clear();
        if (before.tick + 1 != newest.tick)
        {
            return;
        }
        size_t j = 0;
        for (const EntityState& e : newest.entities)
        {
            while (j < before.entities.size() && before.entities[j].id < e.id)
            {
                j++;
            }
            engine::Pose n, b;
            if (j < before.entities.size() && before.entities[j].id == e.id && engine::PoseOf(e, &n) && engine::PoseOf(before.entities[j], &b))
            {
                const float vx = engine::Blocks(n.x) - engine::Blocks(b.x), vy = engine::Blocks(n.y) - engine::Blocks(b.y);
                const float step = sqrtf(vx * vx + vy * vy);
                if (step >= 0.02f && step <= 2)
                {
                    mReference[e.id] = OffFacing(n.rot, vx, vy, step) <= 45;
                }
            }
        }
    }
    void Report(const char* who, uint32_t frame)
    {
        static const char* const kName[4] = {"players", "cars", "peds", "own player"};
        for (int i = 0; i < 4; i++)
        {
            const Kind& k = mKinds[i];
            printf("net: %s frame %u, jerk of %s: %u entity frames, %.4f blocks/tick^2 on average, over 0.05: %.2f%%, over 0.15: %.2f%%, "
                   "over 0.4: %.2f%%, %u jumps\n",
                   who, frame, kName[i], k.frames, k.frames ? k.sum / k.frames : 0.0f, k.frames ? 100.0f * k.over[0] / k.frames : 0.0f,
                   k.frames ? 100.0f * k.over[1] / k.frames : 0.0f, k.frames ? 100.0f * k.over[2] / k.frames : 0.0f, k.jumps);
            if (k.facingFrames)
            {
                printf("net: %s frame %u, facing of %s: %u moving frames, way off the facing by over 45 degrees: %.2f%%, over 135: %.2f%%; "
                       "turns over 30 degrees a frame: %.2f%%, over 60: %.2f%%\n",
                       who, frame, kName[i], k.facingFrames, 100.0f * k.offFacing[0] / k.facingFrames, 100.0f * k.offFacing[1] / k.facingFrames,
                       100.0f * k.fastTurns[0] / k.facingFrames, 100.0f * k.fastTurns[1] / k.facingFrames);
            }
            if (k.agreedFrames)
            {
                printf("net: %s frame %u, facing of %s where the newest snapshots have it moving the way it faces: %u frames, "
                       "shown over 45 degrees off: %.2f%%\n",
                       who, frame, kName[i], k.agreedFrames, 100.0f * k.offAgreed / k.agreedFrames);
            }
            mKinds[i] = Kind();
        }
    }

  private:
    struct Track
    {
        float x, y, vx, vy;
        int32_t rot; // Ang16, 0..1439
        uint32_t frame;
        bool moving;
    };
    struct Kind
    {
        uint32_t frames = 0, jumps = 0, over[3] = {0, 0, 0};
        float sum = 0;
        uint32_t facingFrames = 0, offFacing[2] = {0, 0}, fastTurns[2] = {0, 0}, agreedFrames = 0, offAgreed = 0;
    };
    // How far the way it moved is from the way it faces, in degrees (the game moves a char along sine, cosine of its
    // rotation)
    static float OffFacing(int32_t rot, float vx, float vy, float step)
    {
        const float a = (float)rot * 3.14159265f / 720.0f;
        const float c = (sinf(a) * vx + cosf(a) * vy) / step;
        return acosf(c < -1 ? -1 : c > 1 ? 1 : c) * 180.0f / 3.14159265f;
    }
    // Over the frames it moved at least a fiftieth of a block: the way it moved against its facing (also where the
    // reference had them agree), and how fast it turned
    void Facing(Kind& k, uint32_t id, const Track& o, const Track& t, float step)
    {
        if (step < 0.02f)
        {
            return;
        }
        const float off = OffFacing(t.rot, t.vx, t.vy, step);
        const int turn = abs(((t.rot - o.rot) % 1440 + 1440 + 720) % 1440 - 720) / 4;
        auto ref = mReference.find(id);
        k.facingFrames++;
        k.offFacing[0] += off > 45;
        k.offFacing[1] += off > 135;
        k.fastTurns[0] += turn > 30;
        k.fastTurns[1] += turn > 60;
        if (ref != mReference.end() && ref->second)
        {
            k.agreedFrames++;
            k.offAgreed += off > 45;
        }
    }
    std::unordered_map<uint32_t, Track> mTracks;
    std::unordered_map<uint32_t, bool> mReference;
    Kind mKinds[4];
    bool mPrintJumps = false;
};
} // namespace net
