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
#include <libyul/backends/evm/OptimizedEVMCodeTransform.h>

#include <libyul/backends/evm/StackHelpers.h>
#include <libyul/backends/evm/StackLayoutGenerator.h>

#include <libyul/DataFlowGraph.h>
#include <libyul/Utilities.h>

#include <libsolutil/Permutations.h>
#include <libsolutil/Visitor.h>
#include <libsolutil/cxx20.h>

#include <range/v3/view/drop.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/take.hpp>
#include <range/v3/view/take_last.hpp>

using namespace solidity;
using namespace solidity::yul;
using namespace std;

#if 1
#define DEBUG(x) x
#else
#define DEBUG(x) (void)0;
#endif

class CodeGenerator
{
public:
	static void run(
		AbstractAssembly& _assembly,
		BuiltinContext& _builtinContext,
		bool _useNamedLabelsForFunctions,
		OptimizedCodeTransformContext const& _info,
		DFG::BasicBlock const& _entry
	)
	{
		CodeGenerator generator(_assembly, _builtinContext, _useNamedLabelsForFunctions,  _info);
		generator(_entry);
		generator.generateStaged();

	}
private:
	CodeGenerator(
		AbstractAssembly& _assembly,
		BuiltinContext& _builtinContext,
		bool _useNamedLabelsForFunctions,
		OptimizedCodeTransformContext const& _info
	):
	m_assembly(_assembly),
	m_builtinContext(_builtinContext),
	m_useNamedLabelsForFunctions(_useNamedLabelsForFunctions),
	m_info(_info)
	{
	}
public:

	AbstractAssembly::LabelID getFunctionLabel(Scope::Function const& _function);

	void operator()(DFG::FunctionInfo const& _functionInfo);
	void validateSlot(StackSlot const& _slot, Expression const& _expression);

	void operator()(DFG::FunctionCall const& _call);
	void operator()(DFG::BuiltinCall const& _call);
	void operator()(DFG::Assignment const& _assignment);
	void operator()(DFG::BasicBlock const& _block);

	bool tryCreateStackLayout(Stack _targetStack);
	void compressStack();
	void createStackLayout(Stack _targetStack);
private:

	void generateStaged();

	AbstractAssembly& m_assembly;
	BuiltinContext& m_builtinContext;
	bool m_useNamedLabelsForFunctions = true;
	OptimizedCodeTransformContext const& m_info;
	Stack m_stack;
	std::map<yul::FunctionCall const*, AbstractAssembly::LabelID> m_returnLabels;
	std::map<DFG::BasicBlock const*, AbstractAssembly::LabelID> m_blockLabels;
	std::map<DFG::FunctionInfo const*, AbstractAssembly::LabelID> m_functionLabels;
	std::set<DFG::BasicBlock const*> m_generated;
	std::deque<DFG::BasicBlock const*> m_stagedBlocks;
	std::list<DFG::FunctionInfo const*> m_stagedFunctions;
	std::set<DFG::FunctionInfo const*> m_generatedFunctions;
	DFG::FunctionInfo const* m_currentFunctionInfo = nullptr;
};


AbstractAssembly::LabelID CodeGenerator::getFunctionLabel(Scope::Function const& _function)
{
	ScopedSaveAndRestore restoreStack(m_stack, {});
	DFG::FunctionInfo const& functionInfo = m_info.dfg->functions.at(&_function);
	if (!m_functionLabels.count(&functionInfo))
	{
		m_functionLabels[&functionInfo] = m_useNamedLabelsForFunctions ?
			m_assembly.namedLabel(
				functionInfo.function->name.str(),
				functionInfo.function->arguments.size(),
				functionInfo.function->returns.size(),
				{}
			) : m_assembly.newLabelId();

		m_stagedFunctions.emplace_back(&functionInfo);
	}
	return m_functionLabels[&functionInfo];
}

