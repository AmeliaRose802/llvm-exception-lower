// Single try / catch (int). Falls through; spec add_one(x) == x + 1 still SAT.
#include <cstdio>

extern "C" unsigned add_one(unsigned x) {
    try {
        if (x == 42u) throw 1;
    } catch (int e) {
        std::printf("caught %d\n", e);
    }
    return x + 1;
}
