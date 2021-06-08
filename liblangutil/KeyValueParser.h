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

#include <map>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace solidity::liblangutil
{

enum class KeyValueParserError
{
	InvalidTagName,     // such as empty tags
	// TODO: What other types of errors can we get?
};

class KeyValueParser
{
public:
	using ValueSequence = std::vector<std::string_view>;
	using KeyValueMap = std::map<std::string_view, ValueSequence>;

	struct Result
	{
		KeyValueMap taggedValues;
		ValueSequence untagged;
	};

	std::variant<Result, KeyValueParserError> parse(std::string_view _input);

private:
	std::optional<std::pair<std::string_view, std::string_view>> taggedValue();
	std::optional<std::string_view> multilineText();
	std::optional<std::string_view> textLine();
	std::optional<std::string_view> textLineContinuation();
	std::optional<std::string_view> tagName();

	constexpr bool eos() const noexcept { return m_text.empty(); }

	// input
	//
	std::string_view m_text{};

	// output data
	//
	Result m_parsedData{};
};

}
