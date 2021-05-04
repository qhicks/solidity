{ let z := 0 switch z case 0 { let x := 2 let y := 3 } default { z := 3 } let t := 9 }
// ====
// stackOptimization: true
// ----
//     /* "":11:12   */
//   jumpi(tag_1, eq(0x00, 0x00))
// tag_2:
//     /* "":70:71   */
//   0x03
//     /* "":65:71   */
//   pop
//   jump(tag_3)
// tag_1:
//     /* "":40:41   */
//   0x02
//     /* "":31:41   */
//   pop
//     /* "":51:52   */
//   0x03
//     /* "":42:52   */
//   pop
//   jump(tag_3)
// tag_3:
//     /* "":83:84   */
//   0x09
//     /* "":74:84   */
//   pop
//   stop
