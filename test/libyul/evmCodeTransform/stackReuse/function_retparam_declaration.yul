{
    function f() -> x { pop(address()) let y := callvalue() }
}
// ====
// stackOptimization: true
// ----
//   stop
