abstract contract A {
	uint x;
	function f() mod public {
		assembly { return(0,0) }
	}
	modifier mod() virtual;

}
