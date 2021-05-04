{
    function f() -> x, y, z { pop(callvalue()) }
}
// ====
// stackOptimization: true
// ----
//   stop
