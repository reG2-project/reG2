// port/net (port/docs/netcode.md, "Cars in turn"): the car pool's update with a car ahead of one following it moved first
// (core/car_order.hpp). The game moves its cars one by one, each against the others' places as they are: a follower
// moved before the car it chases hits it where it was a tick ago, and is stopped a tick's move short of where it is drawn.
#include "internal.hpp"
#include "../core/car_order.hpp"
#include "dev/comfort.hpp"
#include "Source/Car.hpp"
#include "Source/CarAI.hpp"
#include "Source/CarPhysics.hpp"
#include "port/src/net_hooks.hpp"
#include "port/src/rand/owner.hpp"
#include <stdlib.h>
#include <unordered_set>
#include <vector>

namespace
{
net::CarMotion Motion(Car* car)
{
    net::CarMotion m = {car->mCarSprite ? car->mCarSprite->mXy.x.ToFloat() : 0.0f, car->mCarSprite ? car->mCarSprite->mXy.y.ToFloat() : 0.0f, 0, 0};
    if (car->mPhysics && car->mCarSprite && !car->IsTrainModel())
    {
        const Fix16_Point v = car->mPhysics->get_linvel();
        m.vx = v.x.ToFloat();
        m.vy = v.y.ToFloat();
    }
    return m;
}

bool InPool(Car* car)
{
    for (Car* c = gCar_BC_Pool->mPool.GetFirstUsed(); c; c = c->mpNext)
    {
        if (c == car)
        {
            return true;
        }
    }
    return false;
}

// A car reacts to a hit in its next update (CarAI::ManageCollisions): not to a car freed since, which a car hit after its
// own update this tick may be by then
void ForgetFreedHitter(Car* car)
{
    if (car->mAI && car->mAI->mBf24.bCollidedWithCar && car->mAI->mCarInCollision && !car->mAI->mCarInCollision->mCarSprite)
    {
        car->mAI->mBf24.bCollidedWithCar = false;
        car->mAI->mCarInCollision = NULL;
    }
}

void Update(Car* car)
{
    gCar_BC_Pool->mPool.field_X_count++;
    ForgetFreedHitter(car);
    PortRand::Scope scope(car);
    if (car->PoolUpdate())
    {
        gCar_BC_Pool->mPool.FindAndDeAllocate(car);
    }
}

void UpdateCars()
{
    std::vector<Car*> cars;
    std::vector<net::CarMotion> motion;
    for (Car* c = gCar_BC_Pool->mPool.GetFirstUsed(); c; c = c->mpNext)
    {
        cars.push_back(c);
        motion.push_back(Motion(c));
    }
    for (Car* c : cars)
    {
        ForgetFreedHitter(c);
    }
    std::vector<int> order;
    if (!net::CarOrder(motion, order))
    {
        // The pool's own walk (Pool::UpdatePool) through Update: after a tick in another order, a car can hold a hitter
        // that came after it then and frees itself before it now (despawned_6, which CarAI::ManageCollisions reads on)
        gCar_BC_Pool->mPool.field_X_count = 0;
        for (Car* c = gCar_BC_Pool->mPool.GetFirstUsed(); c;)
        {
            Car* next = c->mpNext;
            Update(c);
            c = next;
        }
        return;
    }
    net::engine::ComfortCarTurns(cars, order);
    gCar_BC_Pool->mPool.field_X_count = 0;
    const std::unordered_set<Car*> before(cars.begin(), cars.end());
    for (int i : order)
    {
        if (InPool(cars[i])) // not freed by another car's update
        {
            Update(cars[i]);
        }
    }
    // The cars appended during the update, after the last of those before it, as the pool's own walk reaches them
    std::vector<Car*> appended;
    for (Car* c = gCar_BC_Pool->mPool.GetFirstUsed(); c; c = c->mpNext)
    {
        if (before.count(c))
        {
            appended.clear();
        }
        else
        {
            appended.push_back(c);
        }
    }
    for (Car* c : appended)
    {
        if (InPool(c))
        {
            Update(c);
        }
    }
}
} // namespace

// GTA2_NET_CARTURNS=0: the game's own order (a test's, to compare; the server and its clients must agree)
void net::engine::InstallCarTurns(bool on)
{
    const char* env = getenv("GTA2_NET_CARTURNS");
    gPortUpdateCars = on && !(env && env[0] == '0') ? UpdateCars : NULL;
}
