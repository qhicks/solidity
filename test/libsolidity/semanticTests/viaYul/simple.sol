contract C {
	fallback() external {
		assembly {
			function f(x, y) { mstore(0x00, x) mstore(0x20, y) }
			f(0x42, 0x21)
			return(0,0x40)

		}
	}
}
// ====
// compileViaYul: true
// allowNonExistingFunctions: true
// ----
// f(uint256): 3 ->
