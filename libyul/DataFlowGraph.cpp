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
#include <libyul/DataFlowGraph.h>
#include <libyul/AST.h>
#include <libyul/Utilities.h>
#include <libyul/AsmPrinter.h>

#include <libsolutil/cxx20.h>
#include <libsolutil/Permutations.h>
#include <libsolutil/Visitor.h>
#include <libsolutil/Algorithms.h>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/take_last.hpp>
#include <range/v3/view/transform.hpp>
#include <boost/algorithm/string/join.hpp>

using namespace solidity;
using namespace solidity::yul;
using namespace std;

std::unique_ptr<DFG> DataFlowGraphBuilder::build(
	AsmAnalysisInfo& _analysisInfo,
	EVMDialect const& _dialect,
	Block const& _block
)
{
	auto result = std::make_unique<DFG>();
	result->entry = &result->makeBlock();

	DataFlowGraphBuilder builder(*result, _analysisInfo, _dialect);
	builder.m_currentBlock = result->entry;
	builder(_block);

	util::BreadthFirstSearch<DFG::BasicBlock*> reachabilityCheck{{result->entry}};
	for (auto const& functionInfo: result->functions | ranges::views::values)
		reachabilityCheck.verticesToTraverse.emplace_back(functionInfo.entry);

	reachabilityCheck.run([&](DFG::BasicBlock* _node, auto&& _addChild) {
		visit(util::GenericVisitor{
			[&](DFG::BasicBlock::Jump& _jump) {
				_addChild(_jump.target);
			},
			[&](DFG::BasicBlock::ConditionalJump& _jump) {
				_addChild(_jump.zero);
				_addChild(_jump.nonZero);
			},
			[](DFG::BasicBlock::FunctionReturn&) {},
			[](DFG::BasicBlock::Terminated&) {},
			[](DFG::BasicBlock::MainExit&) {}
		}, _node->exit);
	});

	for (auto* node: reachabilityCheck.visited)
		cxx20::erase_if(node->entries, [&](DFG::BasicBlock const* entry) -> bool {
			return !reachabilityCheck.visited.count(const_cast<DFG::BasicBlock*>(entry)); // TODO
		});

	return result;
}

DataFlowGraphBuilder::DataFlowGraphBuilder(
	DFG& _graph,
	AsmAnalysisInfo& _analysisInfo,
	EVMDialect const& _dialect
):
m_graph(_graph),
m_info(_analysisInfo),
m_dialect(_dialect)
{
}

StackSlot DataFlowGraphBuilder::operator()(Literal const& _literal)
{
	return LiteralSlot{valueOfLiteral(_literal), _literal.debugData};
}

StackSlot DataFlowGraphBuilder::operator()(Identifier const& _identifier)
{
	return VariableSlot{&lookupVariable(_identifier.name), _identifier.debugData};
}

DFG::Operation& DataFlowGraphBuilder::visitFunctionCall(FunctionCall const& _call)
{
	yulAssert(m_scope, "");
	yulAssert(m_currentBlock, "");

	if (BuiltinFunctionForEVM const* builtin = m_dialect.builtin(_call.functionName.name))
	{
		DFG::Operation& operation = m_currentBlock->operations.emplace_back(DFG::Operation{
			_call.arguments |
			ranges::views::enumerate |
			ranges::views::reverse |
			ranges::views::filter(util::mapTuple([&](size_t idx, auto const&) {
				return !builtin->literalArgument(idx).has_value();
			})) |
			ranges::views::transform(util::mapTuple([&](size_t, Expression const& _expression) {
				return std::visit(*this, _expression);
			})) | ranges::to<Stack>,
			{},
			DFG::BuiltinCall{_call.debugData, builtin, &_call},
		});
		std::get<DFG::BuiltinCall>(operation.operation).arguments = operation.input.size();
		for (size_t i: ranges::views::iota(0u, builtin->returns.size()))
			operation.output.emplace_back(TemporarySlot{&_call, i});
		return operation;
	}
	else
	{
		Scope::Function* function = nullptr;
		yulAssert(m_scope->lookup(_call.functionName.name, util::GenericVisitor{
			[](Scope::Variable&) { yulAssert(false, "Expected function name."); },
			[&](Scope::Function& _function) { function = &_function; }
		}), "Function name not found.");
		yulAssert(function, "");
		Stack inputs;
		inputs.emplace_back(ReturnLabelSlot{&_call});
		for (Expression const& _expression: _call.arguments | ranges::views::reverse)
			inputs.emplace_back(std::visit(*this, _expression));
		DFG::Operation& operation = m_currentBlock->operations.emplace_back(DFG::Operation{
			inputs,
			{},
			DFG::FunctionCall{_call.debugData, function, &_call}
		});
		for (size_t i: ranges::views::iota(0u, function->returns.size()))
			operation.output.emplace_back(TemporarySlot{&_call, i});
		return operation;
	}
}

