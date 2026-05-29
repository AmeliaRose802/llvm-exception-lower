// Propagating throw (no catch in this function).
extern "C" unsigned add_one(unsigned x) {
    if (x == 42u) throw 1;
    return x + 1;
}

// MSVC `_CxxThrowException` is rewritten to error-state stores plus a sentinel
// return. The MSVC throw-info global `_TI1H` is mapped onto the canonical
// `@__exclow.td.H` synthesized symbol.
//
// CHECK:       @__exclow.td.H = internal constant i8 0, align 1
// CHECK-LABEL: define dso_local i32 @add_one(
// CHECK:       %exclow.error.flag = alloca i1
// CHECK:       %exclow.error.typeinfo = alloca ptr
// CHECK:       %exclow.error.value = alloca ptr
// CHECK:       store ptr @__exclow.td.H, ptr %exclow.error.typeinfo
// CHECK:       store i1 true, ptr %exclow.error.flag
// CHECK:       ret i32 0
//
// CHECK-NOT:   _CxxThrowException
// CHECK-NOT:   __CxxFrameHandler3
// CHECK-NOT:   personality
// CHECK-NOT:   catchswitch
// CHECK-NOT:   catchpad
// CHECK-NOT:   cleanuppad
// CHECK-NOT:   invoke
// CHECK-NOT:   "funclet"
