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

#include <libyul/backends/evm/DataFlowGraph.h>

#include <map>

namespace solidity::yul
{

struct StackLayout
{
	struct BlockInfo
	{
		Stack entryLayout;
		Stack exitLayout;
	};
	std::map<DFG::BasicBlock const*, BlockInfo> blockInfos;
	std::map<DFG::Operation const*, Stack> operationEntryStacks;
};

class StackLayoutGenerator
{
public:
	static StackLayout run(DFG const& _dfg);

private:
	StackLayoutGenerator(StackLayout& _context);

	/// @returns the optimal entry stack layout, s.t. @a _operation can be applied to it and
	/// the result can be transformed to @a _exitStack with minimal stack shuffling.
	Stack determineOptimalLayoutBeforeOperation(Stack _exitStack, DFG::Operation const& _operation);

	/// @returns the desired stack layout at the entry of @a _block, assuming the layout after
	/// executing the block should be @a _exitStack.
	Stack determineBlockEntry(Stack _exitStack, DFG::BasicBlock const& _block);

	void processEntryPoint(DFG::BasicBlock const* _entry);
	void stitchTogether(DFG::BasicBlock& _block, std::set<DFG::BasicBlock const*>& _visited);

	static Stack combineStack(Stack const& _stack1, Stack const& _stack2);

	StackLayout& m_layout;
};

}
