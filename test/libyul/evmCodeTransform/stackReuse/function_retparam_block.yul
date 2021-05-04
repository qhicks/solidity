{
    function f() -> x { pop(address()) { pop(callvalue()) } }
}
// ====
// stackOptimization: true
// ----
//   stop
