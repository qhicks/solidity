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

#include <libyul/backends/evm/StackLayoutGenerator.h>

#include <libyul/backends/evm/StackHelpers.h>
#include <libyul/backends/evm/OptimizedEVMCodeTransform.h>

#include <libsolutil/cxx20.h>
#include <libsolutil/Permutations.h>
#include <libsolutil/Visitor.h>

#include <range/v3/to_container.hpp>
#include <range/v3/view/drop.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/take.hpp>
#include <range/v3/view/transform.hpp>

using namespace solidity;
using namespace solidity::yul;
using namespace std;

#if 1
#define DEBUG(x) x
#else
#define DEBUG(x) (void)0;
#endif

StackLayoutGenerator::StackLayoutGenerator(OptimizedCodeTransformContext& _context): m_context(_context)
{

}

void StackLayoutGenerator::operator()(DFG::FunctionCall const& _call)
{
	(void)_call;
	DEBUG(cout << "B: function call " << _call.functionCall->functionName.name.str() << ": " << stackToString(*m_stack) << std::endl;)
}

void StackLayoutGenerator::operator()(DFG::BuiltinCall const& _call)
{
	(void)_call;
	DEBUG(cout << "B: bultin call " << _call.functionCall->functionName.name.str() << ": " << stackToString(*m_stack) << std::endl;)
}

void StackLayoutGenerator::operator()(DFG::Assignment const& _assignment)
{
	for (auto& stackSlot: *m_stack)
		if (auto const* varSlot = get_if<VariableSlot>(&stackSlot))
			if (util::findOffset(_assignment.variables, *varSlot))
				stackSlot = JunkSlot{};
	DEBUG(cout << "B: assignment (";)
	for (auto var: _assignment.variables)
		DEBUG(cout << var.variable->name.str() << " ";)
	DEBUG(cout << ") pre: " << stackToString(*m_stack) << std::endl;)
}

namespace
{
struct PreviousSlot { size_t slot; };

Stack createIdealLayout(Stack const& post, vector<variant<PreviousSlot, set<unsigned>>> layout)
{
// TODO: argue that util::permuteDup works with this kind of _getTargetPosition.
// Or even better... rewrite this as custom algorithm matching createStackLayout exactly and make it work
// for all cases, including duplicates and removals of slots that can be generated on the fly, etc.
	util::permuteDup(static_cast<unsigned>(layout.size()), [&](unsigned _i) -> set<unsigned> {
// For call return values the target position is known.
		if (set<unsigned>* pos = get_if<set<unsigned>>(&layout.at(_i)))
			return *pos;
// Previous arguments can stay where they are.
		return {_i};
	}, [&](unsigned _i) {
		std::swap(layout.back(), layout.at(layout.size() - _i - 1));
	}, [&](unsigned _i) {
		auto positions = get_if<set<unsigned>>(&layout.at(layout.size() - _i));
		yulAssert(positions, "");
		if (positions->count(static_cast<unsigned>(layout.size())))
		{
			positions->erase(static_cast<unsigned>(layout.size()));
			layout.emplace_back(set<unsigned>{static_cast<unsigned>(layout.size())});
		}
		else
		{
			optional<unsigned> duppingOffset;
			for (unsigned pos: *positions)
			{
				if (pos != layout.size() - _i)
				{
					duppingOffset = pos;
					break;
				}
			}
			yulAssert(duppingOffset, "");
			positions->erase(*duppingOffset);
			layout.emplace_back(set<unsigned>{*duppingOffset});
		}
	}, [&]() {
		yulAssert(false, "");
	}, [&]() {
		layout.pop_back();
	});

// Now we can construct the ideal layout before the operation.
// "layout" has the declared variables in the desired position and
// for any PreviousSlot{x}, x yields the ideal place of the slot before the declaration.
	vector<optional<StackSlot>> idealLayout(post.size(), nullopt);
	for (auto const& [slot, idealPosition]: ranges::zip_view(post, layout))
		if (PreviousSlot* previousSlot = std::get_if<PreviousSlot>(&idealPosition))
			idealLayout.at(previousSlot->slot) = slot;

	while (!idealLayout.empty() && !idealLayout.back())
		idealLayout.pop_back();

	return idealLayout | ranges::views::transform([](optional<StackSlot> s) {
		yulAssert(s, "");
		return *s;
	}) | ranges::to<Stack>;
}
}