StackSlot DataFlowGraphBuilder::operator()(FunctionCall const& _call)
{
	DFG::Operation& operation = visitFunctionCall(_call);
	yulAssert(operation.output.size() == 1, "");
	return operation.output.front();
}

void DataFlowGraphBuilder::operator()(VariableDeclaration const& _varDecl)
{
	yulAssert(m_currentBlock, "");
	std::vector<VariableSlot> variables = _varDecl.variables | ranges::views::transform([&](TypedName const& _var) {
		return VariableSlot{&lookupVariable(_var.name), _var.debugData};
	}) | ranges::to<vector<VariableSlot>>;
	if (_varDecl.value)
		std::visit(util::GenericVisitor{
			[&](FunctionCall const& _call) {
				DFG::Operation& operation = visitFunctionCall(_call);
				yulAssert(variables.size() == operation.output.size(), "");
				m_currentBlock->operations.emplace_back(DFG::Operation{
					operation.output,
					variables | ranges::to<Stack>,
					DFG::Assignment{_varDecl.debugData, variables}
				});
			},
			[&](auto const& _identifierOrLiteral) {
				yulAssert(variables.size() == 1, "");
				m_currentBlock->operations.emplace_back(DFG::Operation{
					{(*this)(_identifierOrLiteral)},
					variables | ranges::to<Stack>,
					DFG::Assignment{_varDecl.debugData, variables}
				});
			},
		}, *_varDecl.value);
	else
	{

		m_currentBlock->operations.emplace_back(DFG::Operation{
			ranges::views::iota(0u, _varDecl.variables.size()) | ranges::views::transform([&](size_t) {
				return LiteralSlot{0, _varDecl.debugData};
			}) | ranges::to<Stack>,
			variables | ranges::to<Stack>,
			DFG::Assignment{_varDecl.debugData, variables}
		});
	}
}
void DataFlowGraphBuilder::operator()(Assignment const& _assignment)
{
	yulAssert(m_currentBlock, "");
	vector<VariableSlot> variables = _assignment.variableNames | ranges::views::transform([&](Identifier const& _var) {
		return VariableSlot{&lookupVariable(_var.name), _var.debugData};
	}) | ranges::to<vector<VariableSlot>>;
	std::visit(util::GenericVisitor{
		[&](FunctionCall const& _call) {
			DFG::Operation& operation = visitFunctionCall(_call);
			yulAssert(variables.size() == operation.output.size(), "");
			m_currentBlock->operations.emplace_back(DFG::Operation{
				operation.output,
				variables | ranges::to<Stack>,
				DFG::Assignment{_assignment.debugData, variables}
			});
		},
		[&](auto const& _identifierOrLiteral) {
			yulAssert(variables.size() == 1, "");
			m_currentBlock->operations.emplace_back(DFG::Operation{
				{(*this)(_identifierOrLiteral)},
				variables | ranges::to<Stack>,
				DFG::Assignment{_assignment.debugData, variables}
			});
		},
	}, *_assignment.value);
}
void DataFlowGraphBuilder::operator()(ExpressionStatement const& _exprStmt)
{
	yulAssert(m_currentBlock, "");
	std::visit(util::GenericVisitor{
		[&](FunctionCall const& _call) {
			DFG::Operation& operation = visitFunctionCall(_call);
			yulAssert(operation.output.empty(), "");
		},
		[&](auto const&) { yulAssert(false, ""); }
	}, _exprStmt.expression);

	if (auto const* funCall = get_if<FunctionCall>(&_exprStmt.expression))
		if (BuiltinFunctionForEVM const* builtin = m_dialect.builtin(funCall->functionName.name))
			if (builtin->controlFlowSideEffects.terminates)
			{
				m_currentBlock->exit = DFG::BasicBlock::Terminated{};
				m_currentBlock = &m_graph.makeBlock();
			}
}