void CodeGenerator::operator()(DFG::FunctionInfo const& _functionInfo)
{
	yulAssert(!m_currentFunctionInfo, "");
	m_currentFunctionInfo = &_functionInfo;

	BlockGenerationInfo const& info = m_info.blockInfos.at(_functionInfo.entry);

	DEBUG(cout << std::endl;)
	DEBUG(cout << "F: start of function " << _functionInfo.function->name.str() << std::endl;)
	m_stack.clear();
	m_stack.emplace_back(ReturnLabelSlot{});
	for (auto const& param: _functionInfo.parameters | ranges::views::reverse)
		m_stack.emplace_back(param);
	m_assembly.setStackHeight(static_cast<int>(m_stack.size()));
	m_assembly.setSourceLocation(locationOf(_functionInfo));
	yulAssert(m_functionLabels.count(&_functionInfo), "");

	m_assembly.appendLabel(m_functionLabels.at(&_functionInfo));
	createStackLayout(info.entryLayout);

	(*this)(*_functionInfo.entry);

	m_currentFunctionInfo = nullptr;
}

void CodeGenerator::validateSlot(StackSlot const& _slot, Expression const& _expression)
{
	std::visit(util::GenericVisitor{
		[&](yul::Literal const& _literal) {
			auto* literalSlot = get_if<LiteralSlot>(&_slot);
			yulAssert(literalSlot && valueOfLiteral(_literal) == literalSlot->value, "");
		},
		[&](yul::Identifier const& _identifier) {
			auto* variableSlot = get_if<VariableSlot>(&_slot);
			yulAssert(variableSlot && variableSlot->variable->name == _identifier.name, "");
		},
		[&](yul::FunctionCall const& _call) {
			auto* temporarySlot = get_if<TemporarySlot>(&_slot);
			yulAssert(temporarySlot && temporarySlot->call == &_call, "");
		}
	}, _expression);
}

void CodeGenerator::operator()(DFG::FunctionCall const& _call)
{
	// Assert that we got a correct stack for the call.
	for (auto&& [arg, slot]: ranges::zip_view(
		_call.functionCall->arguments | ranges::views::reverse,
		m_stack | ranges::views::take_last(_call.functionCall->arguments.size())
	))
		validateSlot(slot, arg);

	auto entryLabel = getFunctionLabel(*_call.function);
	DEBUG(cout << "F: function call " << _call.functionCall->functionName.name.str() << " pre: " << stackToString(m_stack) << std::endl;)
	m_assembly.setSourceLocation(locationOf(_call));
	m_assembly.appendJumpTo(
		entryLabel,
		static_cast<int>(_call.function->returns.size() - _call.function->arguments.size()) - 1,
		AbstractAssembly::JumpType::IntoFunction
	);
	m_assembly.appendLabel(m_returnLabels.at(_call.functionCall));
	for (size_t i = 0; i < _call.function->arguments.size() + 1; ++i)
		m_stack.pop_back();
	for (size_t i = 0; i < _call.function->returns.size(); ++i)
		m_stack.emplace_back(TemporarySlot{_call.functionCall, i});
	DEBUG(cout << "F: function call " << _call.functionCall->functionName.name.str() << " post: " << stackToString(m_stack) << std::endl;)
}

void CodeGenerator::operator()(DFG::BuiltinCall const& _call)
{
	// Assert that we got a correct stack for the call.
	for (auto&& [arg, slot]: ranges::zip_view(
		_call.functionCall->arguments | ranges::views::enumerate |
		ranges::views::filter(util::mapTuple([&](size_t idx, auto&) -> bool { return !_call.builtin->literalArgument(idx); })) |
		ranges::views::reverse | ranges::views::values,
		m_stack | ranges::views::take_last(_call.arguments)
	))
		validateSlot(slot, arg);

	DEBUG(cout << "F: builtin call " << _call.functionCall->functionName.name.str() << " pre: " << stackToString(m_stack) << std::endl;)
	m_assembly.setSourceLocation(locationOf(_call));
	_call.builtin->generateCode(*_call.functionCall, m_assembly, m_builtinContext, [](auto&&){});
	for (size_t i = 0; i < _call.arguments; ++i)
		m_stack.pop_back();
	for (size_t i = 0; i < _call.builtin->returns.size(); ++i)
		m_stack.emplace_back(TemporarySlot{_call.functionCall, i});
	DEBUG(cout << "F: builtin call " << _call.functionCall->functionName.name.str() << " post: " << stackToString(m_stack) << std::endl;)
}

