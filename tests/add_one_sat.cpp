// No exception handling at all. Pre-pass: SAT (add_one(x) == x + 1).
extern "C" unsigned add_one(unsigned x) {
    return x + 1;
}

// The pass must be a no-op on functions with no EH constructs.
//
// CHECK-LABEL: define dso_local i32 @add_one(
// CHECK-NOT:   exclow
// CHECK-NOT:   __exclow.td
// CHECK-NOT:   alloca i1
// CHECK-NOT:   personality
// CHECK-NOT:   landingpad
// CHECK-NOT:   catchswitch
// CHECK-NOT:   catchpad
// CHECK-NOT:   cleanuppad
// CHECK-NOT:   _CxxThrowException
// CHECK-NOT:   __CxxFrameHandler3
// CHECK-NOT:   __gxx_personality_v0