void StackLayoutGenerator::operator()(DFG::Operation const& _operation)
{
	yulAssert(m_stack, "");

	OptimizedCodeTransformContext::OperationInfo& operationInfo = m_context.operationStacks[&_operation];
	operationInfo.exitStack = *m_stack;

	DEBUG(
		cout << "OPERATION post:   " << stackToString(*m_stack) << std::endl
			<< "          input:  " << stackToString(_operation.input) << std::endl
			<< "          output: " << stackToString(_operation.output) << std::endl;
	)

	vector<set<unsigned>> targetPositions(_operation.output.size(), set<unsigned>{});
	size_t numToKeep = 0;
	for (size_t idx: ranges::views::iota(0u, targetPositions.size()))
	{
		auto offsets = findAllOffsets(*m_stack, _operation.output.at(idx));
		for (unsigned offset: offsets)
		{
			targetPositions[idx].emplace(offset);
			++numToKeep;
		}

	}

	vector<variant<PreviousSlot, set<unsigned>>> layout;
	size_t idx = 0;
	for (auto slot: *m_stack | ranges::views::drop_last(numToKeep))
	{
		layout.emplace_back(PreviousSlot{idx++});
	}
	// The call produces values with known target positions.
	layout += targetPositions;

	*m_stack = createIdealLayout(*m_stack, layout);

	std::visit(*this, _operation.operation);

	for (StackSlot const& input: _operation.input)
		m_stack->emplace_back(input);

	operationInfo.entryStack = *m_stack;

	DEBUG(cout << "Operation pre before compress: " << stackToString(*m_stack) << std::endl;)

	// TODO: this is an improper hack and does not account for the induced shuffling.
	cxx20::erase_if(*m_stack, [](StackSlot const& _slot) { return holds_alternative<FunctionCallReturnLabelSlot>(_slot); });

	for (auto&& [idx, slot]: *m_stack | ranges::views::enumerate | ranges::views::reverse)
		// We can always push literals, junk and function call return labels.
		if (holds_alternative<LiteralSlot>(slot) || holds_alternative<JunkSlot>(slot) || holds_alternative<FunctionCallReturnLabelSlot>(slot))
			m_stack->pop_back(); // TODO: verify that this is fine during range traversal
		// We can always dup values already on stack.
		else if (util::findOffset(*m_stack | ranges::views::take(idx), slot))
			m_stack->pop_back();
		else
			break;
	DEBUG(cout << "Operation pre after compress: " << stackToString(*m_stack) << "   " << m_stack << std::endl;)

	if (m_stack->size() > 12)
	{
		Stack newStack;
		for (auto slot: *m_stack)
		{
			if (holds_alternative<LiteralSlot>(slot) || holds_alternative<FunctionCallReturnLabelSlot>(slot))
				continue;
			if (util::findOffset(newStack, slot))
				continue;
			newStack.emplace_back(slot);
		}
		*m_stack = newStack;
	}
}