void DataFlowGraphBuilder::operator()(Block const& _block)
{
	ScopedSaveAndRestore saveScope(m_scope, m_info.scopes.at(&_block).get());
	for (auto const& statement: _block.statements)
		std::visit(*this, statement);
}

std::pair<DFG::BasicBlock*, DFG::BasicBlock*> DataFlowGraphBuilder::makeConditionalJump(StackSlot _condition)
{
	DFG::BasicBlock& nonZero = m_graph.makeBlock();
	DFG::BasicBlock& zero = m_graph.makeBlock();
	makeConditionalJump(move(_condition), nonZero, zero);
	return {&nonZero, &zero};
}
void DataFlowGraphBuilder::makeConditionalJump(StackSlot _condition, DFG::BasicBlock& _nonZero, DFG::BasicBlock& _zero)
{
	yulAssert(m_currentBlock, "");
	m_currentBlock->exit = DFG::BasicBlock::ConditionalJump{
		move(_condition),
		&_nonZero,
		&_zero
	};
	_nonZero.entries.emplace_back(m_currentBlock);
	_zero.entries.emplace_back(m_currentBlock);
	m_currentBlock = nullptr;
}
void DataFlowGraphBuilder::jump(DFG::BasicBlock& _target, bool backwards)
{
	yulAssert(m_currentBlock, "");
	m_currentBlock->exit = DFG::BasicBlock::Jump{&_target, backwards};
	_target.entries.emplace_back(m_currentBlock);
	m_currentBlock = &_target;
}
void DataFlowGraphBuilder::operator()(If const& _if)
{
	auto&& [ifBranch, afterIf] = makeConditionalJump(std::visit(*this, *_if.condition));
	m_currentBlock = ifBranch;
	(*this)(_if.body);
	jump(*afterIf);
}

void DataFlowGraphBuilder::operator()(Switch const& _switch)
{
	yulAssert(m_currentBlock, "");
	auto ghostVariableId = m_graph.ghostVariables.size();
	Scope::Variable& ghostVar = m_graph.ghostVariables.emplace_back(Scope::Variable{""_yulstring, YulString("GHOST[" + to_string(ghostVariableId) + "]")});

	m_currentBlock->operations.emplace_back(DFG::Operation{
		Stack{std::visit(*this, *_switch.expression)},
		Stack{VariableSlot{&ghostVar}},
		DFG::Assignment{_switch.debugData, {VariableSlot{&ghostVar}}}
	});

	auto makeValueCompare = [&](Literal const& _value) {

		// TODO: debug data
		yul::FunctionCall const& ghostCall = m_graph.ghostCalls.emplace_back(yul::FunctionCall{
			{},
			yul::Identifier{{}, "eq"_yulstring},
			{_value, Identifier{{}, YulString("GHOST[" + to_string(ghostVariableId) + "]")}}
		});

		BuiltinFunctionForEVM const* builtin = m_dialect.equalityFunction({});
		yulAssert(builtin, "");

		DFG::Operation& operation = m_currentBlock->operations.emplace_back(DFG::Operation{
			Stack{VariableSlot{&ghostVar}, LiteralSlot{valueOfLiteral(_value), {}}},
			Stack{TemporarySlot{&ghostCall, 0}},
			DFG::BuiltinCall{_switch.debugData, builtin, &ghostCall, 2},
		});

		return operation.output.front();
	};
	DFG::BasicBlock& afterSwitch = m_graph.makeBlock();
	for (auto const& switchCase: _switch.cases | ranges::views::drop_last(1))
	{
		yulAssert(switchCase.value, "");
		auto&& [caseBranch, elseBranch] = makeConditionalJump(makeValueCompare(*switchCase.value));
		m_currentBlock = caseBranch;
		(*this)(switchCase.body);
		jump(afterSwitch);
		m_currentBlock = elseBranch;
	}
	Case const& switchCase = _switch.cases.back();
	if (switchCase.value)
	{
		DFG::BasicBlock& caseBranch = m_graph.makeBlock();
		makeConditionalJump(makeValueCompare(*switchCase.value), caseBranch, afterSwitch);
		m_currentBlock = &caseBranch;
		(*this)(switchCase.body);
	}
	else
		(*this)(switchCase.body);
	jump(afterSwitch);
}

