// Single try / catch (int). Falls through; spec add_one(x) == x + 1 still SAT.
//
// `printf` is forward-declared rather than included via <cstdio> so the
// fixture cross-compiles for `-target x86_64-pc-windows-msvc` from a Linux
// host (where MSVC headers are not on the include path).
extern "C" int printf(const char *, ...);

extern "C" unsigned add_one(unsigned x) {
    try {
        if (x == 42u) throw 1;
    } catch (int e) {
        printf("caught %d\n", e);
    }
    return x + 1;
}

// Both sides (throw site and catchswitch dispatch) must route through the same
// canonical synthesized type-descriptor symbol, otherwise the typed `icmp eq`
// can never match (Issue 1 fix). The synthesized symbol is `internal constant`
// rather than `linkonce_odr` so SAW's globals allocator can model it (Issue 2).
//
// CHECK:       @__exclow.td.H = internal constant i8 0, align 1
// CHECK-LABEL: define dso_local i32 @add_one(
// CHECK:       %exclow.error.flag = alloca i1
// CHECK-DAG:   store ptr @__exclow.td.H, ptr %exclow.error.typeinfo
// CHECK-DAG:   icmp eq ptr %exclow.ti, @__exclow.td.H
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