void CodeGenerator::operator()(DFG::Assignment const& _assignment)
{
	m_assembly.setSourceLocation(locationOf(_assignment));
	DEBUG(cout << "F: assign (";)
	for (auto var: _assignment.variables)
		DEBUG(cout << var.variable->name.str() << " ";)
	DEBUG(cout << ") pre: " << stackToString(m_stack) << std::endl;)

	for (auto& currentSlot: m_stack)
		if (VariableSlot const* varSlot = get_if<VariableSlot>(&currentSlot))
			if (util::findOffset(_assignment.variables, *varSlot))
				currentSlot = JunkSlot{};

	for (auto&& [currentSlot, varSlot]: ranges::zip_view(m_stack | ranges::views::take_last(_assignment.variables.size()), _assignment.variables))
		currentSlot = varSlot;

	DEBUG(cout << "F: assign (";)
	for (auto var: _assignment.variables)
		DEBUG(cout << var.variable->name.str() << " ";)
	DEBUG(cout << ") post: " << stackToString(m_stack) << std::endl;)
}

void CodeGenerator::operator()(DFG::BasicBlock const& _block)
{
	if (m_generated.count(&_block))
		return;
	m_generated.insert(&_block);

	BlockGenerationInfo const& info = m_info.blockInfos.at(&_block);

	if (auto label = util::valueOrNullptr(m_blockLabels, &_block))
		m_assembly.appendLabel(*label);

	{
		auto label = util::valueOrNullptr(m_blockLabels, &_block);
		(void)label;
		DEBUG(cout << "F: GENERATING: " << &_block << " (label: " << (label ? std::to_string(*label) : "NONE") << ")" << std::endl;)
	}

	yulAssert(m_stack == info.entryLayout, "");

	DEBUG(cout << "F: CREATING ENTRY LAYOUT " << stackToString(info.entryLayout) << " FROM " << stackToString(m_stack) << std::endl;)
	createStackLayout(info.entryLayout);

	for (auto const& operation: _block.operations)
	{
		OptimizedCodeTransformContext::OperationInfo const& operationInfo = m_info.operationStacks.at(&operation);
		createStackLayout(operationInfo.entryStack);
		std::visit(*this, operation.operation);
		// TODO: is this actually necessary each time? Last time is probably enough, if at all needed.
		// createStackLayout(operationInfo.exitStack);
	}
	createStackLayout(info.exitLayout);

	DEBUG(cout << std::endl << std::endl;)
	DEBUG(cout << "F: EXIT LAYOUT (" << &_block << "): " << stackToString(info.exitLayout) << " == " << stackToString(m_stack) << std::endl;)
	// TODO: conditions!
	//		yulAssert(info.exitLayout == m_stack, "");


	std::visit(util::GenericVisitor{
		[&](DFG::BasicBlock::MainExit const&)
		{
			DEBUG(cout << "F: MAIN EXIT" << std::endl;)
			m_assembly.appendInstruction(evmasm::Instruction::STOP);
		},
		[&](DFG::BasicBlock::Jump const& _jump)
		{
			DEBUG(cout << "F: JUMP EXIT TO: " << _jump.target << std::endl;)

			BlockGenerationInfo const& targetInfo = m_info.blockInfos.at(_jump.target);
			DEBUG(cout << "F: CURRENT " << stackToString(m_stack) << " => " << stackToString(targetInfo.entryLayout) << std::endl;)
			createStackLayout(targetInfo.entryLayout);
			/*
			 * Actually this should be done, but since the stack shuffling doesn't allow anything for Junk slots, but explicitly "creates"
			 * them this actually *costs* currently:
			 * Similarly for the conditional case.
			 * Probably even better to do it when assigning the entry layouts.
			 */
			/*
			createStackLayout(targetInfo.entryLayout | ranges::views::transform([&](StackSlot const& _slot) -> StackSlot {
				if (!_jump.target->operations.empty())
				{
					OptimizedCodeTransformContext::OperationInfo const& operationInfo = m_info.operationStacks.at(&_jump.target->operations.front());
					if (!util::findOffset(operationInfo.entryStack, _slot))
						return JunkSlot{};
				}
				return _slot;
			}) | ranges::to<Stack>);
			m_stack = targetInfo.entryLayout;
			 */

			if (!m_blockLabels.count(_jump.target) && _jump.target->entries.size() == 1)
				(*this)(*_jump.target);
			else
			{
				if (!m_blockLabels.count(_jump.target))
					m_blockLabels[_jump.target] = m_assembly.newLabelId();

				yulAssert(m_stack == m_info.blockInfos.at(_jump.target).entryLayout, "");
				m_assembly.appendJumpTo(m_blockLabels[_jump.target]);
				if (!m_generated.count(_jump.target))
					m_stagedBlocks.emplace_back(_jump.target);
			}
		},
		[&](DFG::BasicBlock::ConditionalJump const& _conditionalJump)
		{
			DEBUG(cout << "F: CONDITIONAL JUMP EXIT TO: " << _conditionalJump.nonZero << " / " << _conditionalJump.zero << std::endl;)
			DEBUG(cout << "F: CURRENT EXIT LAYOUT: " << stackToString(info.exitLayout) << std::endl;)
			BlockGenerationInfo const& nonZeroInfo = m_info.blockInfos.at(_conditionalJump.nonZero);
			(void)nonZeroInfo;
			BlockGenerationInfo const& zeroInfo = m_info.blockInfos.at(_conditionalJump.zero);
			(void)zeroInfo;
			DEBUG(cout << "F: non-zero entry layout: " << stackToString(nonZeroInfo.entryLayout) << std::endl;)
			DEBUG(cout << "F: zero entry layout: " << stackToString(zeroInfo.entryLayout) << std::endl;)

			for (auto const* nonZeroEntry: _conditionalJump.nonZero->entries)
			{
				BlockGenerationInfo const& entryInfo = m_info.blockInfos.at(nonZeroEntry);
				(void)entryInfo;
				DEBUG(cout << "  F: non-zero entry exit: " << stackToString(entryInfo.exitLayout) << std::endl;)
			}
			for (auto const* zeroEntry: _conditionalJump.zero->entries)
			{
				BlockGenerationInfo const& entryInfo = m_info.blockInfos.at(zeroEntry);
				(void)entryInfo;
				DEBUG(cout << "  F: zero entry exit: " << stackToString(entryInfo.exitLayout) << std::endl;)
			}
/*
 * TODO!
				yulAssert(nonZeroInfo.entryLayout == zeroInfo.entryLayout, "");
				yulAssert((m_stack | ranges::views::drop_last(1) | ranges::to<Stack>) == nonZeroInfo.entryLayout, "");
*/
			if (!m_blockLabels.count(_conditionalJump.nonZero))
				m_blockLabels[_conditionalJump.nonZero] = m_assembly.newLabelId();
			m_assembly.appendJumpToIf(m_blockLabels[_conditionalJump.nonZero]);
			m_stack.pop_back();
			// TODO: assert this?
			yulAssert(m_stack == m_info.blockInfos.at(_conditionalJump.nonZero).entryLayout, "");
			yulAssert(m_stack == m_info.blockInfos.at(_conditionalJump.zero).entryLayout, "");

			if (!m_generated.count(_conditionalJump.nonZero))
				m_stagedBlocks.emplace_back(_conditionalJump.nonZero);

			if (!m_blockLabels.count(_conditionalJump.zero))
				m_blockLabels[_conditionalJump.zero] = m_assembly.newLabelId();
			if (m_generated.count(_conditionalJump.zero))
				m_assembly.appendJumpTo(m_blockLabels[_conditionalJump.zero]);
			else
				(*this)(*_conditionalJump.zero);
		},
		[&](DFG::BasicBlock::FunctionReturn const& _functionReturn)
		{
			yulAssert(m_currentFunctionInfo == _functionReturn.info, "");
			DEBUG(cout << "F: Function return exit: " << _functionReturn.info->function->name.str() << std::endl;)

			yulAssert(m_currentFunctionInfo, "");
			Stack exitStack = m_currentFunctionInfo->returnVariables | ranges::views::transform([](auto const& _varSlot){
				return StackSlot{_varSlot};
			}) | ranges::to<Stack>;
			exitStack.emplace_back(ReturnLabelSlot{});

			DEBUG(cout << "Return from function " << m_currentFunctionInfo->function->name.str() << std::endl;)
			DEBUG(cout << "EXIT STACK: " << stackToString(exitStack) << std::endl;)
			createStackLayout(exitStack);
			m_assembly.setSourceLocation(locationOf(*m_currentFunctionInfo));
			m_assembly.appendJump(0, AbstractAssembly::JumpType::OutOfFunction); // TODO: stack height diff.
			m_assembly.setStackHeight(0);
			m_stack.clear();

		},
		[&](DFG::BasicBlock::Terminated const&)
		{
			DEBUG(cout << "F: TERMINATED" << std::endl;)
		}
	}, _block.exit);
}

