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

#include <liblangutil/KeyValueParser.h>

#include <liblangutil/ErrorReporter.h>

using namespace std;
using namespace std::string_view_literals;

/**
 * EBNF:
 *
 * StructuredDocumentation ::= MultilineText? TaggedValue*
 *
 * TaggedValue             ::= SP* '@' NAME SP+ MultilineText
 *
 * MultilineText           ::= TextLine NL TextLineContinuation*
 * TextLine                ::= [^@] <text except NL>
 * TextLineContinuation    ::= SP* TextLine NL
 *
 * NAME                    ::= [A-Za-z0-9_-]+
 *
 * Examples:
 *
 * @15-foo blah
 *
 * @-blah  fnord
 *
 * @x-foo  line-1
 *     line-2
 *   ...
 *        line-N @blah
 *
 * @next   first line to that `next`
 *
 *         with an empty line another line, both belonging to the above `next`.
 */

/**
 * @brief some header doc
 *
 * @note blah my note
 *
 * yet another blob
 *
 * @param x y z
 *
 * Here some fulltext docs
 *
 * KeyValueParser
 */

namespace solidity::liblangutil
{

// auto KeyValueParser::parse(string_view _input) -> variant<Result, KeyValueParserError>
variant<KeyValueParser::Result, KeyValueParserError> KeyValueParser::parse(string_view _input)
{
	// StructuredDocumentation ::= MultilineText? TaggedValue*
	m_text = _input;

	m_parsedData.untagged = multilineText().value_or(""sv);

	while (!eos())
	{
		if (auto const item = taggedValue(); item.has_value())
			m_parsedData.taggedValues.emplace(item.value());
	}

	// TODO: error handling :-)

	return m_parsedData;
}

optional<pair<string_view, string_view>> KeyValueParser::taggedValue()
{
	// TaggedValue ::= SP* '@' NAME SP+ MultilineText
	return nullopt;
}

optional<string_view> KeyValueParser::multilineText()
{
	// MultilineText ::= TextLine NL TextLineContinuation*
	return nullopt;
}

optional<string_view> KeyValueParser::textLine()
{
	// TextLine ::= [^@] <text except NL>
	return nullopt;
}

optional<string_view> KeyValueParser::textLineContinuation()
{
	// TextLineContinuation ::= SP* TextLine NL
	return nullopt;
}

optional<string_view> KeyValueParser::tagName()
{
	// NAME ::= [A-Za-z0-9_-]+
	return nullopt;
}

}
