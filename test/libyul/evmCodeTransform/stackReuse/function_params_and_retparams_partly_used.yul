{
    function f(a, b, c, d) -> x, y { b := 3 let s := 9 y := 2 mstore(s, y) }
}
// ====
// stackOptimization: true
// ----
//   stop