bool CodeGenerator::tryCreateStackLayout(Stack _targetStack)
{
	Stack commonPrefix;
	for (auto&& [slot1, slot2]: ranges::zip_view(m_stack, _targetStack))
	{
		if (!(slot1 == slot2))
			break;
		commonPrefix.emplace_back(slot1);
	}
	Stack temporaryStack = m_stack | ranges::views::drop(commonPrefix.size()) | ranges::to<Stack>;

	bool good = true;
	::createStackLayout(temporaryStack, _targetStack  | ranges::views::drop(commonPrefix.size()) | ranges::to<Stack>, [&](unsigned _i) {
		if (_i > 16)
			good = false;
	}, [&](unsigned _i) {
		if (_i > 16)
			good = false;
	}, [&](StackSlot const& _slot) {
		Stack currentFullStack = commonPrefix;
		for (auto slot: temporaryStack)
			currentFullStack.emplace_back(slot);
		if (auto depth = util::findOffset(currentFullStack | ranges::views::reverse, _slot))
		{
			if (*depth + 1 > 16)
				good = false;
			return;
		}
	}, [&]() {});
	return good;
}

void CodeGenerator::compressStack()
{
	DEBUG(std::cout << "COMPRESS STACK" << std::endl;)
	static constexpr auto canBeRegenerated = [](StackSlot const& _slot) -> bool {
		if (auto* returnSlot = get_if<ReturnLabelSlot>(&_slot))
			if (returnSlot->call)
				return true;
		if (holds_alternative<LiteralSlot>(_slot))
			return true;
		return false;
	};
	while (!m_stack.empty())
	{
		auto top = m_stack.back();
		if (canBeRegenerated(top))
		{
			m_assembly.appendInstruction(evmasm::Instruction::POP);
			m_stack.pop_back();
			continue;
		}
		if (auto offset = util::findOffset(m_stack, top))
			if (*offset < m_stack.size() - 1)
			{
				m_assembly.appendInstruction(evmasm::Instruction::POP);
				m_stack.pop_back();
				continue;
			}

		size_t topSize = m_stack.size() > 16 ? 16 : m_stack.size();
		for (auto&& [offset, slot]: (m_stack | ranges::views::take_last(topSize)) | ranges::views::enumerate)
		{
			if (offset == topSize - 1)
				return;
			if (canBeRegenerated(slot))
			{
				std::swap(m_stack.back(), m_stack.at(m_stack.size() - topSize + offset));
				m_assembly.appendInstruction(evmasm::swapInstruction(static_cast<unsigned>(topSize - 1 - offset)));
				m_stack.pop_back();
				m_assembly.appendInstruction(evmasm::Instruction::POP);
				break;
			}
		}
	}
}

