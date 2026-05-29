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
//
// CHECK:       @_ZTIi = external constant
// CHECK-LABEL: define dso_local i32 @add_one(
// CHECK:       %exclow.error.flag = alloca i1
// CHECK:       store ptr @_ZTIi, ptr %exclow.error.typeinfo
// LLVM 18 emits `@llvm.eh.typeid.for(ptr ...)`; LLVM 20+ emits the type-
// overloaded `@llvm.eh.typeid.for.p0(ptr ...)`. Match both via prefix.
// CHECK-DAG:   call i32 @llvm.eh.typeid.for
// CHECK-DAG:   load ptr, ptr %exclow.error.typeinfo
//
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
