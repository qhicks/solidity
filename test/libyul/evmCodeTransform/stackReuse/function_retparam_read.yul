{
    function f() -> x { pop(address()) sstore(0, x) pop(callvalue()) }
}
// ====
// stackOptimization: true
// ----
//   stop
