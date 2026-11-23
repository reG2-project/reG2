// port/net (port/docs/netcode.md, "Entities"): a snapshot captured from and applied to the game; the world entity
// (world_entity.cpp), the players (players.cpp), cars (cars.cpp), a client's replicas (replica.cpp) and pickups
// (pickups.cpp) do the parts. Also the helpers the engine's files share, the game's controls the session uses
// (running, the player count, the inputs, the tick) and a client's view of what is shown.
#include "internal.hpp"
#include "port/src/rand/owner.hpp"
#include "port/src/menu.hpp"
#include "port/src/net_hooks.hpp"
#include "port/src/pause_menu.hpp"
#include "port/src/rand/state.hpp"
#include "Source/Car.hpp"
#include "Source/Char_Pool.hpp"
#include "Source/Game.hpp"
#include "Source/Ped.hpp"
#include "Source/Player.hpp"
#include "Source/SpriteGrid.hpp"
#include "Source/TrafficLights.hpp"
#include "Source/char.hpp"
#include "Source/network_data.hpp"
#include "Source/rng.hpp"
#include "Source/sprite.hpp"
#include "Source/GameSession.hpp"
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <unordered_map>


// --- Shared helpers ---

Player* net::engine::GetPlayer(int slot)
{
    Game* g = gGame_0x40;
    return g && slot >= 0 && slot < (int)g->mNumPlayers ? g->mPlayers[slot] : NULL;
}

Ped* net::engine::InGamePed(int slot)
{
    Player* p = GetPlayer(slot);
    return p && p->mInUse ? p->mPlayerPed : NULL;
}

int32_t net::engine::CarRef(const Car* car)
{
    return car ? car->mCarId + 1 : 0;
}

uint32_t net::engine::CarRefNetId(int32_t ref)
{
    return ref ? kCarIdBase + (uint32_t)(ref - 1) : 0;
}

Car* net::engine::RefCar(int32_t ref)
{
    return ref ? ReplicaCar(CarRefNetId(ref)) : NULL;
}

Sprite* net::engine::CharSprite(const Ped* ped)
{
    return ped->mGameObject ? ped->mGameObject->mSpritePtr : NULL;
}

Sprite* net::engine::PedSprite(Ped* ped)
{
    return ped->mCar ? ped->mCar->mCarSprite : CharSprite(ped);
}

net::EntityState net::engine::Entity(uint32_t id, uint8_t type)
{
    EntityState e;
    memset(&e, 0, sizeof(e));
    e.id = id;
    e.type = type;
    return e;
}

static_assert(net::engine::kPlayerX == 0 && net::engine::kCarX == 0 && net::engine::kPedX == 0 && net::engine::kObjX == 0 && net::engine::kPlayerRot == 3 &&
                  net::engine::kCarRot == 3 && net::engine::kPedRot == 3 && net::engine::kObjRot == 3 && net::engine::kTrainX == 0 &&
                  net::engine::kTrainRot == 3,
              "every entity with a place has x, y, z, rot first");

void net::engine::PutPosition(EntityState& e, const Sprite* s)
{
    e.f[0] = s->mXy.x.mValue;
    e.f[1] = s->mXy.y.mValue;
    e.f[2] = s->mZpos.mValue;
    e.f[3] = s->mRotation.rValue;
}

bool net::engine::Near(const Sprite* s, int32_t x, int32_t y, int32_t radius)
{
    const int32_t dx = s->mXy.x.mValue - x, dy = s->mXy.y.mValue - y;
    return dx < radius && dx > -radius && dy < radius && dy > -radius;
}

net::engine::ReachArea::ReachArea(const Snapshot& snap, float margin) : mReach(SnapshotReach(snap) - margin)
{
    mLocated = PlayerPosition(LocalPlayer(), &mX, &mY);
}

float net::engine::ReachArea::DistanceTo(const Sprite* s) const
{
    const float dx = s->mXy.x.ToFloat() - mX, dy = s->mXy.y.ToFloat() - mY;
    return sqrtf(dx * dx + dy * dy);
}

bool net::engine::ReachArea::Inside(const Sprite* s) const
{
    const float dx = s->mXy.x.ToFloat() - mX, dy = s->mXy.y.ToFloat() - mY;
    return mLocated && dx * dx + dy * dy < mReach * mReach;
}

