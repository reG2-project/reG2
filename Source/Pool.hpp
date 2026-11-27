#pragma once
#include "types.hpp"
#if defined(GTA2_PORT)
#include "../port/src/rand/owner.hpp" // PortRand::Scope: who a random draw belongs to (port/docs/netcode.md)
#include "../port/src/pool/guard.hpp"
#endif

#if defined(GTA2_PORT)
// Room in a pool for n entries, within GTA2_POOL_LIMIT's cap (tests) if any. False records the refusal
#define PORT_POOL_ROOM_METHODS(head)                                                                    \
    static PortPool::Info* PortInfo()                                                                   \
    {                                                                                                   \
        static PortPool::Info* sInfo = PortPool::Register(__PRETTY_FUNCTION__, PoolSize);               \
        return sInfo;                                                                                   \
    }                                                                                                   \
    s32 PortFree(s32 upTo = PoolSize) const                                                             \
    {                                                                                                   \
        s32 n = 0;                                                                                      \
        for (const PoolType* p = head; p && n < upTo; p = p->mpNext)                                    \
        {                                                                                               \
            n++;                                                                                        \
        }                                                                                               \
        return n;                                                                                       \
    }                                                                                                   \
    bool PortRoom(s32 n = 1)                                                                            \
    {                                                                                                   \
        PortPool::Info* info = PortInfo();                                                              \
        const s32 need = n + (info->limit ? PoolSize - info->limit : 0);                                \
        if (head && (need <= 1 || PortFree(need) >= need))                                              \
        {                                                                                               \
            return true;                                                                                \
        }                                                                                               \
        PortPool::Refused(info, PoolSize - PortFree(need));                                             \
        return false;                                                                                   \
    }
#endif

// A free list only, with O(1) allocate and deallocate: unlike Pool, no list of the used items
template<typename PoolType, s32 PoolSize>
class PoolBasic
{
  public:
    __forceinline PoolBasic()
    {
        PoolType* pIter = &mPool[0];
        for (s32 i = 0; i < PoolSize - 1; i++)
        {
            pIter->mpNext = pIter + 1;
            pIter++;
        }

        mPool[PoolSize - 1].mpNext = NULL;
        mpHead = mPool;
    }

    /*~PoolBasic()
    {
        mpHead = 0;
    }*/

#if defined(GTA2_PORT)
    PORT_POOL_ROOM_METHODS(mpHead)
#endif

    PoolType* Allocate()
    {
#if defined(GTA2_PORT)
        if (!PortRoom())
        {
            return NULL; // full: the caller skips its creation (port/src/pool/guard.hpp)
        }
#endif
        PoolType* pNew = mpHead;
        mpHead = pNew->mpNext;
        pNew->PoolAllocate();
        return pNew;
    }

    void DeAllocate(PoolType* pItem)
    {
        pItem->PoolDeallocate();
        pItem->mpNext = mpHead;
        mpHead = pItem;
    }

    PoolType* UnlinkAndReturnNext(PoolType* pOldItem)
    {
        PoolType* pNewItem = pOldItem->mpNext;
        pOldItem->mpNext = this->mpHead;
        this->mpHead = pOldItem;
        return pNewItem;
    }

    PoolType* mpHead; // 0x0
    PoolType mPool[PoolSize]; // 0x4
};

template<typename PoolType, s32 PoolSize>
class Pool
{
  public:
    // 9.6f 0x48A8F0
    inline bool has_pStart()
    {
        return mpStart != NULL;
    }

    // 9.6f 0x4B8FD0 (CokeZero_FC, 10.5 copy 0x5935C0): first used item
    inline PoolType* GetFirstUsed()
    {
        return mpPrev;
    }

    __forceinline Pool()
    {
        PoolType* pIter = mPool;
        for (s32 i = 0; i < PoolSize - 1; i++)
        {
            pIter->mpNext = pIter + 1;
            pIter++;
        }

        mPool[PoolSize - 1].mpNext = NULL;
        mpStart = mPool;

        mpPrev = NULL;
        field_X_count = 0;
    }

    ~Pool()
    {
        mpStart = NULL;
        mpPrev = NULL;
    }

#if defined(GTA2_PORT)
    PORT_POOL_ROOM_METHODS(mpStart)
#endif

    PoolType* Allocate()
    {
#if defined(GTA2_PORT)
        if (!PortRoom())
        {
            return NULL; // full: the caller skips its creation (port/src/pool/guard.hpp)
        }
#endif
        PoolType* pNew = mpStart;
        mpStart = pNew->mpNext;
        pNew->mpNext = mpPrev;
        mpPrev = pNew;
        pNew->PoolAllocate();
        return pNew;
    }

    // alloc_append_4254A0
    PoolType* AllocateAppend()
    {
#if defined(GTA2_PORT)
        if (!PortRoom())
        {
            return NULL;
        }
#endif
        // Pop from free list
        PoolType* obj = mpStart;
        mpStart = obj->mpNext;
        obj->mpNext = NULL;

        // Append to active list (FIFO style)
        if (!mpPrev)
        {
            mpPrev = obj; // active list was empty
        }
        else
        {
            PoolType* tail = mpPrev;
            while (tail->mpNext)
            {
                tail = tail->mpNext;
            }
            tail->mpNext = obj;
        }

        obj->PoolAllocate(); // per-object init
        return obj;
    }

