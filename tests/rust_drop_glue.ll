; Rust Win64 SEH drop glue: a pure *cleanup* funclet, no catch dispatch.
;
; This mirrors the IR `rustc --emit=llvm-bc` produces for
; `core::ptr::drop_in_place::<T>` on x86_64-pc-windows-msvc: an `invoke` whose
; unwind edge targets a `cleanuppad` block, a `cleanupret ... unwind to caller`,
; and a `[ "funclet"(token %cp) ]` operand bundle on the call inside the
; funclet. SAW's llvm-pretty-bc-parser aborts on the operand bundle before any
; simulation can run.
;
; Under the default `--mode=auto`, this function is auto-detected as
; cleanup-only (it has cleanuppad/cleanupret and no catchpad/catchswitch). The
; pass drops the invoke's unwind edge (`call` + `br` to the normal dest), strips
; the funclet bundle, clears the `__CxxFrameHandler3` personality, and deletes
; the now-unreachable funclet blocks. The result is straight-line, funclet-free
; IR that parses cleanly. No `__exclow_*` error-state globals are introduced —
; the panic-free path needs none.

target triple = "x86_64-pc-windows-msvc"

declare void @drop_field(ptr)
declare void @drop_inner(ptr)

define void @drop_in_place(ptr %p) personality ptr @__CxxFrameHandler3 {
start:
  invoke void @drop_field(ptr %p)
          to label %ok unwind label %funclet

funclet:
  %cp = cleanuppad within none []
  call void @drop_inner(ptr %p) [ "funclet"(token %cp) ]
  cleanupret from %cp unwind to caller

ok:
  call void @drop_inner(ptr %p)
  ret void
}

declare ptr @__CxxFrameHandler3(...)

; CHECK-LABEL: define void @drop_in_place(
; CHECK:       call void @drop_field(ptr %p)
; CHECK:       br label %ok
; CHECK:       call void @drop_inner(ptr %p)
; CHECK:       ret void
;
; CHECK-NOT:   cleanuppad
; CHECK-NOT:   cleanupret
; CHECK-NOT:   invoke
; CHECK-NOT:   "funclet"
; CHECK-NOT:   __CxxFrameHandler3
; CHECK-NOT:   personality
; CHECK-NOT:   __exclow_error_flag
; CHECK-NOT:   __exclow.td
