{
    let z := 21
    let x := 0
    for {} x { } {
        sstore(z, z)
    }
}
// ====
// stackOptimization: true
// ----