Stack StackLayoutGenerator::operator()(DFG::BasicBlock const& _block, Stack _initialExitLayout)
{
	if (auto* lastSeenExitLayout = util::valueOrNullptr(m_initialExitLayoutOnLastVisit, &_block))
	{
		bool allSeen = true;
		for (auto slot: _initialExitLayout)
			if (!util::findOffset(*lastSeenExitLayout, slot))
			{
				allSeen = false;
				break;
			}
		if (allSeen)
			return m_context.blockInfos.at(&_block).entryLayout;
		bool allPresent = true;
		for (auto slot: *lastSeenExitLayout)
			if (!util::findOffset(_initialExitLayout, slot))
//				_initialExitLayout.emplace_back(slot); // TODO: think through when this happens. Maybe use combineStack.
			{
				allPresent = false;
				break;
			}
		if (!allPresent)
			_initialExitLayout = combineStack(_initialExitLayout, *lastSeenExitLayout);
		//yulAssert(util::findOffset(_initialExitLayout, slot), "How to deal with this?");
	}
	m_initialExitLayoutOnLastVisit[&_block] = _initialExitLayout;


	ScopedSaveAndRestore stackRestore(m_stack, nullptr);
	BlockGenerationInfo& info = m_context.blockInfos[&_block];

	Stack currentStack = _initialExitLayout;

	DEBUG(cout << "Block: " << &_block << std::endl;)

	std::visit(util::GenericVisitor{
		[&](DFG::BasicBlock::MainExit const&)
		{
			currentStack.clear();
		},
		[&](DFG::BasicBlock::Jump const& _jump)
		{
			if (!_jump.backwards)
			{
				currentStack = (*this)(*_jump.target, {});
				currentStack = combineStack(_initialExitLayout, currentStack); // TODO: why is this necessary?
/*				for (auto slot: _initialExitLayout)
					yulAssert(util::findOffset(currentStack, slot), "1");*/
			}

			DEBUG(cout << "B: JUMP EXIT: " << _jump.target << std::endl;)
			/*
			if (!targetInfo.entryLayout.has_value())
			{
				// We're in a loop. We must have jumped here conditionally:
				auto const* conditionalJump = get_if<DFG::BasicBlock::ConditionalJump>(&_jump.target->exit);
				yulAssert(conditionalJump, "");
				DEBUG(cout << "Current " << &_block << " conditonals: " << conditionalJump->zero << " " << conditionalJump->nonZero << std::endl;)
				{
					BlockGenerationInfo& zeroEntryInfo = m_context.blockInfos.at(conditionalJump->zero);
					BlockGenerationInfo& nonZeroEntryInfo = m_context.blockInfos.at(conditionalJump->nonZero);
					// One branch must already have an entry layout, the other is the looping branch.
					yulAssert(zeroEntryInfo.entryLayout.has_value() != nonZeroEntryInfo.entryLayout.has_value(), "");

					auto layout = zeroEntryInfo.entryLayout ? zeroEntryInfo.entryLayout : nonZeroEntryInfo.entryLayout;
					yulAssert(layout.has_value(), "");
					DEBUG(cout << "Looping entry layout: " << stackToString(*layout) << std::endl;)
					DEBUG(cout << "Zero has value: " << zeroEntryInfo.entryLayout.has_value() << std::endl;)
					DEBUG(cout << "Non-Zero has value: " << nonZeroEntryInfo.entryLayout.has_value() << std::endl;)


				}
			}
			yulAssert(targetInfo.entryLayout.has_value(), "");
			currentStack = *targetInfo.entryLayout;*/
		},
		[&](DFG::BasicBlock::ConditionalJump const& _conditionalJump)
		{
			Stack zeroEntryStack = (*this)(*_conditionalJump.zero, {});
			Stack nonZeroEntryStack = (*this)(*_conditionalJump.nonZero, {});
			currentStack = combineStack(zeroEntryStack, nonZeroEntryStack);
			/*BlockGenerationInfo& zeroInfo = m_context.blockInfos.at(_conditionalJump.zero);
			BlockGenerationInfo& nonZeroInfo = m_context.blockInfos.at(_conditionalJump.nonZero);
			zeroInfo.entryLayout = currentStack;
			nonZeroInfo.entryLayout = currentStack;*/
			currentStack = combineStack(_initialExitLayout, currentStack);
/*			for (auto slot: _initialExitLayout)
			{
				currentStack.emplace_front(slot);
				continue;
				std::cout << stackToString(_initialExitLayout) << " vs "  << stackToString(currentStack) << std::endl;
				yulAssert(util::findOffset(currentStack, slot), "2");
			}*/

			currentStack.emplace_back(_conditionalJump.condition);


			DEBUG(cout << "B: CONDITIONAL JUMP EXIT: " << _conditionalJump.zero << " / " << _conditionalJump.nonZero << std::endl;)
		},
		[&](DFG::BasicBlock::FunctionReturn const& _functionReturn)
		{
			yulAssert(_functionReturn.info, "");
			currentStack = _functionReturn.info->returnVariables | ranges::views::transform([](auto const& _varSlot){
				return StackSlot{_varSlot};
			}) | ranges::to<Stack>;
			currentStack.emplace_back(FunctionReturnLabelSlot{});
		},
		[&](DFG::BasicBlock::Terminated const&) { currentStack.clear(); },
	}, _block.exit);

	DEBUG(cout << "B: BLOCK: " << &_block << std::endl;)

	m_stack = &currentStack;
	info.exitLayout = currentStack;

	DEBUG(cout << "B: EXIT LAYOUT (" << &_block << "): " << stackToString(currentStack) << std::endl;)

	for (auto& operation: _block.operations | ranges::views::reverse)
		(*this)(operation);

	DEBUG(cout << "B: ENTRY LAYOUT (" << &_block << "): " << stackToString(currentStack) << std::endl;)

	info.entryLayout = currentStack;

	for (auto const* backEntry: _block.entries)
		(*this)(*backEntry, currentStack);

	return info.entryLayout;
}

