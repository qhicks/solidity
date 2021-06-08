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
 * Transformation of a Yul AST into a data flow graph.
 */
#pragma once

#include <libyul/backends/evm/DataFlowGraph.h>

namespace solidity::yul
{

class DataFlowGraphBuilder
{
public:
	DataFlowGraphBuilder(DataFlowGraphBuilder const&) = delete;
	DataFlowGraphBuilder& operator=(DataFlowGraphBuilder const&) = delete;
	static std::unique_ptr<DFG> build(AsmAnalysisInfo& _analysisInfo, EVMDialect const& _dialect, Block const& _block);

	StackSlot operator()(Expression const& _literal);
	StackSlot operator()(Literal const& _literal);
	StackSlot operator()(Identifier const& _identifier);
	StackSlot operator()(FunctionCall const&);

	void operator()(VariableDeclaration const& _varDecl);
	void operator()(Assignment const& _assignment);
	void operator()(ExpressionStatement const& _statement);

	void operator()(Block const& _block);

	void operator()(If const& _if);
	void operator()(Switch const& _switch);
	void operator()(ForLoop const&);
	void operator()(Break const&);
	void operator()(Continue const&);
	void operator()(Leave const&);
	void operator()(FunctionDefinition const&);

private:
	DataFlowGraphBuilder(
		DFG& _graph,
		AsmAnalysisInfo& _analysisInfo,
		EVMDialect const& _dialect
	);
	DFG::Operation& visitFunctionCall(FunctionCall const&);

	Scope::Variable const& lookupVariable(YulString _name) const;
	std::pair<DFG::BasicBlock*, DFG::BasicBlock*> makeConditionalJump(StackSlot _condition);
	void makeConditionalJump(StackSlot _condition, DFG::BasicBlock& _nonZero, DFG::BasicBlock& _zero);
	void jump(DFG::BasicBlock& _target, bool _backwards = false);
	DFG& m_graph;
	AsmAnalysisInfo& m_info;
	EVMDialect const& m_dialect;
	DFG::BasicBlock* m_currentBlock = nullptr;
	Scope* m_scope = nullptr;
	struct ForLoopInfo
	{
		std::reference_wrapper<DFG::BasicBlock> afterLoop;
		std::reference_wrapper<DFG::BasicBlock> post;
	};
	std::optional<ForLoopInfo> m_forLoopInfo;
	DFG::BasicBlock* m_currentFunctionExit = nullptr;
};

}
