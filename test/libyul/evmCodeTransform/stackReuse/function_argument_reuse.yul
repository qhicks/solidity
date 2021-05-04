{
    function f(a, b, c) -> x { pop(address()) sstore(a, c) pop(callvalue()) x := b }
}
// ====
// stackOptimization: true
// ----
//   stop