Stack StackLayoutGenerator::combineStack(Stack const& _stack1, Stack const& _stack2)
{
	if (_stack1.empty())
		return _stack2;
	if (_stack2.empty())
		return _stack1;

	// TODO: there is probably a better way than brute-forcing. This has n! complexity or worse, so
	// we can't keep it like this.

	Stack commonPrefix;
	for (auto&& [slot1, slot2]: ranges::zip_view(_stack1, _stack2))
	{
		if (!(slot1 == slot2))
			break;
		commonPrefix.emplace_back(slot1);
	}
	Stack stack1 = _stack1 | ranges::views::drop(commonPrefix.size()) | ranges::to<Stack>;
	Stack stack2 = _stack2 | ranges::views::drop(commonPrefix.size()) | ranges::to<Stack>;
	cxx20::erase_if(stack1, [](StackSlot const& slot) {
		return holds_alternative<LiteralSlot>(slot) || holds_alternative<FunctionCallReturnLabelSlot>(slot);
	});
	cxx20::erase_if(stack2, [](StackSlot const& slot) {
		return holds_alternative<LiteralSlot>(slot) || holds_alternative<FunctionCallReturnLabelSlot>(slot);
	});

	Stack candidate;
	for (auto slot: stack1)
		if (!util::findOffset(candidate, slot))
			candidate.emplace_back(slot);
	for (auto slot: stack2)
		if (!util::findOffset(candidate, slot))
			candidate.emplace_back(slot);
	std::map<size_t, Stack> sortedCandidates;

/*	if (candidate.size() > 8)
		return candidate;*/

	DEBUG(cout << "COMBINE STACKS: " << stackToString(stack1) << " + " << stackToString(stack2) << std::endl;)

	auto evaluate = [&](Stack const& _candidate) -> size_t {
		unsigned numOps = 0;
		Stack testStack = _candidate;
		createStackLayout(
			testStack,
			stack1,
			[&](unsigned _swapDepth) { ++numOps; if (_swapDepth > 16) numOps += 1000; },
			[&](unsigned _dupDepth) { ++numOps; if (_dupDepth > 16) numOps += 1000; },
			[&](StackSlot const&) {},
			[&](){},
			true
		);
		testStack = _candidate;
		createStackLayout(
			testStack,
			stack2,
			[&](unsigned _swapDepth) { ++numOps; if (_swapDepth > 16) numOps += 1000;  },
			[&](unsigned _dupDepth) { ++numOps; if (_dupDepth > 16) numOps += 1000; },
			[&](StackSlot const&) {},
			[&](){},
			true
		);
		// DEBUG(cout << "  CANDIDATE: " << stackToString(_candidate) << ": " << numOps << " swaps." << std::endl;)
		return numOps;
	};

	// See https://en.wikipedia.org/wiki/Heap's_algorithm
	size_t n = candidate.size();
	sortedCandidates.insert(std::make_pair(evaluate(candidate), candidate));
	std::vector<size_t> c(n, 0);
	size_t i = 1;
	while (i < n)
	{
		if (c[i] < i)
		{
			if (i & 1)
				std::swap(candidate.front(), candidate[i]);
			else
				std::swap(candidate[c[i]], candidate[i]);
			sortedCandidates.insert(std::make_pair(evaluate(candidate), candidate));
			++c[i];
			++i;
		}
		else
		{
			c[i] = 0;
			++i;
		}
	}

	DEBUG(cout << " BEST: " << stackToString(sortedCandidates.begin()->second) << " (" << sortedCandidates.begin()->first << " swaps)" << std::endl;)

	for (auto slot: sortedCandidates.begin()->second)
		commonPrefix.emplace_back(slot);
	return commonPrefix;
}

