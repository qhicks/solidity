{
    let b := 3
    function f(a, r) -> t {
        // r could be removed right away, but a cannot - this is not implemented, though
        let x := a a := 3 t := a
    }
    b := 7
}
// ====
// stackOptimization: true
// ----
//     /* "":15:16   */
//   0x03
//     /* "":6:16   */
//   pop
//     /* "":182:183   */
//   0x07
//     /* "":177:183   */
//   pop
//   stop