void CodeGenerator::createStackLayout(Stack _targetStack)
{
	DEBUG(cout << "F: CREATE " << stackToString(_targetStack) << " FROM " << stackToString(m_stack) << std::endl;)

	Stack commonPrefix;
	for (auto&& [slot1, slot2]: ranges::zip_view(m_stack, _targetStack))
	{
		if (!(slot1 == slot2))
			break;
		commonPrefix.emplace_back(slot1);
	}

	Stack temporaryStack = m_stack | ranges::views::drop(commonPrefix.size()) | ranges::to<Stack>;

	if (!tryCreateStackLayout(_targetStack))
	{
		// TODO: check if we can do better.
		// Maybe switching to a general "fix everything deep first" algorithm.
		std::map<unsigned, StackSlot> slotsByDepth;
		for (auto slot: _targetStack | ranges::views::take_last(_targetStack.size() - commonPrefix.size()))
		{
			auto offset = util::findOffset(m_stack | ranges::views::reverse | ranges::to<Stack>, slot);
			if (offset)
				slotsByDepth.insert(std::make_pair(*offset, slot));
		}
		for (auto slot: slotsByDepth | ranges::views::reverse | ranges::views::values)
		{
			if (!util::findOffset(temporaryStack, slot))
			{
				auto offset = util::findOffset(m_stack | ranges::views::reverse | ranges::to<Stack>, slot);
				m_stack.emplace_back(slot);
				DEBUG(
					if (*offset + 1 > 16)
						std::cout << "Cannot reach slot: " << stackSlotToString(slot) << std::endl;
				)
				m_assembly.appendInstruction(evmasm::dupInstruction(static_cast<unsigned>(*offset + 1)));
			}
		}

		temporaryStack = m_stack | ranges::views::drop(commonPrefix.size()) | ranges::to<Stack>;
	}


	DEBUG(cout << "F: CREATE " << stackToString(_targetStack) << " FROM " << stackToString(m_stack) << std::endl;)
	DEBUG(
		if (!commonPrefix.empty())
			cout << "   (USE " << stackToString(_targetStack | ranges::views::drop(commonPrefix.size()) | ranges::to<Stack>) << " FROM " << stackToString(temporaryStack) << std::endl;
	)
	::createStackLayout(temporaryStack, _targetStack  | ranges::views::drop(commonPrefix.size()) | ranges::to<Stack>, [&](unsigned _i) {
		m_assembly.appendInstruction(evmasm::swapInstruction(_i));
	}, [&](unsigned _i) {
		m_assembly.appendInstruction(evmasm::dupInstruction(_i));
	}, [&](StackSlot const& _slot) {
		Stack currentFullStack = commonPrefix;
		for (auto slot: temporaryStack)
			currentFullStack.emplace_back(slot);
		if (auto depth = util::findOffset(currentFullStack | ranges::views::reverse, _slot))
		{
			m_assembly.appendInstruction(evmasm::dupInstruction(static_cast<unsigned>(*depth + 1)));
			return;
		}
		std::visit(util::GenericVisitor{
			[&](LiteralSlot const& _literal)
			{
				m_assembly.setSourceLocation(locationOf(_literal));
				m_assembly.appendConstant(_literal.value);
			},
			[&](ReturnLabelSlot const& _returnLabel)
			{
				yulAssert(_returnLabel.call, "Cannot produce function return label.");
				// TODO: maybe actually use IDs to index into returnLabels.
				if (!m_returnLabels.count(_returnLabel.call))
					m_returnLabels[_returnLabel.call] = m_assembly.newLabelId();
				m_assembly.appendLabelReference(m_returnLabels.at(_returnLabel.call));
			},
			[&](VariableSlot const& _variable)
			{
				if (m_currentFunctionInfo)
					if (util::contains(m_currentFunctionInfo->returnVariables, _variable))
					{
						// TODO: maybe track uninitialized return variables.
						m_assembly.appendConstant(0);
						return;
					}
				DEBUG(cout << "SLOT: " << stackSlotToString(StackSlot{_slot}) << std::endl;)
				DEBUG(cout << "THIS SHOULD NOT HAPPEN!" << std::endl;)
				// Also on control flow joins some slots are not correctly marked as junk slots and end up here so far.
				m_assembly.appendInstruction(evmasm::Instruction::CALLDATASIZE);
			},
			[&](TemporarySlot const&)
			{
				yulAssert(false, "");
			},
			[&](JunkSlot const&)
			{
				m_assembly.appendInstruction(evmasm::Instruction::CALLDATASIZE);
			},
			[&](auto const& _slot)
			{
				DEBUG(cout << "SLOT: " << stackSlotToString(StackSlot{_slot}) << std::endl;)
				DEBUG(cout << "THIS SHOULD NOT HAPPEN!" << std::endl;) // Actually it appears to happen for uninitialized return variables.
				// Also on control flow joins some slots are not correctly marked as junk slots and end up here so far.
				// TODO: Both should be fixed.
				m_assembly.appendInstruction(evmasm::Instruction::CALLDATASIZE);
				//yulAssert(false, "Slot not found.");
			}
		}, _slot);
	}, [&]() { m_assembly.appendInstruction(evmasm::Instruction::POP); });
	m_stack = commonPrefix;
	for (auto slot: temporaryStack)
		m_stack.emplace_back(slot);
}

