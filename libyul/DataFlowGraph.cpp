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
#include <libsolutil/Visitor.h>
#include <libsolutil/Algorithms.h>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/concat.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/single.hpp>
#include <range/v3/view/take_last.hpp>
#include <range/v3/view/transform.hpp>

using namespace solidity;
using namespace solidity::yul;
using namespace std;

// TODO: if this is generally useful, promote to libsolutil - otherwise, remove and inline the ranges::views::transform.
template<auto>
struct MemberTransform;
template<typename T, typename C, T C::*ptr>
struct MemberTransform<ptr>
{
	static constexpr auto transform = ranges::views::transform([](auto&& _c) { return _c.*ptr; });
};
template<auto ptr>
static constexpr auto member_view = MemberTransform<ptr>::transform;

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

	// Determine which blocks are reachable from the entry.
	util::BreadthFirstSearch<DFG::BasicBlock*> reachabilityCheck{ranges::views::concat(
		ranges::views::single(result->entry),
		result->functions | ranges::views::values | member_view<&DFG::FunctionInfo::entry>
	) | ranges::to<list>};
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

	// Remove all entries from unreachable nodes from the graph.
	for (auto* node: reachabilityCheck.visited)
		cxx20::erase_if(node->entries, [&](DFG::BasicBlock const* entry) -> bool {
			// TODO: the const_cast is harmless, but weird.
			return !reachabilityCheck.visited.count(const_cast<DFG::BasicBlock*>(entry));
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

StackSlot DataFlowGraphBuilder::operator()(Expression const& _expression)
{
	return std::visit(*this, _expression);
}


DFG::Operation& DataFlowGraphBuilder::visitFunctionCall(FunctionCall const& _call)
{
	yulAssert(m_scope, "");
	yulAssert(m_currentBlock, "");

	if (BuiltinFunctionForEVM const* builtin = m_dialect.builtin(_call.functionName.name))
	{
		DFG::Operation& operation = m_currentBlock->operations.emplace_back(DFG::Operation{
			// input
			_call.arguments |
			ranges::views::enumerate |
			ranges::views::reverse |
			ranges::views::filter(util::mapTuple([&](size_t idx, auto const&) {
				return !builtin->literalArgument(idx).has_value();
			})) |
			ranges::views::values |
			ranges::views::transform(std::ref(*this)) |
			ranges::to<Stack>,
			// output
			ranges::views::iota(0u, builtin->returns.size()) | ranges::views::transform([&](size_t _i) {
				return TemporarySlot{_call, _i};
			}) | ranges::to<Stack>,
			// operation
			DFG::BuiltinCall{_call.debugData, builtin, &_call},
		});
		std::get<DFG::BuiltinCall>(operation.operation).arguments = operation.input.size();
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
		DFG::Operation& operation = m_currentBlock->operations.emplace_back(DFG::Operation{
			// input
			ranges::views::concat(
				ranges::views::single(FunctionCallReturnLabelSlot{_call}),
				_call.arguments | ranges::views::reverse | ranges::views::transform(std::ref(*this))
			) | ranges::to<Stack>,
			// output
			ranges::views::iota(0u, function->returns.size()) | ranges::views::transform([&](size_t _i) {
				return TemporarySlot{_call, _i};
			}) | ranges::to<Stack>,
			// operation
			DFG::FunctionCall{_call.debugData, function, &_call}
		});
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
	auto declaredVariables = _varDecl.variables | ranges::views::transform([&](TypedName const& _var) {
		return VariableSlot{&lookupVariable(_var.name), _var.debugData};
	}) | ranges::to<vector>;
	Stack input;
	if (_varDecl.value)
		input = std::visit(util::GenericVisitor{
			[&](FunctionCall const& _call) -> Stack {
				DFG::Operation& operation = visitFunctionCall(_call);
				yulAssert(declaredVariables.size() == operation.output.size(), "");
				return operation.output;
			},
			[&](auto const& _identifierOrLiteral) -> Stack{
				yulAssert(declaredVariables.size() == 1, "");
				return {(*this)(_identifierOrLiteral)};
			}
		}, *_varDecl.value);
	else
		input = ranges::views::iota(0u, _varDecl.variables.size()) | ranges::views::transform([&](size_t) {
			return LiteralSlot{0, _varDecl.debugData};
		}) | ranges::to<Stack>;
	m_currentBlock->operations.emplace_back(DFG::Operation{
		std::move(input),
		declaredVariables | ranges::to<Stack>,
		DFG::Assignment{_varDecl.debugData, declaredVariables}
	});
}
void DataFlowGraphBuilder::operator()(Assignment const& _assignment)
{
	vector<VariableSlot> assignedVariables = _assignment.variableNames | ranges::views::transform([&](Identifier const& _var) {
		return VariableSlot{&lookupVariable(_var.name), _var.debugData};
	}) | ranges::to<vector<VariableSlot>>;

	yulAssert(m_currentBlock, "");
	m_currentBlock->operations.emplace_back(DFG::Operation{
		// input
		std::visit(util::GenericVisitor{
			[&](FunctionCall const& _call) -> Stack {
				DFG::Operation& operation = visitFunctionCall(_call);
				yulAssert(assignedVariables.size() == operation.output.size(), "");
				return operation.output;
			},
			[&](auto const& _identifierOrLiteral) -> Stack {
				yulAssert(assignedVariables.size() == 1, "");
				return {(*this)(_identifierOrLiteral)};
			}
		}, *_assignment.value),
		// output
		assignedVariables | ranges::to<Stack>,
		// operation
		DFG::Assignment{_assignment.debugData, assignedVariables}
	});
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

	// TODO: Ideally this would be done on the expression label and for all functions that always revert,
	//       not only for builtins.
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
	YulString ghostVariableName("GHOST[" + to_string(ghostVariableId) + "]");
	auto& ghostVar = m_graph.ghostVariables.emplace_back(Scope::Variable{""_yulstring, ghostVariableName});

	// Artifically generate:
	// let <ghostVariable> := <switchExpression>
	VariableSlot ghostVarSlot{&ghostVar, debugDataOf(*_switch.expression)};
	m_currentBlock->operations.emplace_back(DFG::Operation{
		Stack{std::visit(*this, *_switch.expression)},
		Stack{ghostVarSlot},
		DFG::Assignment{_switch.debugData, {ghostVarSlot}}
	});

	BuiltinFunctionForEVM const* equalityBuiltin = m_dialect.equalityFunction({});
	yulAssert(equalityBuiltin, "");

	// Artificially generate:
	// eq(<literal>, <ghostVariable>)
	auto makeValueCompare = [&](Literal const& _value) {
		yul::FunctionCall const& ghostCall = m_graph.ghostCalls.emplace_back(yul::FunctionCall{
			_value.debugData,
			yul::Identifier{{}, "eq"_yulstring},
			{_value, Identifier{{}, ghostVariableName}}
		});
		DFG::Operation& operation = m_currentBlock->operations.emplace_back(DFG::Operation{
			Stack{ghostVarSlot, LiteralSlot{valueOfLiteral(_value), _value.debugData}},
			Stack{TemporarySlot{ghostCall, 0}},
			DFG::BuiltinCall{_switch.debugData, equalityBuiltin, &ghostCall, 2},
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
			jump(afterLoop);
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

	yulAssert(m_info.scopes.at(&_function.body), "");
	Scope* virtualFunctionScope = m_info.scopes.at(m_info.virtualBlocks.at(&_function).get()).get();
	yulAssert(virtualFunctionScope, "");

	DFG::FunctionInfo& info = m_graph.functions[&function] = DFG::FunctionInfo{
		_function.debugData,
		&function,
		&m_graph.makeBlock(),
		_function.parameters | ranges::views::transform([&](auto const& _param) {
			return VariableSlot{
				&std::get<Scope::Variable>(virtualFunctionScope->identifiers.at(_param.name)),
				_param.debugData
			};
		}) | ranges::to<vector>,
		_function.returnVariables | ranges::views::transform([&](auto const& _retVar) {
			return VariableSlot{
				&std::get<Scope::Variable>(virtualFunctionScope->identifiers.at(_retVar.name)),
				_retVar.debugData
			};
		}) | ranges::to<vector>
	};

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
		[&](Scope::Variable& _var) { var = &_var; },
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
