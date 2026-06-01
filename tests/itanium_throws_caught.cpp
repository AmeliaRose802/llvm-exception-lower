// Itanium sanity test: no stdlib headers, single catch.
extern "C" int sink(int);

extern "C" unsigned add_one(unsigned x) {
    try {
        if (x == 42u) throw 1;
    } catch (int e) {
        sink(e);
    }
    return x + 1;
}

// Itanium typeinfo `@_ZTIi` is shared by the throw site and the landingpad
// clause already, so the canonicalisation layer must leave it alone.
// `__cxa_throw` / `landingpad` / `invoke` / `resume` / `__cxa_begin_catch` /
// `__cxa_end_catch` / `__gxx_personality_v0` are all replaced or stripped.
// Catch-side typeid dispatch is now a direct `icmp eq` against the stored
// typeinfo pointer, so `@llvm.eh.typeid.for` calls are eliminated entirely.
// Error state lives in module-level globals.
//
// CHECK:       @_ZTIi = external constant
// CHECK:       @__exclow_error_flag = internal global i1 false
// CHECK:       @__exclow_error_typeinfo = internal global ptr null
// CHECK:       @__exclow_error_value = internal global ptr null
// CHECK-LABEL: define dso_local i32 @add_one(
// CHECK:       store ptr @_ZTIi, ptr @__exclow_error_typeinfo
// CHECK-DAG:   load ptr, ptr @__exclow_error_typeinfo
//
// CHECK-NOT:   alloca i1
// CHECK-NOT:   call i32 @llvm.eh.typeid.for
// CHECK-NOT:   landingpad
// CHECK-NOT:   __cxa_throw
// CHECK-NOT:   __cxa_rethrow
// CHECK-NOT:   __cxa_begin_catch
// CHECK-NOT:   __cxa_end_catch
// CHECK-NOT:   __cxa_allocate_exception
// CHECK-NOT:   __cxa_free_exception
// CHECK-NOT:   __gxx_personality_v0
// CHECK-NOT:   invoke
// CHECK-NOT:   resume
// CHECK-NOT:   personality
// CHECK-NOT:   __exclow.td
