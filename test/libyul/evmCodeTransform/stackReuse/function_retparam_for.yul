{
    function f() -> x { pop(address()) for { pop(callvalue()) } 0 {} { } }
}
// ====
// stackOptimization: true
// ----
//   stop