void StackLayoutGenerator::stitchTogether(DFG::BasicBlock& _block, std::set<DFG::BasicBlock const*>& _visited)
{
	if (_visited.count(&_block))
		return;
	_visited.insert(&_block);
	auto& info = m_context.blockInfos.at(&_block);
	std::visit(util::GenericVisitor{
		[&](DFG::BasicBlock::MainExit const&)
		{
		},
		[&](DFG::BasicBlock::Jump const& _jump)
		{
			/*auto& targetInfo = context.blockInfos.at(_jump.target);
			// TODO: Assert correctness, resp. achievability of layout.
			targetInfo.entryLayout = info.exitLayout;*/
			if (!_jump.backwards)
				stitchTogether(*_jump.target, _visited);
		},
		[&](DFG::BasicBlock::ConditionalJump const& _conditionalJump)
		{
			auto& zeroTargetInfo = m_context.blockInfos.at(_conditionalJump.zero);
			auto& nonZeroTargetInfo = m_context.blockInfos.at(_conditionalJump.nonZero);
			// TODO: Assert correctness, resp. achievability of layout.
			Stack exitLayout = info.exitLayout;
			yulAssert(!exitLayout.empty(), "");
			exitLayout.pop_back();
			zeroTargetInfo.entryLayout = exitLayout;
			nonZeroTargetInfo.entryLayout = exitLayout;
			stitchTogether(*_conditionalJump.zero, _visited);
			stitchTogether(*_conditionalJump.nonZero, _visited);
		},
		[&](DFG::BasicBlock::FunctionReturn const&)
		{
		},
		[&](DFG::BasicBlock::Terminated const&) { },
	}, _block.exit);

}

void StackLayoutGenerator::run(OptimizedCodeTransformContext& _context)
{
	StackLayoutGenerator stackLayoutGenerator{_context};
	stackLayoutGenerator(*_context.dfg->entry, {});
	for (auto& functionInfo: _context.dfg->functions | ranges::views::values)
		stackLayoutGenerator(*functionInfo.entry, {});

	std::set<DFG::BasicBlock const*> visited;
	stackLayoutGenerator.stitchTogether(*_context.dfg->entry, visited);
	for (auto& functionInfo: _context.dfg->functions | ranges::views::values)
		stackLayoutGenerator.stitchTogether(*functionInfo.entry, visited);
}