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

#include <libyul/backends/evm/OptimizedEVMCodeTransform.h>
#include <libyul/backends/evm/StackHelpers.h>

#include <libsolutil/Algorithms.h>
#include <libsolutil/cxx20.h>
#include <libsolutil/Permutations.h>
#include <libsolutil/Visitor.h>

#include <range/v3/algorithm/any_of.hpp>
#include <range/v3/range/conversion.hpp>
#include <range/v3/view/all.hpp>
#include <range/v3/view/concat.hpp>
#include <range/v3/view/drop.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/take.hpp>
#include <range/v3/view/take_last.hpp>
#include <range/v3/view/transform.hpp>

using namespace solidity;
using namespace solidity::yul;
using namespace std;

StackLayoutGenerator::StackLayoutGenerator(StackLayout& _layout): m_layout(_layout)
{
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

Stack StackLayoutGenerator::propagateStackThroughOperation(Stack _exitStack, DFG::Operation const& _operation)
{
	Stack& stack = _exitStack;

	vector<set<unsigned>> targetPositions(_operation.output.size(), set<unsigned>{});
	size_t numToKeep = 0;
	for (size_t idx: ranges::views::iota(0u, targetPositions.size()))
	{
		auto offsets = findAllOffsets(stack, _operation.output.at(idx));
		for (unsigned offset: offsets)
		{
			targetPositions[idx].emplace(offset);
			++numToKeep;
		}

	}

	vector<variant<PreviousSlot, set<unsigned>>> layout;
	size_t idx = 0;
	for (auto slot: stack | ranges::views::drop_last(numToKeep))
	{
		layout.emplace_back(PreviousSlot{idx++});
	}
	// The call produces values with known target positions.
	layout += targetPositions;

	stack = createIdealLayout(stack, layout);

	if (auto const* assignment = get_if<DFG::Assignment>(&_operation.operation))
		for (auto& stackSlot: stack)
			if (auto const* varSlot = get_if<VariableSlot>(&stackSlot))
				if (util::findOffset(assignment->variables, *varSlot))
					stackSlot = JunkSlot{};

	for (StackSlot const& input: _operation.input)
		stack.emplace_back(input);

	m_layout.operationEntryLayout[&_operation] = stack;

	// TODO: We will potentially accumulate a lot of return labels here.
	// Removing them naively has huge implications on both code size and runtime gas cost (both positive and negative):
	//   cxx20::erase_if(*m_stack, [](StackSlot const& _slot) { return holds_alternative<FunctionCallReturnLabelSlot>(_slot); });
	// Consider removing them properly while accounting for the induced backwards stack shuffling.

	for (auto&& [idx, slot]: stack | ranges::views::enumerate | ranges::views::reverse)
		// We can always push literals, junk and function call return labels.
		if (holds_alternative<LiteralSlot>(slot) || holds_alternative<JunkSlot>(slot) || holds_alternative<FunctionCallReturnLabelSlot>(slot))
			stack.pop_back(); // TODO: verify that this is fine during range traversal
		// We can always dup values already on stack.
		else if (util::findOffset(stack | ranges::views::take(idx), slot))
			stack.pop_back();
		else
			break;

	// TODO: suboptimal. Should account for induced stack shuffling.
	if (stack.size() > 12)
	{
		Stack newStack;
		for (auto slot: stack)
		{
			if (holds_alternative<LiteralSlot>(slot) || holds_alternative<FunctionCallReturnLabelSlot>(slot))
				continue;
			if (util::findOffset(newStack, slot))
				continue;
			newStack.emplace_back(slot);
		}
		stack = newStack;
	}
	return stack;
}

Stack StackLayoutGenerator::propagateStackThroughBlock(Stack _exitStack, DFG::BasicBlock const& _block)
{
	Stack stack = std::move(_exitStack);
	for (auto& operation: _block.operations | ranges::views::reverse)
		stack = propagateStackThroughOperation(stack, operation);
	return stack;
}

void StackLayoutGenerator::processEntryPoint(DFG::BasicBlock const* _entry)
{
	std::list<DFG::BasicBlock const*> toVisit{_entry};
	std::set<DFG::BasicBlock const*> visited;
	std::list<std::pair<DFG::BasicBlock const*, DFG::BasicBlock const*>> backwardsJumps;

	while (!toVisit.empty())
	{
		DFG::BasicBlock const *block = *toVisit.begin();
		toVisit.pop_front();

		if (visited.count(block))
			continue;

		if (std::optional<Stack> exitLayout = std::visit(util::GenericVisitor{
			[&](DFG::BasicBlock::MainExit const&) -> std::optional<Stack>
			{
				visited.emplace(block);
				return Stack{};
			},
			[&](DFG::BasicBlock::Jump const& _jump) -> std::optional<Stack>
			{
				if (_jump.backwards)
				{
					visited.emplace(block);
					backwardsJumps.emplace_back(block, _jump.target);
					if (auto* info = util::valueOrNullptr(m_layout.blockInfos, _jump.target))
						return info->entryLayout;
					return Stack{};
				}
				if (visited.count(_jump.target))
				{
					visited.emplace(block);
					return m_layout.blockInfos.at(_jump.target).entryLayout;
				}
				toVisit.emplace_front(_jump.target);
				return nullopt;
			},
			[&](DFG::BasicBlock::ConditionalJump const& _conditionalJump) -> std::optional<Stack>
			{
				bool zeroVisited = visited.count(_conditionalJump.zero);
				bool nonZeroVisited = visited.count(_conditionalJump.nonZero);
				if (zeroVisited && nonZeroVisited)
				{
					Stack stack = combineStack(
						m_layout.blockInfos.at(_conditionalJump.zero).entryLayout,
						m_layout.blockInfos.at(_conditionalJump.nonZero).entryLayout
					);
					stack.emplace_back(_conditionalJump.condition);
					visited.emplace(block);
					return stack;
				}
				if (!zeroVisited)
					toVisit.emplace_front(_conditionalJump.zero);
				if (!nonZeroVisited)
					toVisit.emplace_front(_conditionalJump.nonZero);
				return nullopt;
			},
			[&](DFG::BasicBlock::FunctionReturn const& _functionReturn) -> std::optional<Stack>
			{
				visited.emplace(block);
				yulAssert(_functionReturn.info, "");
				Stack stack = _functionReturn.info->returnVariables | ranges::views::transform([](auto const& _varSlot){
					return StackSlot{_varSlot};
				}) | ranges::to<Stack>;
				stack.emplace_back(FunctionReturnLabelSlot{});
				return stack;
			},
			[&](DFG::BasicBlock::Terminated const&) -> std::optional<Stack>
			{
				visited.emplace(block);
				return Stack{};
			},
		}, block->exit))
		{
			// TODO: why does this not work?
			/*if (auto* previousInfo = util::valueOrNullptr(m_context.blockInfos, block))
				if (previousInfo->exitLayout == *exitLayout)
					continue;*/
			auto& info = m_layout.blockInfos[block];
			info.exitLayout = *exitLayout;
			info.entryLayout = propagateStackThroughBlock(info.exitLayout, *block);

			for (auto entry: block->entries)
				toVisit.emplace_back(entry);
		}
	}

	for (auto [block, target]: backwardsJumps)
		if (ranges::any_of(
			m_layout.blockInfos[target].entryLayout,
			[exitLayout = m_layout.blockInfos[block].exitLayout](StackSlot const& _slot) {
				return !util::findOffset(exitLayout, _slot);
			}
		))
			// This block jumps backwards, but does not provide all slots required by the jump target on exit.
			// Visit the subgraph starting at the block once more; the block will now start with the
			// required entry layout.
			// TODO: While this will eventually stabilize and terminate, this will currently traverse the graph more
			// often than required. Trying to cleverly clear ``visited`` might be more efficient.
			processEntryPoint(block);
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

	Stack candidate;
	for (auto slot: stack1)
		if (!util::findOffset(candidate, slot))
			candidate.emplace_back(slot);
	for (auto slot: stack2)
		if (!util::findOffset(candidate, slot))
			candidate.emplace_back(slot);
	cxx20::erase_if(candidate, [](StackSlot const& slot) {
		return holds_alternative<LiteralSlot>(slot) || holds_alternative<FunctionCallReturnLabelSlot>(slot);
	});

	std::map<size_t, Stack> sortedCandidates;

/*	if (candidate.size() > 8)
		return candidate;*/

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

	for (auto slot: sortedCandidates.begin()->second)
		commonPrefix.emplace_back(slot);
	return commonPrefix;
}

void StackLayoutGenerator::stitchConditionalJumps(DFG::BasicBlock& _block)
{
	util::BreadthFirstSearch<DFG::BasicBlock*> breadthFirstSearch{{&_block}};
	breadthFirstSearch.run([&](DFG::BasicBlock* _block, auto _addChild) {
		auto& info = m_layout.blockInfos.at(_block);
		std::visit(util::GenericVisitor{
			[&](DFG::BasicBlock::MainExit const&) {},
			[&](DFG::BasicBlock::Jump const& _jump)
			{
				if (!_jump.backwards)
					_addChild(_jump.target);
			},
			[&](DFG::BasicBlock::ConditionalJump const& _conditionalJump)
			{
				auto& zeroTargetInfo = m_layout.blockInfos.at(_conditionalJump.zero);
				auto& nonZeroTargetInfo = m_layout.blockInfos.at(_conditionalJump.nonZero);
				// TODO: Assert correctness, resp. achievability of layout.
				Stack exitLayout = info.exitLayout;
				yulAssert(!exitLayout.empty(), "");
				exitLayout.pop_back();
				{
					Stack newZeroEntryLayout = exitLayout;
					for (auto& slot: newZeroEntryLayout)
						if (!util::findOffset(zeroTargetInfo.entryLayout, slot))
							slot = JunkSlot{};
					zeroTargetInfo.entryLayout = newZeroEntryLayout;
				}
				{
					Stack newNonZeroEntryLayout = exitLayout;
					for (auto& slot: newNonZeroEntryLayout)
						if (!util::findOffset(nonZeroTargetInfo.entryLayout, slot))
							slot = JunkSlot{};
					nonZeroTargetInfo.entryLayout = newNonZeroEntryLayout;
				}
				_addChild(_conditionalJump.zero);
				_addChild(_conditionalJump.nonZero);
			},
			[&](DFG::BasicBlock::FunctionReturn const&)	{},
			[&](DFG::BasicBlock::Terminated const&) { },
		}, _block->exit);
	});
}

void StackLayoutGenerator::fixStackTooDeep(DFG::BasicBlock& _block)
{
	// This is just an initial proof of concept. Doing this in a clever way and in all cases will take some doing.
	util::BreadthFirstSearch<DFG::BasicBlock*> breadthFirstSearch{{&_block}};
	breadthFirstSearch.run([&](DFG::BasicBlock* _block, auto _addChild) {
		Stack stack;
		stack = m_layout.blockInfos.at(_block).entryLayout;

		for (auto&& [index, operation]: _block->operations | ranges::views::enumerate)
		{
			Stack& operationEntry = m_layout.operationEntryLayout.at(&operation);
			auto unreachable = OptimizedEVMCodeTransform::tryCreateStackLayout(stack, operationEntry);
			if (!unreachable.empty())
			{
				std::cout << "UNREACHABLE SLOTS DURING OPERATION ENTRY: " << stackToString(unreachable) << std::endl;
				std::cout << "ATTEMPTING AD HOC FIX" << std::endl;
				for (auto& op: (_block->operations | ranges::views::take(index)) | ranges::views::reverse)
				{
					Stack& opEntry = m_layout.operationEntryLayout.at(&op);
					Stack newStack = ranges::concat_view(
						opEntry | ranges::views::take(opEntry.size() - op.input.size()),
						unreachable,
						opEntry | ranges::views::take_last(op.input.size())
					) | ranges::to<Stack>;
					opEntry = newStack;
				}
			}
			stack = operationEntry;
			for (size_t i = 0; i < operation.input.size(); i++)
				stack.pop_back();
			stack += operation.output;
		}
		auto unreachable = OptimizedEVMCodeTransform::tryCreateStackLayout(stack, m_layout.blockInfos.at(_block).exitLayout);
		if (!unreachable.empty())
		{
			std::cout << "UNREACHABLE SLOTS AT BLOCK EXIT: " << stackToString(unreachable) << std::endl;
			std::cout << "ATTEMPTING AD HOC FIX" << std::endl;
			for (auto& op: _block->operations | ranges::views::reverse)
			{
				Stack& opEntry = m_layout.operationEntryLayout.at(&op);
				Stack newStack = ranges::concat_view(
					opEntry | ranges::views::take(opEntry.size() - op.input.size()),
					unreachable,
					opEntry | ranges::views::take_last(op.input.size())
				) | ranges::to<Stack>;
				opEntry = newStack;
			}
		}
		stack = m_layout.blockInfos.at(_block).exitLayout;

		std::visit(util::GenericVisitor{
			[&](DFG::BasicBlock::MainExit const&) {},
			[&](DFG::BasicBlock::Jump const& _jump)
			{
				auto unreachable = OptimizedEVMCodeTransform::tryCreateStackLayout(stack, m_layout.blockInfos.at(_jump.target).entryLayout);
				if (!unreachable.empty())
					std::cout
						<< "UNREACHABLE SLOTS AT JUMP: " << stackToString(unreachable) << std::endl
						<< "CANNOT FIX YET" << std::endl;

				if (!_jump.backwards)
					_addChild(_jump.target);
			},
			[&](DFG::BasicBlock::ConditionalJump const& _conditionalJump)
			{
				auto unreachable = OptimizedEVMCodeTransform::tryCreateStackLayout(stack, m_layout.blockInfos.at(_conditionalJump.zero).entryLayout);
				if (!unreachable.empty())
					std::cout
						<< "UNREACHABLE SLOTS AT CONDITIONAL JUMP: " << stackToString(unreachable) << std::endl
						<< "CANNOT FIX YET" << std::endl;
				unreachable = OptimizedEVMCodeTransform::tryCreateStackLayout(stack, m_layout.blockInfos.at(_conditionalJump.nonZero).entryLayout);
				if (!unreachable.empty())
					std::cout
						<< "UNREACHABLE SLOTS AT CONDITIONAL JUMP: " << stackToString(unreachable) << std::endl
						<< "CANNOT FIX YET" << std::endl;

				_addChild(_conditionalJump.zero);
				_addChild(_conditionalJump.nonZero);
			},
			[&](DFG::BasicBlock::FunctionReturn const&) {},
			[&](DFG::BasicBlock::Terminated const&) { },
		}, _block->exit);

	});
}

StackLayout StackLayoutGenerator::run(DFG const& _dfg)
{
	StackLayout stackLayout;
	StackLayoutGenerator stackLayoutGenerator{stackLayout};

	stackLayoutGenerator.processEntryPoint(_dfg.entry);
	for (auto& functionInfo: _dfg.functionInfo | ranges::views::values)
		stackLayoutGenerator.processEntryPoint(functionInfo.entry);

	std::set<DFG::BasicBlock const*> visited;
	stackLayoutGenerator.stitchConditionalJumps(*_dfg.entry);
	for (auto& functionInfo: _dfg.functionInfo | ranges::views::values)
		stackLayoutGenerator.stitchConditionalJumps(*functionInfo.entry);

	stackLayoutGenerator.fixStackTooDeep(*_dfg.entry);
	for (auto& functionInfo: _dfg.functionInfo | ranges::views::values)
		stackLayoutGenerator.fixStackTooDeep(*functionInfo.entry);


	return stackLayout;
}
