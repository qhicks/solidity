{ let x := 1 x := 6 let y := 2 y := 4 }
// ====
// stackOptimization: true
// ----
//     /* "":11:12   */
//   pop(0x01)
//     /* "":18:19   */
//   pop(0x06)
//     /* "":29:30   */
//   pop(0x02)
//     /* "":36:37   */
//   pop(0x04)
//   stop
