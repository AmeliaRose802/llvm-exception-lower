; Forced `--mode=cleanup-only` on a function that *does* have a catch handler.
;
; The `; EXCLOW-ARGS:` directive below tells the test runner to pass
; `--mode=cleanup-only` to exception-lower for this fixture. In that mode every
; function takes the cleanup-only path regardless of shape: the invoke's unwind
; edge is dropped, the funclet bundle stripped, and the catch funclet — which
; the panic-free path never enters — is conservatively deleted along with its
; landing block. The catch body (`@on_catch`) therefore does not survive.
;
; This documents the conservative "catch may be dropped/abort-modeled" behaviour
; the proposal calls out for the explicit cleanup-only flag.
;
; EXCLOW-ARGS: --mode=cleanup-only

target triple = "x86_64-pc-windows-msvc"

declare void @work(ptr)
declare void @on_catch(ptr)

define i32 @forced(ptr %p) personality ptr @__CxxFrameHandler3 {
entry:
  invoke void @work(ptr %p)
          to label %normal unwind label %cs_bb

cs_bb:
  %cs = catchswitch within none [label %cp] unwind to caller

cp:
  %c = catchpad within %cs [ptr null, i32 64, ptr null]
  call void @on_catch(ptr %p) [ "funclet"(token %c) ]
  catchret from %c to label %caught

normal:
  ret i32 0

caught:
  ret i32 1
}

declare ptr @__CxxFrameHandler3(...)

; CHECK-LABEL: define i32 @forced(
; CHECK:       call void @work(ptr %p)
; CHECK:       br label %normal
; CHECK:       ret i32 0
;
; The catch funclet and its body are conservatively dropped.
; CHECK-NOT:   call void @on_catch
; CHECK-NOT:   catchswitch
; CHECK-NOT:   catchpad
; CHECK-NOT:   catchret
; CHECK-NOT:   invoke
; CHECK-NOT:   "funclet"
; CHECK-NOT:   __CxxFrameHandler3
; CHECK-NOT:   personality
; CHECK-NOT:   __exclow_error_flag
