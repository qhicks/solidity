/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Stack layout generator for Yul to EVM code generation.
 */

#pragma once

#include <libyul/DataFlowGraph.h>

#include <map>

namespace solidity::yul
{

struct OptimizedCodeTransformContext;

class StackLayoutGenerator
{
public:
	static void run(OptimizedCodeTransformContext& _context);

	Stack operator()(DFG::BasicBlock const& _block, Stack _initialExitLayout);

	void operator()(DFG::Operation const& _operation);

	void operator()(DFG::BuiltinCall const& _builtinCall);
	void operator()(DFG::FunctionCall const& _functionCall);
	void operator()(DFG::Assignment const& _literal);

private:
	StackLayoutGenerator(OptimizedCodeTransformContext& _context);

	void stitchTogether(DFG::BasicBlock& _block, std::set<DFG::BasicBlock const*>& _visited);

	OptimizedCodeTransformContext& m_context;

	Stack* m_stack;

	// TODO: name!
	std::map<DFG::BasicBlock const*, Stack> m_initialExitLayoutOnLastVisit;

	Stack combineStack(Stack const& _stack1, Stack const& _stack2);
};

}
