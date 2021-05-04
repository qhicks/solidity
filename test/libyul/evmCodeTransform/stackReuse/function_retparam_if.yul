{
    function f() -> x { pop(address()) if 1 { pop(callvalue()) } }
}
// ====
// stackOptimization: true
// ----
//   stop
