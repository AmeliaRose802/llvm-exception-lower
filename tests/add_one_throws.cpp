// Propagating throw (no catch in this function).
extern "C" unsigned add_one(unsigned x) {
    if (x == 42u) throw 1;
    return x + 1;
}

// MSVC `_CxxThrowException` is rewritten to error-state stores plus a sentinel
// return. The MSVC throw-info global `_TI1H` is mapped onto the canonical
// `@__exclow.td.H` synthesized symbol. Error state lives in module-level
// globals (`@__exclow_error_flag` / `_typeinfo` / `_value`) so it propagates
// across call boundaries.
//
// CHECK:       @__exclow_error_flag = internal global i1 false
// CHECK:       @__exclow_error_typeinfo = internal global ptr null
// CHECK:       @__exclow_error_value = internal global ptr null
// CHECK:       @__exclow.td.H = internal constant i8 0, align 1
// CHECK-LABEL: define dso_local i32 @add_one(
// CHECK:       store ptr @__exclow.td.H, ptr @__exclow_error_typeinfo
// CHECK:       store i1 true, ptr @__exclow_error_flag
// CHECK:       ret i32 0
//
// CHECK-NOT:   alloca i1
// CHECK-NOT:   _CxxThrowException
// CHECK-NOT:   __CxxFrameHandler3
// CHECK-NOT:   personality
// CHECK-NOT:   catchswitch
// CHECK-NOT:   catchpad
// CHECK-NOT:   cleanuppad
// CHECK-NOT:   invoke
// CHECK-NOT:   "funclet"