void DataFlowGraphBuilder::operator()(ForLoop const& _loop)
{
	ScopedSaveAndRestore scopeRestore(m_scope, m_info.scopes.at(&_loop.pre).get());
	(*this)(_loop.pre);

	std::optional<bool> constantCondition;
	if (auto const* literalCondition = get_if<yul::Literal>(_loop.condition.get()))
	{
		if (valueOfLiteral(*literalCondition) == 0)
			constantCondition = false;
		else
			constantCondition = true;
	}

	DFG::BasicBlock& loopCondition = m_graph.makeBlock();
	DFG::BasicBlock& loopBody = m_graph.makeBlock();
	DFG::BasicBlock& post = m_graph.makeBlock();
	DFG::BasicBlock& afterLoop = m_graph.makeBlock();

	ScopedSaveAndRestore scopedSaveAndRestore(m_forLoopInfo, ForLoopInfo{&afterLoop, &post});

	if (constantCondition.has_value())
	{
		if (*constantCondition)
		{
			jump(loopBody);
			(*this)(_loop.body);
			jump(post);
			(*this)(_loop.post);
			jump(loopBody, true);
		}
		else
		{
			jump(afterLoop);
		}
	}
	else
	{
		jump(loopCondition);
		makeConditionalJump(std::visit(*this, *_loop.condition), loopBody, afterLoop);
		m_currentBlock = &loopBody;
		(*this)(_loop.body);
		jump(post);
		(*this)(_loop.post);
		jump(loopCondition, true);
	}

	m_currentBlock = &afterLoop;
}

void DataFlowGraphBuilder::operator()(Break const&)
{
	yulAssert(m_forLoopInfo.has_value(), "");
	jump(*m_forLoopInfo->afterLoop);
	m_currentBlock = &m_graph.makeBlock();
}

void DataFlowGraphBuilder::operator()(Continue const&)
{
	yulAssert(m_forLoopInfo.has_value(), "");
	jump(*m_forLoopInfo->post);
	m_currentBlock = &m_graph.makeBlock();
}


void DataFlowGraphBuilder::operator()(Leave const&)
{
	yulAssert(m_currentFunctionExit, "");
	jump(*m_currentFunctionExit);
	m_currentBlock = &m_graph.makeBlock();
}

void DataFlowGraphBuilder::operator()(FunctionDefinition const& _function)
{
	yulAssert(m_scope, "");
	yulAssert(m_scope->identifiers.count(_function.name), "");
	Scope::Function& function = std::get<Scope::Function>(m_scope->identifiers.at(_function.name));

	DFG::FunctionInfo& info = m_graph.functions[&function] = DFG::FunctionInfo{
		_function.debugData,
		&function,
		&m_graph.makeBlock(),
		{},
		{}
	};

	yulAssert(m_info.scopes.at(&_function.body), "");
	Scope* virtualFunctionScope = m_info.scopes.at(m_info.virtualBlocks.at(&_function).get()).get();
	yulAssert(virtualFunctionScope, "");
	for (auto const& v: _function.parameters)
		info.parameters.emplace_back(VariableSlot{
			&std::get<Scope::Variable>(virtualFunctionScope->identifiers.at(v.name)),
			v.debugData
		});
	for (auto const& v: _function.returnVariables)
		info.returnVariables.emplace_back(VariableSlot{
			&std::get<Scope::Variable>(virtualFunctionScope->identifiers.at(v.name)),
			v.debugData
		});

	DataFlowGraphBuilder builder{m_graph, m_info, m_dialect};
	builder.m_currentFunctionExit = &m_graph.makeBlock();
	builder.m_currentFunctionExit->exit = DFG::BasicBlock::FunctionReturn{&info};
	builder.m_currentBlock = info.entry;
	builder(_function.body);
	builder.jump(*builder.m_currentFunctionExit);
}

Scope::Variable const& DataFlowGraphBuilder::lookupVariable(YulString _name) const
{
	yulAssert(m_scope, "");
	Scope::Variable *var = nullptr;
	if (m_scope->lookup(_name, util::GenericVisitor{
		[&](Scope::Variable& _var)
		{
			var = &_var;
		},
		[](Scope::Function&)
		{
			yulAssert(false, "Function not removed during desugaring.");
		}
	}))
	{
		yulAssert(var, "");
		return *var;
	};
	yulAssert(false, "External identifier access unimplemented.");

}