    PoolType* AllocateUntracked()
    {
#if defined(GTA2_PORT)
        if (!PortRoom())
        {
            return NULL;
        }
#endif
        PoolType* pNew = this->mpStart;
        this->mpStart = pNew->mpNext;
        pNew->mpNext = 0;
        pNew->PoolAllocate();
        return pNew;
    }

    void DeAllocate(PoolType* pItem)
    {
        pItem->PoolDeallocate();
        pItem->mpNext = mpStart;
        mpStart = pItem;
    }
    
    void Remove_NoDeAllocate(PoolType* pItem)
    {
        pItem->mpNext = mpStart;
        mpStart = pItem;
    }

    void UpdateNextPrev(PoolType* pItem)
    {
        pItem->mpNext = mpPrev;
        mpPrev = pItem;
    }

    void UpdatePool()
    {
        field_X_count = 0;

        PoolType* pPreviousItem = NULL;
        PoolType* pCurrItem = mpPrev;

        while (pCurrItem)
        {
            ++field_X_count;

            PoolType* pNext = pCurrItem->mpNext;
#if defined(GTA2_PORT)
            PortRand::Scope portRandScope(pCurrItem); // an entity's draws are its own in a network game (cars; other pools: none)
#endif

            if (pCurrItem->PoolUpdate())
            {
                pCurrItem->PoolDeallocate();

                if (pPreviousItem && pPreviousItem->mpNext != pCurrItem)
                {
                    pPreviousItem = NULL;
                }

                if (pPreviousItem)
                {
                    pPreviousItem->mpNext = pCurrItem->mpNext;
                }
                else
                {
                    if (mpPrev == pCurrItem)
                    {
                        mpPrev = pCurrItem->mpNext;
                    }
                    else
                    {
                        pPreviousItem = mpPrev;
                        while (pPreviousItem->mpNext != pCurrItem)
                        {
                            pPreviousItem = pPreviousItem->mpNext;
                        }
                        pPreviousItem->mpNext = pCurrItem->mpNext;
                    }
                }

                pCurrItem->mpNext = mpStart;
                mpStart = pCurrItem;
            }
            else
            {
                pPreviousItem = pCurrItem;
            }

            pCurrItem = pNext;
        }
    }

    void UpdatePoolNoDeallocate()
    {
        field_X_count = 0;

        PoolType* pPreviousItem = NULL;
        PoolType* pCurrItem = mpPrev;

        while (pCurrItem)
        {
            ++field_X_count;

            PoolType* pNext = pCurrItem->mpNext;
#if defined(GTA2_PORT)
            PortRand::Scope portRandScope(pCurrItem); // an entity's draws are its own in a network game (cars; other pools: none)
#endif

            if (pCurrItem->PoolUpdate())
            {
                if (pPreviousItem && pPreviousItem->mpNext != pCurrItem)
                {
                    pPreviousItem = NULL;
                }

                if (pPreviousItem)
                {
                    pPreviousItem->mpNext = pCurrItem->mpNext;
                }
                else
                {
                    if (mpPrev == pCurrItem)
                    {
                        mpPrev = pCurrItem->mpNext;
                    }
                    else
                    {
                        pPreviousItem = mpPrev;
                        while (pPreviousItem->mpNext != pCurrItem)
                        {
                            pPreviousItem = pPreviousItem->mpNext;
                        }
                        pPreviousItem->mpNext = pCurrItem->mpNext;
                    }
                }

                pCurrItem->mpNext = mpStart;
                mpStart = pCurrItem;
            }
            else
            {
                pPreviousItem = pCurrItem;
            }

            pCurrItem = pNext;
        }
    }

    PoolType* FindAndDeAllocate(PoolType* pToFind)
    {
        PoolType* pLast = NULL;

        for (PoolType* pIter = mpPrev; pIter; pIter = pIter->mpNext)
        {
            if (pIter == pToFind)
            {
                pIter->PoolDeallocate();

                if (pLast)
                {
                    pLast->mpNext = pIter->mpNext;
                }
                else
                {
                    mpPrev = pIter->mpNext;
                }
                pIter->mpNext = mpStart;
                mpStart = pIter;
                return pToFind;
            }
            pLast = pIter;
        }
        return pToFind;
    }

    // 9.6f 0x420F30
    void UnlinkFromActiveList(PoolType* toFind)
    {
        PoolType* pLast = 0;
        for (PoolType* pIter = this->mpPrev; pIter; pIter = pIter->mpNext)
        {
            if (pIter == toFind)
            {
                if (pLast)
                {
                    pLast->mpNext = pIter->mpNext;
                }
                else
                {
                    this->mpPrev = pIter->mpNext;
                }
                pIter->mpNext = 0;
                return;
            }
            pLast = pIter;
        }
    }

    PoolType* mpStart; // 0x0
    PoolType* mpPrev; // 0x4
    PoolType mPool[PoolSize]; // 0x8

    // Unknown offsets due to sizeof(PoolType) varying per template
    s16 field_X_count;
    s16 field_X_pad;
};
