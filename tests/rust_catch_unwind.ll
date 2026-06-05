; Rust `catch_unwind` lowering: a `catchswitch` with a Rust-typed catchpad and
; a foreign catch-all catchpad, mirroring rustc's `__rust_try` shim on
; x86_64-pc-windows-msvc.
;
; Because this function *catches* (it has catchpads), the default `--mode=auto`
; routes it through the FULL Itanium+MSVC lowering rather than the cleanup-only
; path: the `catchswitch` becomes a typed `icmp eq` dispatch chain against the
; in-flight type descriptor, the funclet bundle is stripped, and every SEH
; funclet construct is removed. This is the "catch_unwind fixture lowers without
; error" acceptance case from the proposal.

target triple = "x86_64-pc-windows-msvc"

@rust_panic_ti = external constant i8

declare void @do_call(ptr, ptr)
declare void @on_catch(ptr)

define i32 @rust_try(ptr %try_fn, ptr %data) personality ptr @__CxxFrameHandler3 {
entry:
  invoke void @do_call(ptr %try_fn, ptr %data)
          to label %normal unwind label %cs_bb

cs_bb:
  %cs = catchswitch within none [label %cp_rust, label %cp_foreign] unwind to caller

cp_rust:
  %c1 = catchpad within %cs [ptr @rust_panic_ti, i32 8, ptr null]
  call void @on_catch(ptr %data) [ "funclet"(token %c1) ]
  catchret from %c1 to label %caught

cp_foreign:
  %c2 = catchpad within %cs [ptr null, i32 64, ptr null]
  catchret from %c2 to label %caught

normal:
  ret i32 0

caught:
  ret i32 1
}

declare ptr @__CxxFrameHandler3(...)

; The full path materialises the module-level error-state globals and the
; canonical type descriptor, and lowers the catchswitch to a typed dispatch.
; CHECK:       @__exclow_error_flag = internal global i1 false
; CHECK-LABEL: define i32 @rust_try(
; CHECK:       icmp eq ptr %exclow.ti, @rust_panic_ti
;
; CHECK-NOT:   catchswitch
; CHECK-NOT:   catchpad
; CHECK-NOT:   cleanuppad
; CHECK-NOT:   catchret
; CHECK-NOT:   cleanupret
; CHECK-NOT:   invoke
; CHECK-NOT:   "funclet"
; CHECK-NOT:   __CxxFrameHandler3
; CHECK-NOT:   personality
