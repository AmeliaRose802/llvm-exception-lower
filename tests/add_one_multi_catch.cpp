// Two catch arms. HarmlessTag falls through (spec preserved); HarmfulTag
// returns 0 (spec violated for x == 99). Expected counterexample: x == 99.
#include <cstdio>

struct HarmlessTag {};
struct HarmfulTag {};

extern "C" unsigned add_one(unsigned x) {
    try {
        if (x == 7u)  throw HarmlessTag{};
        if (x == 99u) throw HarmfulTag{};
    } catch (const HarmlessTag&) {
        std::printf("harmless\n");
    } catch (const HarmfulTag&) {
        return 0;
    }
    return x + 1;
}
