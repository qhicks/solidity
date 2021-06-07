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

#include <libyul/DataFlowGraph.h>

#include <libsolutil/Visitor.h>

#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/reverse.hpp>

namespace solidity::yul
{

inline std::string stackSlotToString(StackSlot const& _slot)
{
	return std::visit(util::GenericVisitor{
		[](FunctionCallReturnLabelSlot const& _ret) -> std::string { return "RET[" + _ret.call.get().functionName.name.str() + "]"; },
		[](FunctionReturnLabelSlot const&) -> std::string { return "RET"; },
		[](VariableSlot const& _var) { return _var.variable->name.str(); },
		[](LiteralSlot const& _lit) { return util::toCompactHexWithPrefix(_lit.value); },
		[](TemporarySlot const& _tmp) -> std::string { return "TMP[" + _tmp.call.get().functionName.name.str() + ", " + std::to_string(_tmp.idx) + "]"; },
		[](JunkSlot const&) -> std::string { return "JUNK"; }
	}, _slot);
}

inline std::string stackToString(Stack const& _stack)
{
	std::string result("[ ");
	for (auto const& slot: _stack)
		result += stackSlotToString(slot) + ' ';
	result += ']';
	return result;
}
template<typename Range, typename Value>
std::set<unsigned> findAllOffsets(Range&& _range, Value&& _value)
{
	std::set<unsigned> result;
	auto begin = std::begin(_range);
	auto end = std::end(_range);
	auto it = begin;
	while (it != end)
	{
		it = std::find(it, end, std::forward<Value>(_value));
		if (it == end)
			return result;
		result.emplace(static_cast<unsigned>(std::distance(begin, it)));
		++it;
	}
	return result;
}

template<typename Swap, typename Dup, typename Pop, typename PushSlot>
void createStackLayout(Stack& _currentStack, Stack const& _targetStack, Swap _swap, Dup _dup, PushSlot _push, Pop _pop, bool _silent = false)
{
	_silent = true;
	if (_currentStack == _targetStack)
		return;
	if (!_silent)
		std::cout << "CREATE STACK LAYOUT: " << stackToString(_targetStack) << " FROM " << stackToString(_currentStack) << std::endl;

	if (_currentStack.empty())
	{
		while (_currentStack.size() < _targetStack.size())
		{
			StackSlot newSlot = _targetStack.at(_currentStack.size());
			_push(newSlot);
			_currentStack.emplace_back(newSlot);
		}
		yulAssert(_currentStack == _targetStack, "");
		return;
	}

	auto topTargets = findAllOffsets(_targetStack, _currentStack.back());
	if (topTargets.size() < findAllOffsets(_currentStack, _currentStack.back()).size())
	{
		if (!_silent)
			std::cout << "POP TOP" << std::endl;
		_pop();
		_currentStack.pop_back();
		createStackLayout(_currentStack, _targetStack, _swap, _dup, _push, _pop, _silent);
		return;
	}
	else if (_targetStack.size() >= _currentStack.size() && _targetStack.at(_currentStack.size() - 1) == _currentStack.back())
	{
		if (!_silent)
			std::cout << "TOP is in place" << std::endl;

		// Current top is in place.
		// Dup deepest one to be dupped (TODO: choose optimal).
		for (auto&& [offset, slot]: _currentStack | ranges::views::enumerate)
		{
			if (findAllOffsets(_currentStack, slot).size() < findAllOffsets(_targetStack, slot).size())
			{
				if (!_silent)
					std::cout << "DUP" << std::endl;

				auto leastDeepOccurrence = util::findOffset(_currentStack | ranges::views::reverse, slot);
				yulAssert(leastDeepOccurrence, "");
				_dup(static_cast<unsigned>(*leastDeepOccurrence + 1));
				//_dup(static_cast<unsigned>(_currentStack.size() - offset));

				_currentStack.emplace_back(_currentStack.at(offset));
				createStackLayout(_currentStack, _targetStack, _swap, _dup, _push, _pop, _silent);
				return;
			}
		}
		// Nothing to dup. Find anything to be pushed and push it.
		for (auto const& slot: _targetStack)
		{
			if (!util::findOffset(_currentStack, slot))
			{
				if (!_silent)
					std::cout << "PUSH" << std::endl;
				_push(slot);
				_currentStack.emplace_back(slot);
				createStackLayout(_currentStack, _targetStack, _swap, _dup, _push, _pop, _silent);
				return;
			}
		}
		// Nothing to push or dup.
		// Swap the deepest one that's not in place up.
		for (auto&& [offset, slot]: _currentStack | ranges::views::enumerate)
		{
			if (!(slot == _targetStack.at(offset)) && !(slot == _currentStack.back()))
			{
				if (!_silent)
					std::cout << "SWAP " << offset << std::endl;
				_swap(static_cast<unsigned>(_currentStack.size() - offset - 1));
				std::swap(_currentStack.back(), _currentStack.at(offset));
				createStackLayout(_currentStack, _targetStack, _swap, _dup, _push, _pop, _silent);
				return;
			}
		}
		// Nothing to push or dup and nothing out of place => done.
		yulAssert(_currentStack == _targetStack, "");
		return;
	}
	else
	{
		if (!_silent)
			std::cout << "TOP is not in place" << std::endl;

		for (unsigned deepestTopTarget: topTargets)
		{
			if (deepestTopTarget >= _currentStack.size())
				break;
			if (!(_currentStack.at(deepestTopTarget) == _targetStack.at(deepestTopTarget)))
			{
				if (!_silent)
					std::cout << "Move into place " << deepestTopTarget << std::endl;

				// Move top into place.
				_swap(static_cast<unsigned>(_currentStack.size() - deepestTopTarget - 1));
				std::swap(_currentStack.back(), _currentStack.at(deepestTopTarget));
				createStackLayout(_currentStack, _targetStack, _swap, _dup, _push, _pop, _silent);
				return;
			}
		}

		// There needs to be something to dup or push. Try dupping. (TODO: suboptimal)
		for (auto&& [offset, slot]: _currentStack | ranges::views::enumerate)
		{
			if (findAllOffsets(_currentStack, slot).size() < findAllOffsets(_targetStack, slot).size())
			{
				if (!_silent)
					std::cout << "DUP " << offset << std::endl;

				auto leastDeepOccurrence = util::findOffset(_currentStack | ranges::views::reverse, slot);
				yulAssert(leastDeepOccurrence, "");
				_dup(static_cast<unsigned>(*leastDeepOccurrence + 1));
				// _dup(static_cast<unsigned>(_currentStack.size() - offset));

				_currentStack.emplace_back(_currentStack.at(offset));
				createStackLayout(_currentStack, _targetStack, _swap, _dup, _push, _pop, _silent);
				return;
			}
		}
		// Nothing to dup. Find anything to be pushed and push it.
		for (auto const& slot: _targetStack)
		{
			if (!util::findOffset(_currentStack, slot))
			{
				if (!_silent)
					std::cout << "PUSH" << std::endl;
				_push(slot);
				_currentStack.template emplace_back(slot);
				createStackLayout(_currentStack, _targetStack, _swap, _dup, _push, _pop, _silent);
				return;
			}
		}
		yulAssert(false, "");
	}

	if (!_silent)
		std::cout << "CREATED STACK LAYOUT: " << stackToString(_currentStack) << std::endl;
	yulAssert(_currentStack == _targetStack, "");

}

}