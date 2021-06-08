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
 * Code generator for translating Yul / inline assembly to EVM.
 */

#pragma once

#include <libyul/backends/evm/EVMAssembly.h>

#include <libyul/backends/evm/EVMDialect.h>
#include <libyul/backends/evm/DataFlowGraph.h>
#include <libyul/AST.h>
#include <libyul/Scope.h>

#include <optional>
#include <stack>

namespace solidity::langutil
{
class ErrorReporter;
}

namespace solidity::yul
{
struct AsmAnalysisInfo;
struct StackLayout;

class OptimizedEVMCodeTransform
{
public:
	static void run(
		AbstractAssembly& _assembly,
		AsmAnalysisInfo& _analysisInfo,
		Block const& _block,
		EVMDialect const& _dialect,
		BuiltinContext& _builtinContext,
		ExternalIdentifierAccess const& _identifierAccess = ExternalIdentifierAccess(),
		bool _useNamedLabelsForFunctions = false
	);
private:
	OptimizedEVMCodeTransform(
		AbstractAssembly& _assembly,
		BuiltinContext& _builtinContext,
		bool _useNamedLabelsForFunctions,
		DFG const& _dfg,
		StackLayout const& _stackLayout
	);

	AbstractAssembly::LabelID getFunctionLabel(Scope::Function const& _function);
	void validateSlot(StackSlot const& _slot, Expression const& _expression);

	bool tryCreateStackLayout(Stack _targetStack);
	void compressStack();
	void createStackLayout(Stack _targetStack);

	void operator()(DFG::BasicBlock const& _block);
	void operator()(DFG::FunctionInfo const& _functionInfo);
public:
	void operator()(DFG::FunctionCall const& _call);
	void operator()(DFG::BuiltinCall const& _call);
	void operator()(DFG::Assignment const& _assignment);

private:
	static void assertLayoutCompatibility(Stack const& _currentStack, Stack const& _desiredStack);

	AbstractAssembly& m_assembly;
	BuiltinContext& m_builtinContext;
	bool m_useNamedLabelsForFunctions = true;
	DFG const& m_dfg;
	StackLayout const& m_stackLayout;
	Stack m_stack;
	std::map<yul::FunctionCall const*, AbstractAssembly::LabelID> m_returnLabels;
	std::map<DFG::BasicBlock const*, AbstractAssembly::LabelID> m_blockLabels;
	std::map<DFG::FunctionInfo const*, AbstractAssembly::LabelID> m_functionLabels;
	std::set<DFG::BasicBlock const*> m_generated;
	DFG::FunctionInfo const* m_currentFunctionInfo = nullptr;
};

}
