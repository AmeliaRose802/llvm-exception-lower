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

// Multi-arm catchswitch lowers to a typed `icmp eq` dispatch chain (G3) with
// an intermediate `exclow.catch.next` block separating the arms and a
// terminal `exclow.unhandled` sentinel. Each tag's throw-info `_TI1?AU<Tag>@@`
// and type-descriptor `??_R0?AU<Tag>@@@8` are folded onto one canonical
// `@"__exclow.td.?AU<Tag>@@"` symbol so both sides compare equal.
//
// CHECK-DAG:   @"__exclow.td.?AUHarmlessTag@@" = internal constant i8 0
// CHECK-DAG:   @"__exclow.td.?AUHarmfulTag@@" = internal constant i8 0
// CHECK-LABEL: define dso_local i32 @add_one(
// CHECK-DAG:   store ptr @"__exclow.td.?AUHarmlessTag@@", ptr %exclow.error.typeinfo
// CHECK-DAG:   store ptr @"__exclow.td.?AUHarmfulTag@@", ptr %exclow.error.typeinfo
// CHECK-DAG:   icmp eq ptr %exclow.ti, @"__exclow.td.?AUHarmlessTag@@"
// CHECK-DAG:   icmp eq ptr %exclow.ti, @"__exclow.td.?AUHarmfulTag@@"
// CHECK:       exclow.catch.next:
// CHECK:       exclow.unhandled:
//
// CHECK-NOT:   catchswitch
// CHECK-NOT:   catchpad
// CHECK-NOT:   cleanuppad
// CHECK-NOT:   catchret
// CHECK-NOT:   cleanupret
// CHECK-NOT:   _CxxThrowException
// CHECK-NOT:   __CxxFrameHandler3
// CHECK-NOT:   invoke
// CHECK-NOT:   "funclet"
