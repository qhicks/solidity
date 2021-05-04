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
#include <libyul/DataFlowGraph.h>
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

struct BlockGenerationInfo
{
	Stack entryLayout{};
	Stack exitLayout{};
};
struct OptimizedCodeTransformContext
{
	std::unique_ptr<DFG> dfg;
	std::map<DFG::BasicBlock const*, BlockGenerationInfo> blockInfos;
	struct OperationInfo
	{
		Stack entryStack;
		Stack exitStack;
	};
	std::map<DFG::Operation const*, OperationInfo> operationStacks;
};

class OptimizedCodeTransform
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
};

}