void CodeGenerator::generateStaged()
{
	while (!m_stagedBlocks.empty())
	{
		DFG::BasicBlock const* _block = *m_stagedBlocks.begin();
		m_stagedBlocks.pop_front();
		m_stack = m_info.blockInfos.at(_block).entryLayout;
		m_assembly.setStackHeight(static_cast<int>(m_stack.size()));
		(*this)(*_block);
		// TODO: assert that this block jumped at exit.
	}
	while (!m_stagedFunctions.empty())
	{
		while (!m_stagedFunctions.empty())
		{
			DFG::FunctionInfo const* _functionInfo = *m_stagedFunctions.begin();
			m_stagedFunctions.pop_front();
			if (!m_generatedFunctions.count(_functionInfo))
			{
				m_generatedFunctions.emplace(_functionInfo);
				(*this)(*_functionInfo);
			}
			yulAssert(!m_currentFunctionInfo, "");
			m_currentFunctionInfo = _functionInfo;
			while (!m_stagedBlocks.empty())
			{
				DFG::BasicBlock const* _block = *m_stagedBlocks.begin();
				m_stagedBlocks.pop_front();
				m_stack = m_info.blockInfos.at(_block).entryLayout;
				m_assembly.setStackHeight(static_cast<int>(m_stack.size()));
				(*this)(*_block);
				// TODO: assert that this block jumped at exit.
			}
			m_currentFunctionInfo = nullptr;
		}
	}
}

void OptimizedCodeTransform::run(
	AbstractAssembly& _assembly,
	AsmAnalysisInfo& _analysisInfo,
	Block const& _block,
	EVMDialect const& _dialect,
	BuiltinContext& _builtinContext,
	ExternalIdentifierAccess const&,
	bool _useNamedLabelsForFunctions
)
{
	OptimizedCodeTransformContext context{
		DataFlowGraphBuilder::build(_analysisInfo, _dialect, _block),
		{},
		{}
	};
	DEBUG(cout << std::endl << std::endl;)
	DEBUG(cout << "GENERATE STACK LAYOUTS" << std::endl;)
	DEBUG(cout << std::endl << std::endl;)
	StackLayoutGenerator::run(context);

	DEBUG(cout << std::endl << std::endl;)
	DEBUG(cout << "FORWARD CODEGEN" << std::endl;)
	DEBUG(cout << std::endl << std::endl;)
	CodeGenerator::run(_assembly, _builtinContext, _useNamedLabelsForFunctions, context, *context.dfg->entry);
}