bool net::engine::ReachArea::Beyond(const Sprite* s) const
{
    if (!s || !mLocated)
    {
        return false;
    }
    const float dx = s->mXy.x.ToFloat() - mX, dy = s->mXy.y.ToFloat() - mY;
    return dx * dx + dy * dy > mReach * mReach;
}

bool net::engine::IsSpareDriver(const Ped* ped)
{
    return ped && gCarManager_6C && ped == gCarManager_6C->GetSpareDriver();
}

// The collision grid's rows and cells are the map's 256 by 256 blocks, unchecked: a sprite put off the map is linked
// into memory past them (a cycle there hung the next removal)
bool net::engine::OnMap(const Fix16& x, const Fix16& y, uint32_t id)
{
    const int32_t kMax = 256 << 14;
    if (x.mValue >= 0 && x.mValue < kMax && y.mValue >= 0 && y.mValue < kMax)
    {
        return true;
    }
    printf("net: CHECK entity %u moved off the map, to (%.2f, %.2f): not moved\n", id, x.ToFloat(), y.ToFloat());
    return false;
}

void net::engine::MovePed(Ped* ped, const Fix16& x, const Fix16& y, const Fix16& z, const Ang16& rot)
{
    Sprite* s = CharSprite(ped);
    if (!s || AtCarDoor(ped) || !OnMap(x, y, kPedIdBase + ped->mId))
    {
        return; // getting in or out at the door, the game keeps its char's place in the collision grid itself: re-added, a link
                // stays behind (the walk to the door is plain walking: corrected, it no longer drifted from the server's)
    }
    gSpriteGrid_1->RemoveFromRegionBuckets(s);
    s->set_xyz_lazy(x, y, z);
    s->set_ang_lazy(rot); // what is drawn: the char's own rotation only reaches its sprite in its next update
    ped->SetRotation(rot);
    gSpriteGrid_1->AddToRegionBuckets(s);
    ped->mCam.x = x;
    ped->mCam.y = y;
    ped->mCam.z = z;
}

// --- The game ---

bool net::engine::GameRunning()
{
    return gGame_0x40 && gGame_0x40->mNumPlayers > 0;
}

int net::engine::PlayerCount()
{
    return gGame_0x40 ? gGame_0x40->mNumPlayers : 0;
}

int net::engine::LocalPlayer()
{
    return (int)NetworkPlayerIdx();
}

uint32_t net::engine::LocalInput()
{
    return CurrentNetInputs().mInputs[NetworkPlayerIdx()].mInputs;
}

void net::engine::SetPlayerInput(int player, uint32_t buttons)
{
    if (player >= 0 && player < MAX_PLAYERS)
    {
        CurrentNetInputs().mInputs[player].mInputs = buttons;
    }
}

unsigned short port_TypedChar(int dik); // compat/src/dinput.cpp: the character a key's last press typed, with shift

namespace
{
struct Typed
{
    s32 dik;
    u16 typed;
};
Typed gTyped[MAX_PLAYERS];
const uint32_t kKeyDown = 0x200000; // a key event's press, its DirectInput code in bits 12-20 (Source/input_bits.hpp)

u16 ChatChar(Player* pPlayer, s32 dik)
{
    const int i = pPlayer ? pPlayer->mPlayerIdx : -1;
    return i >= 0 && i < MAX_PLAYERS && gTyped[i].dik == dik ? gTyped[i].typed : 0;
}
} // namespace

void net::engine::SetPlayerTyped(int player, uint32_t buttons, uint16_t typed)
{
    if (player >= 0 && player < MAX_PLAYERS)
    {
        gTyped[player].dik = buttons & kKeyDown ? (s32)((buttons >> 12) & 0x1FF) : -1;
        gTyped[player].typed = typed;
        gPortChatChar = ChatChar;
    }
}

uint16_t net::engine::TypedChar(uint32_t buttons)
{
    return buttons & kKeyDown ? port_TypedChar((int)((buttons >> 12) & 0x1FF)) : 0;
}

bool net::engine::MenuOpen()
{
    return PortPauseMenu::IsOpen();
}

