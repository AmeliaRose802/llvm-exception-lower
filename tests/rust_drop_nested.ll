; Rust Win64 SEH drop glue with *nested* cleanup funclets.
;
; A struct with several drop-glue fields produces a chain of cleanup pads: the
; first `cleanupret` unwinds to a second `cleanuppad` rather than to the caller.
; This exercises the cleanup-only path's deletion of a chain of mutually
; unreachable funclet blocks (each only reachable via an unwind edge that the
; pass removes).
;
; Auto-detected as cleanup-only (cleanup pads only, no catch). After lowering,
; both funclet blocks are gone and only the straight-line normal path remains.

target triple = "x86_64-pc-windows-msvc"

declare void @drop_a(ptr)
declare void @drop_b(ptr)
declare void @drop_c(ptr)

define void @drop_nested(ptr %p) personality ptr @__CxxFrameHandler3 {
start:
  invoke void @drop_a(ptr %p)
          to label %ok unwind label %funclet1

funclet1:
  %cp1 = cleanuppad within none []
  call void @drop_b(ptr %p) [ "funclet"(token %cp1) ]
  cleanupret from %cp1 unwind label %funclet2

funclet2:
  %cp2 = cleanuppad within none []
  call void @drop_c(ptr %p) [ "funclet"(token %cp2) ]
  cleanupret from %cp2 unwind to caller

ok:
  call void @drop_b(ptr %p)
  call void @drop_c(ptr %p)
  ret void
}

declare ptr @__CxxFrameHandler3(...)

; CHECK-LABEL: define void @drop_nested(
; CHECK:       call void @drop_a(ptr %p)
; CHECK:       br label %ok
; CHECK:       ret void
;
; CHECK-NOT:   cleanuppad
; CHECK-NOT:   cleanupret
; CHECK-NOT:   invoke
; CHECK-NOT:   "funclet"
; CHECK-NOT:   __CxxFrameHandler3
; CHECK-NOT:   personality
; CHECK-NOT:   __exclow_error_flag
