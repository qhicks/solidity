{
    function f() -> x { pop(callvalue()) }
}
// ====
// stackOptimization: true
// ----
//   stop