void net::engine::MenuKey(uint32_t dik, uint16_t typed)
{
    PortPauseMenu::HandleNetworkKey((s32)dik, typed);
}

net::engine::MenuChoice net::engine::TakeMenuChoice()
{
    return (MenuChoice)PortPauseMenu::TakeNetworkChoice();
}

void net::engine::OfferMenu(const std::vector<PlayerRow>& players, const char* scoreHeading, int ready, const std::string& vote)
{
    if (!PortPauseMenu::IsOpen())
    {
        return;
    }
    PortMenu::NetworkPlayer rows[PortMenu::kMaxNetworkPlayers];
    int n = 0;
    for (size_t i = 0; i < players.size() && n < PortMenu::kMaxNetworkPlayers; i++, n++)
    {
        PortMenu::NetworkPlayer& row = rows[n];
        size_t c = 0;
        for (; c < players[i].name.size() && c < GTA2_COUNTOF(row.mName) - 1; c++)
        {
            row.mName[c] = (wchar_t)(unsigned char)toupper((unsigned char)players[i].name[c]); // as the menu's fonts have them
        }
        row.mName[c] = 0;
        row.mScore = players[i].score;
        row.mDeaths = players[i].deaths;
        row.mPing = players[i].ping;
        row.mYou = players[i].you;
    }
    static wchar_t sHeading[16], sVote[64];
    size_t c = 0;
    for (; scoreHeading[c] && c < GTA2_COUNTOF(sHeading) - 1; c++)
    {
        sHeading[c] = (wchar_t)scoreHeading[c];
    }
    sHeading[c] = 0;
    for (c = 0; c < vote.size() && c < GTA2_COUNTOF(sVote) - 1; c++)
    {
        sVote[c] = (wchar_t)(unsigned char)toupper((unsigned char)vote[c]);
    }
    sVote[c] = 0;
    PortMenu::OfferNetwork(rows, n, sHeading, ready, vote.empty() ? NULL : sVote);
}

bool port_PadMode(); // compat/src/pad.cpp
double port_ClockSpeed(); // compat/src/kernel.cpp: GTA2_SPEED

void net::engine::SetRoundPad(bool results)
{
    gPortNetRoundPad = results ? 2 : 1;
}

bool net::engine::PadMode()
{
    return port_PadMode();
}

void net::engine::IdleWait()
{
    // The game spins on its message loop and RunGameFrame until winmain.cpp's sMatchStartTime, as the original does,
    // and would keep a core busy. Never when a step is due (lockstep runs, replays), so nothing runs slower
    static const u32& sNextStep = PortStatic<u32>(0x6F5A28);
    if ((s32)(sNextStep - timeGetTime()) > 1)
    {
        Sleep(1);
    }
}

int net::engine::NetworkWaitMs()
{
    const int kMarginMs = 2, kMostMs = 50;
    static const u32& sNextStep = PortStatic<u32>(0x6F5A28);
    const int ms = (int)((s32)(sNextStep - timeGetTime()) / port_ClockSpeed()) - kMarginMs; // GTA2_SPEED: the game's ms
    return ms <= 0 ? 0 : ms < kMostMs ? ms : kMostMs;
}

void net::engine::HoldStep()
{
    static u32& sNextStep = PortStatic<u32>(0x6F5A28);
    sNextStep = timeGetTime() + (gGame_0x40 ? gGame_0x40->GetFrameDurationMs() : 33);
}

void net::engine::SetTickMs(int ms)
{
    if (gGame_0x40 && ms > 0)
    {
        gGame_0x40->mNetworkFrameMs = ms; // Game::GetFrameDurationMs in network games
    }
}

void net::engine::SeedRandom(uint32_t seed)
{
    PortRand::SetState(seed);
    PortRand::SetSeed(seed);
}

// --- Snapshots ---

