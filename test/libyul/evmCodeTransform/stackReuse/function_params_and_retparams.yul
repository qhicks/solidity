// This does not re-use the parameters for the return parameters
// We do not expect parameters to be fully unused, so the stack
// layout for a function is still fixed, even though parameters
// can be re-used.
{
    function f(a, b, c, d) -> x, y { }
}
// ====
// stackOptimization: true
// ----
//   stop
