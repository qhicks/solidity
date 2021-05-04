{ for { let z := 0 } 1 { } { let x := 3 } let t := 2 }
// ====
// stackOptimization: true
// ----
//     /* "":17:18   */
//   0x00
//     /* "":8:18   */
//   pop
//   jump(tag_1)
// tag_1:
//     /* "":38:39   */
//   0x03
//     /* "":29:39   */
//   pop
//   jump(tag_1)