void net::engine::CapturePed(Ped* p, Car* driving, Car* riding, Snapshot& out)
{
    // On foot, driving or riding (a driver or passenger is where its car is); players are their own entities
    Car* in = driving ? driving : riding;
    Sprite* s = in ? in->mCarSprite : !p->mCar ? CharSprite(p) : NULL;
    if (!s || p->mPedType == ped_type::player_2 || !(in || p->IsActive()) || IsSpareDriver(p)) // (drivers aren't "active")
    {
        return;
    }
    EntityState e = Entity(kPedIdBase + p->mId, kEntityPed);
    PutPosition(e, s);
    e.f[kPedRemap] = (uint8_t)p->mRemap;
    e.f[kPedCar] = CarRef(in); // a passenger's has the game's passenger flag with its state (PassengerThere)
    e.f[kPedState] = InCarTransition(p) ? 1 : 0;
    CapturePedState(p, e);
    out.entities.push_back(e);
}

void net::engine::Capture(Snapshot& out)
{
    out.entities.clear();
    CaptureWorld(out);
    for (int i = 0; i < PlayerCount(); i++)
    {
        CapturePlayer(i, out);
    }
    // Drivers by their cars: a traffic car knows its driver (Car::mDriver), but the driver's own mCar
    // needn't point back (measured: 1 of 12 did)
    std::unordered_map<const Ped*, Car*> driving, riding;
    for (Car* c = gCar_BC_Pool->mPool.mpPrev; c; c = c->mpNext)
    {
        if (c->mCarSprite)
        {
            CaptureCar(c, out);
            if (Car* trailer = TowedTrailer(c)) // off the pool's list while towed (Car::AttachTrailer)
            {
                CaptureCar(trailer, out);
            }
            if (c->mDriver && !IsSpareDriver(c->mDriver))
            {
                driving[c->mDriver] = c;
            }
            // Its passengers: those sitting in it (one getting in or out is on foot, its char at the door)
            for (PedListNode* n = c->mPassengersList.mpFirstPed; n; n = n->mpNext)
            {
                Ped* q = n->mCharPed;
                if (q && q != c->mDriver && IsPassengerOf(q, c))
                {
                    riding[q] = c;
                }
            }
        }
    }
    for (Ped* p = gPedPool->mPool.mpPrev; p; p = p->mpNext)
    {
        auto d = driving.find(p);
        auto r = riding.find(p);
        CapturePed(p, d != driving.end() ? d->second : NULL, d == driving.end() && r != riding.end() ? r->second : NULL, out);
    }
    CapturePickups(out);
    CaptureTrains(out);
    SortById(out);
}

void net::engine::Apply(const Snapshot& snap)
{
    if (const EntityState* w = FindWorld(snap))
    {
        ApplyWorld(*w);
    }
    const s16 zoneTurn = ZoneTurn(); // the server's: a player put in the game here takes a restart zone in its turn
    ApplySlots(snap);
    ZoneTurn() = zoneTurn;
    for (const EntityState& e : snap.entities)
    {
        if (e.type == kEntityPlayer)
        {
            ApplyPlayer(e);
        }
    }
    ApplyCarsAndPeds(snap);
    ApplyPickups(snap);
}

// --- A client's view ---

void net::engine::Poses(std::vector<Pose>& out)
{
    out.clear();
    for (int i = 0; i < PlayerCount(); i++)
    {
        Ped* ped = InGamePed(i);
        Sprite* s = ped && !ped->mCar ? CharSprite(ped) : NULL;
        if (s)
        {
            out.push_back(Pose{(uint32_t)(1 + i), s->mXy.x.mValue, s->mXy.y.mValue, s->mZpos.mValue, s->mRotation.rValue});
        }
    }
    ReplicaPoses(out);
}

bool net::engine::PoseOf(const EntityState& e, Pose* out)
{
    if ((e.type == kEntityPlayer && !e.f[kPlayerInCar]) || e.type == kEntityCar || e.type == kEntityPed)
    {
        *out = Pose{e.id, e.f[0], e.f[1], e.f[2], e.f[3]};
        return true;
    }
    return false;
}

void net::engine::SetPose(const Pose& pose)
{
    if (pose.id >= kCarIdBase)
    {
        SetReplicaPose(pose);
        return;
    }
    Ped* ped = InGamePed((int)pose.id - 1);
    if (ped && !ped->mCar && !AtCarDoor(ped) && !ped->isDead())
    {
        MovePed(ped, Raw(pose.x), Raw(pose.y), Raw(pose.z), Angle(pose.rot));
    }
}
