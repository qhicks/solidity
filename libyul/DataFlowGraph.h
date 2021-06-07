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

#pragma once

#include <libyul/AST.h>
#include <libyul/AsmAnalysisInfo.h>
#include <libyul/backends/evm/EVMDialect.h>

#include <libyul/optimiser/ASTWalker.h>
#include <libyul/Scope.h>

#include <vector>

namespace solidity::yul
{

struct FunctionCallReturnLabelSlot
{
	std::reference_wrapper<yul::FunctionCall const> call;
	bool operator==(FunctionCallReturnLabelSlot const& _rhs) const { return &call.get() == &_rhs.call.get(); }
	bool operator<(FunctionCallReturnLabelSlot const& _rhs) const { return &call.get() < &_rhs.call.get(); }
};
struct FunctionReturnLabelSlot
{
	bool operator==(FunctionReturnLabelSlot const&) const { return true; }
	bool operator<(FunctionReturnLabelSlot const&) const { return false; }
};
struct VariableSlot
{
	Scope::Variable const* variable = nullptr;
	std::shared_ptr<DebugData const> debugData{};
	bool operator==(VariableSlot const& _rhs) const { return variable == _rhs.variable; }
	bool operator<(VariableSlot const& _rhs) const { return variable < _rhs.variable; }
};
struct LiteralSlot
{
	u256 value;
	std::shared_ptr<DebugData const> debugData{};
	bool operator==(LiteralSlot const& _rhs) const { return value == _rhs.value; }
	bool operator<(LiteralSlot const& _rhs) const { return value < _rhs.value; }
};
struct TemporarySlot
{
	std::reference_wrapper<yul::FunctionCall const> call;
	size_t idx = 0;
	bool operator==(TemporarySlot const& _rhs) const { return &call.get() == &_rhs.call.get() && idx == _rhs.idx; }
	bool operator<(TemporarySlot const& _rhs) const { return std::make_pair(&call.get(), idx) < std::make_pair(&_rhs.call.get(), _rhs.idx); }
};
struct JunkSlot
{
	bool operator==(JunkSlot const&) const { return true; }
	bool operator<(JunkSlot const&) const { return false; }
};
using StackSlot = std::variant<FunctionCallReturnLabelSlot, FunctionReturnLabelSlot, VariableSlot, LiteralSlot, TemporarySlot, JunkSlot>;
using Stack = std::vector<StackSlot>;

struct DFG
{
	explicit DFG() {}
	DFG(DFG const&) = delete;
	DFG(DFG&&) = delete;
	DFG& operator=(DFG const&) = delete;
	DFG& operator=(DFG&&) = delete;
	struct BuiltinCall
	{
		std::shared_ptr<DebugData const> debugData;
		BuiltinFunctionForEVM const* builtin = nullptr;
		yul::FunctionCall const* functionCall = nullptr;
		size_t arguments = 0;
	};
	struct FunctionCall
	{
		std::shared_ptr<DebugData const> debugData;
		Scope::Function const* function = nullptr;
		yul::FunctionCall const* functionCall = nullptr;
	};
	struct Assignment
	{
		std::shared_ptr<DebugData const> debugData;
		std::vector<VariableSlot> variables;
	};

	struct Operation
	{
		Stack input;
		Stack output;
		std::variant<FunctionCall, BuiltinCall, Assignment> operation;
	};

	struct FunctionInfo;
	struct BasicBlock
	{
		std::vector<BasicBlock const*> entries;
		std::vector<Operation> operations;
		struct MainExit {};
		struct ConditionalJump
		{
			StackSlot condition;
			BasicBlock* nonZero = nullptr;
			BasicBlock* zero = nullptr;
		};
		struct Jump
		{
			BasicBlock* target = nullptr;
			bool backwards = false;
		};
		struct FunctionReturn { DFG::FunctionInfo* info = nullptr; };
		struct Terminated {};
		std::variant<MainExit, Jump, ConditionalJump, FunctionReturn, Terminated> exit = MainExit{};
	};

	struct FunctionInfo
	{
		std::shared_ptr<DebugData const> debugData;
		Scope::Function const* function = nullptr;
		BasicBlock* entry = nullptr;
		std::vector<VariableSlot> parameters;
		std::vector<VariableSlot> returnVariables;
	};

	BasicBlock* entry = nullptr;
	std::map<Scope::Function const*, FunctionInfo> functions;

	/// Container for blocks for explicit ownership.
	std::list<BasicBlock> blocks;
	/// Container for creates variables for explicit ownership.
	std::list<Scope::Variable> ghostVariables;
	/// Container for creates calls for explicit ownership.
	std::list<yul::FunctionCall> ghostCalls;

	BasicBlock& makeBlock()
	{
		return blocks.emplace_back(BasicBlock{});
	}
};

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
	struct ForLoopInfo { DFG::BasicBlock* afterLoop = nullptr; DFG::BasicBlock* post = nullptr; };
	std::optional<ForLoopInfo> m_forLoopInfo;
	DFG::BasicBlock* m_currentFunctionExit = nullptr;
};

}
