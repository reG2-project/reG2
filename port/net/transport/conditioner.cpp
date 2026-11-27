// port/net transport (port/docs/netcode.md, "The server"): a link conditioner for tests. It wraps a transport and holds every
// send back by a delay with jitter and spikes, and drops some unreliable ones, alone or in bursts. On both ends, a round
// trip gets twice the delay.
#include "transport.hpp"
#include "../core/protocol.hpp"
#include <chrono>
#include <deque>
#include <stdio.h>
#include <stdlib.h>

namespace
{
class Conditioner : public net::Transport
{
  public:
    Conditioner(net::Transport* inner, const net::LinkShape& shape) : mInner(inner), mShape(shape), mStart(Now())
    {
        // GTA2_NET_CUT=<every ms>:<ms>: the link is cut that long that often, every unreliable send lost (an outage)
        const char* cut = getenv("GTA2_NET_CUT");
        if (cut && sscanf(cut, "%d:%d", &mCutEveryMs, &mCutMs) != 2)
        {
            mCutEveryMs = mCutMs = 0;
        }
    }
    ~Conditioner() override
    {
        delete mInner;
    }

    bool Poll(net::TransportEvent& event) override
    {
        Release();
        return mInner->Poll(event);
    }

    bool Wait(net::TransportEvent& event, int ms) override
    {
        Release();
        return mHeld.empty() ? mInner->Wait(event, ms) : mInner->Poll(event); // held sends go out on time
    }

    bool Send(uint32_t peer, uint8_t channel, const void* data, size_t size, bool reliable) override
    {
        if (!reliable && size > net::MaxPayload())
        {
            return false; // the same refusal as the real one
        }
        if (!reliable && mCutEveryMs > 0 && (Now() - mStart) % mCutEveryMs >= mCutEveryMs - mCutMs)
        {
            return true; // the link is cut
        }
        if (!reliable && Lost())
        {
            return true; // lost on the way
        }
        int delay = mShape.lagMs + Spike() + Stall();
        if (mShape.jitterMs > 0)
        {
            delay += (int)(Next() % (2 * mShape.jitterMs + 1)) - mShape.jitterMs;
        }
        Held h;
        h.due = Now() + (delay > 0 ? delay : 0);
        if (!mHeld.empty() && h.due < mHeld.back().due)
        {
            h.due = mHeld.back().due; // One queue in order, so jitter never reorders (ENet's sequencing would drop them)
        }
        h.peer = peer;
        h.channel = channel;
        h.reliable = reliable;
        h.data.assign((const uint8_t*)data, (const uint8_t*)data + size);
        mHeld.push_back(h);
        return true;
    }

    void Flush() override
    {
        Release();
    }

    void Disconnect(uint32_t peer) override
    {
        mInner->Disconnect(peer);
    }

    uint32_t RoundTripMs(uint32_t peer) const override
    {
        return mInner->RoundTripMs(peer); // includes our delay: ENet measures it on the wire
    }

  private:
    struct Held
    {
        long long due;
        uint32_t peer;
        uint8_t channel;
        bool reliable;
        std::vector<uint8_t> data;
    };

    static long long Now()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Independent losses, or Gilbert-Elliott bursts: in the bad state every send is lost, it lasts lossBurst sends on
    // average, and the good state ends as often as keeps the mean rate at lossPercent
    bool Lost()
    {
        const double p = mShape.lossPercent / 100.0;
        if (mShape.lossBurst <= 1 || p <= 0 || p >= 1)
        {
            return (int)(Next() % 100) < mShape.lossPercent;
        }
        const double draw = (Next() % 1000000) / 1000000.0;
        if (mBad)
        {
            mBad = draw >= 1.0 / mShape.lossBurst;
        }
        else
        {
            mBad = draw < p / (mShape.lossBurst * (1 - p));
        }
        return mBad;
    }

    // The spike's extra delay now: ramped in over the first second of each period's <on> seconds, out over the next
    int Spike() const
    {
        if (!mShape.spikeMs || mShape.spikeEveryS <= 0)
        {
            return 0;
        }
        const long long t = (Now() - mStart) % (mShape.spikeEveryS * 1000LL);
        const long long on = mShape.spikeOnS * 1000LL;
        const double ramp = t < 1000 ? t / 1000.0 : t < on ? 1.0 : t < on + 1000 ? 1.0 - (t - on) / 1000.0 : 0.0;
        return (int)(mShape.spikeMs * ramp);
    }

    // A stall at the start of each period: what is sent during it waits for its end, then goes at once (a Wi-Fi hiccup)
    int Stall() const
    {
        if (!mShape.stallMs || mShape.stallEveryS <= 0)
        {
            return 0;
        }
        const long long t = (Now() - mStart) % (mShape.stallEveryS * 1000LL);
        return t < mShape.stallMs ? (int)(mShape.stallMs - t) : 0;
    }

    uint32_t Next()
    {
        mSeed = mSeed * 1103515245u + 12345u;
        return mSeed >> 8;
    }

    void Release()
    {
        const long long now = Now();
        bool any = false;
        while (!mHeld.empty() && mHeld.front().due <= now)
        {
            const Held& h = mHeld.front();
            mInner->Send(h.peer, h.channel, h.data.data(), h.data.size(), h.reliable);
            mHeld.pop_front();
            any = true;
        }
        if (any)
        {
            mInner->Flush();
        }
    }

    net::Transport* mInner;
    net::LinkShape mShape;
    long long mStart;
    bool mBad = false;
    int mCutEveryMs = 0, mCutMs = 0;
    uint32_t mSeed = 12345;
    std::deque<Held> mHeld;
};
} // namespace

net::Transport* net::CreateConditioner(Transport* inner, const LinkShape& shape)
{
    return new Conditioner(inner, shape);
}
