// Calls INFRA VR's queued render-thread functor the way the engine (MSVC, thiscall) does:
// AddRef, operator(), Release (which must free it).
#include "../src/engine/hooks_game.cpp"

#include <cstdio>

using namespace ivr;

typedef int(__attribute__((thiscall)) * RefFn)(void *);
typedef void(__attribute__((thiscall)) * CallFn)(void *);

int main()
{
    int ran = 0;
    QueuedCall *c = new QueuedCall;
    c->vtable = g_qcVtable;
    c->fn = [&ran] { ++ran; };
    c->refs = 0;
    void **vt = *reinterpret_cast<void ***>(c);
    int r1 = reinterpret_cast<RefFn>(vt[0])(c);  // ICallQueue::QueueFunctor's AddRef
    reinterpret_cast<CallFn>(vt[3])(c);          // operator()
    int r2 = reinterpret_cast<RefFn>(vt[1])(c);  // Release after the call: deletes it
    printf("addref %d, ran %d, release %d -> %s\n", r1, ran, r2, r1 == 1 && ran == 1 && r2 == 0 ? "PASSED" : "FAILED");
    return 0;
}
