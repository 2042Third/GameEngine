#include "FeatureTest/SDKReader.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace Strata::Tests::SDKReader
{

	namespace
	{

		////////////////////////////////////////////////////////////////////////////////
		// Tokens
		////////////////////////////////////////////////////////////////////////////////

		bool IsIdentifierStart(char character)
		{
			return std::isalpha(static_cast<unsigned char>(character)) || character == '_';
		}

		bool IsIdentifierPart(char character)
		{
			return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
		}

		bool IsDigit(char character)
		{
			return std::isdigit(static_cast<unsigned char>(character)) != 0;
		}

		// Words that never name a type, variable or function.
		bool IsKeyword(std::string_view text)
		{
			static const std::set<std::string_view> s_Keywords = { "alignas", "alignof", "and", "asm", "break", "case", "catch", "co_await",
				"co_return", "co_yield", "concept", "const_cast", "continue", "decltype", "default", "delete", "do", "dynamic_cast", "else",
				"false", "for", "friend", "goto", "if", "namespace", "new", "noexcept", "not", "nullptr", "operator", "or", "private",
				"protected", "public", "reinterpret_cast", "requires", "return", "sizeof", "static_assert", "static_cast", "switch",
				"template", "this", "throw", "true", "try", "typedef", "typeid", "using", "while", "xor" };
			return s_Keywords.contains(text);
		}

		// Words that may precede a type in a declaration without being part of its name.
		bool IsSpecifier(std::string_view text)
		{
			static const std::set<std::string_view> s_Specifiers = { "const", "volatile", "static", "inline", "constexpr", "consteval",
				"constinit", "virtual", "explicit", "typename", "mutable", "extern", "thread_local", "struct", "class", "enum", "union" };
			return s_Specifiers.contains(text);
		}

		// Moves index past the quoted literal starting at it. An unterminated literal ends at the end of its line.
		size_t SkipQuoted(std::string_view source, size_t index)
		{
			const char quote = source[index++];
			while (index < source.size())
			{
				if (source[index] == quote)
					return index + 1;
				if (source[index] == '\n')
					return index;
				index += source[index] == '\\' ? 2 : 1;
			}
			return source.size();
		}

		bool IsRawStringPrefix(std::string_view word)
		{
			return word == "R" || word == "LR" || word == "uR" || word == "UR" || word == "u8R";
		}

		bool IsLiteralPrefix(std::string_view word)
		{
			return word == "L" || word == "u" || word == "U" || word == "u8";
		}

		void TokenizeCode(std::string_view code, std::vector<Token>& out)
		{
			int32_t line = 1;
			size_t index = 0;
			while (index < code.size())
			{
				const char character = code[index];
				const char next = index + 1 < code.size() ? code[index + 1] : '\0';
				if (character == '\n')
				{
					line++;
					index++;
				}
				else if (std::isspace(static_cast<unsigned char>(character)) || character == '\\')
				{
					index++;
				}
				else if (IsIdentifierStart(character) || IsDigit(character) || (character == '.' && IsDigit(next)))
				{
					const bool identifier = IsIdentifierStart(character);
					const size_t start = index;
					while (index < code.size() && (IsIdentifierPart(code[index]) || (!identifier && code[index] == '.')))
						index++;
					out.push_back({ std::string(code.substr(start, index - start)), identifier, line });
				}
				else if ((character == ':' && next == ':') || (character == '-' && next == '>') || (character == '#' && next == '#'))
				{
					out.push_back({ std::string(code.substr(index, 2)), false, line });
					index += 2;
				}
				else
				{
					out.push_back({ std::string(1, character), false, line });
					index++;
				}
			}
		}

		SourceTokens TokenizeSource(std::string_view source, bool directives);

		// The macro of a "#define" line (continuations joined); nullopt for other directives.
		std::optional<std::pair<std::string, MacroDefinition>> ParseDirective(std::string_view directive)
		{
			size_t position = directive.find_first_not_of(" \t", 1);
			if (position == std::string_view::npos || directive.substr(position, 6) != "define")
				return std::nullopt;
			position = directive.find_first_not_of(" \t", position + 6);
			if (position == std::string_view::npos || !IsIdentifierStart(directive[position]))
				return std::nullopt;

			size_t end = position;
			while (end < directive.size() && IsIdentifierPart(directive[end]))
				end++;
			std::string name(directive.substr(position, end - position));

			MacroDefinition macro;
			if (end < directive.size() && directive[end] == '(')
			{
				const size_t close = directive.find(')', end);
				if (close == std::string_view::npos)
					return std::nullopt;
				macro.FunctionLike = true;
				std::string_view parameters = directive.substr(end + 1, close - end - 1);
				while (!parameters.empty())
				{
					const size_t comma = parameters.find(',');
					std::string_view parameter = parameters.substr(0, comma);
					const size_t first = parameter.find_first_not_of(" \t");
					const size_t last = parameter.find_last_not_of(" \t");
					if (first != std::string_view::npos)
					{
						parameter = parameter.substr(first, last - first + 1);
						macro.Parameters.push_back(parameter == "..." ? std::string("__VA_ARGS__") : std::string(parameter));
					}
					parameters = comma == std::string_view::npos ? std::string_view() : parameters.substr(comma + 1);
				}
				end = close + 1;
			}
			// A body starting with '#' (stringizing) is no directive.
			macro.Body = TokenizeSource(directive.substr(end), false).Tokens;
			for (Token& token : macro.Body)
				token.Line = 0;
			return std::make_pair(std::move(name), std::move(macro));
		}

		SourceTokens TokenizeSource(std::string_view source, bool directives)
		{
			// First pass: comments, literals and directives out (newlines kept for line numbers), numbers normalized.
			SourceTokens result;
			std::string code;
			code.reserve(source.size());
			bool lineStart = true;
			size_t index = 0;
			while (index < source.size())
			{
				const char character = source[index];
				const char next = index + 1 < source.size() ? source[index + 1] : '\0';
				if (character == '/' && next == '/')
				{
					while (index < source.size() && source[index] != '\n')
						index++;
				}
				else if (character == '/' && next == '*')
				{
					const size_t end = source.find("*/", index + 2);
					const size_t stop = end == std::string_view::npos ? source.size() : end + 2;
					code += ' ';
					code.append(static_cast<size_t>(std::count(source.begin() + static_cast<std::ptrdiff_t>(index), source.begin() + static_cast<std::ptrdiff_t>(stop), '\n')), '\n');
					index = stop;
				}
				else if (character == '#' && lineStart && directives)
				{
					// A directive runs to the end of the line, including backslash continuations.
					std::string directive;
					size_t newlines = 0;
					while (index < source.size() && source[index] != '\n')
					{
						if (source[index] == '\\' && index + 1 < source.size() && (source[index + 1] == '\n' || (source[index + 1] == '\r' && index + 2 < source.size() && source[index + 2] == '\n')))
						{
							directive += ' ';
							index += source[index + 1] == '\r' ? 3 : 2;
							newlines++;
							continue;
						}
						if (source[index] == '/' && index + 1 < source.size() && source[index + 1] == '/')
						{
							while (index < source.size() && source[index] != '\n')
								index++;
							break;
						}
						directive += source[index++];
					}
					code.append(newlines, '\n');
					if (auto macro = ParseDirective(directive))
						result.Macros[macro->first] = std::move(macro->second);
				}
				else if (character == '"' || character == '\'')
				{
					index = SkipQuoted(source, index);
					code += " 0 "; // A literal is an expression token
					lineStart = false;
				}
				else if (IsIdentifierStart(character))
				{
					size_t end = index;
					while (end < source.size() && IsIdentifierPart(source[end]))
						end++;
					const std::string_view word = source.substr(index, end - index);
					if (end < source.size() && source[end] == '"' && IsRawStringPrefix(word))
					{
						// R"delimiter( ... )delimiter"
						const size_t open = source.find('(', end);
						const std::string terminator = ")" + std::string(open == std::string_view::npos ? std::string_view() : source.substr(end + 1, open - end - 1)) + "\"";
						const size_t close = open == std::string_view::npos ? std::string_view::npos : source.find(terminator, open);
						const size_t stop = close == std::string_view::npos ? source.size() : close + terminator.size();
						code += " 0 ";
						code.append(static_cast<size_t>(std::count(source.begin() + static_cast<std::ptrdiff_t>(index), source.begin() + static_cast<std::ptrdiff_t>(stop), '\n')), '\n');
						index = stop;
					}
					else if (end < source.size() && (source[end] == '"' || source[end] == '\'') && IsLiteralPrefix(word))
					{
						index = SkipQuoted(source, end);
						code += " 0 ";
					}
					else
					{
						code += word;
						index = end;
					}
					lineStart = false;
				}
				else if (IsDigit(character) || (character == '.' && IsDigit(next)))
				{
					// A number, with digit separators (1'000) removed and exponent signs kept (1e+5).
					while (index < source.size())
					{
						const char part = source[index];
						if (IsIdentifierPart(part) || part == '.')
						{
							code += part;
							index++;
						}
						else if (part == '\'' && index + 1 < source.size() && IsIdentifierPart(source[index + 1]))
						{
							index++;
						}
						else if ((part == '+' || part == '-') && !code.empty() && (code.back() == 'e' || code.back() == 'E' || code.back() == 'p' || code.back() == 'P'))
						{
							code += part;
							index++;
						}
						else
						{
							break;
						}
					}
					lineStart = false;
				}
				else
				{
					if (character == '\n')
						lineStart = true;
					else if (!std::isspace(static_cast<unsigned char>(character)))
						lineStart = false;
					code += character;
					index++;
				}
			}
			TokenizeCode(code, result.Tokens);
			return result;
		}

		// Index of the token opening the bracket that tokens[close] closes, searching back to `lower`.
		std::optional<size_t> MatchBackward(const std::vector<Token>& tokens, size_t close, std::string_view open, size_t lower)
		{
			const std::string& closeText = tokens[close].Text;
			int32_t depth = 0;
			for (size_t index = close + 1; index-- > lower;)
			{
				const std::string& text = tokens[index].Text;
				if (text == closeText)
					depth++;
				else if (text == open && --depth == 0)
					return index;
			}
			return std::nullopt;
		}

		// Index of the token closing the bracket tokens[open] opens. Angle brackets inside parentheses do not count.
		std::optional<size_t> MatchForward(const std::vector<Token>& tokens, size_t open, std::string_view close)
		{
			const std::string& openText = tokens[open].Text;
			const bool angles = openText == "<";
			int32_t depth = 0;
			int32_t parentheses = 0;
			for (size_t index = open; index < tokens.size(); index++)
			{
				const std::string& text = tokens[index].Text;
				if (angles && text == "(")
					parentheses++;
				else if (angles && text == ")")
					parentheses--;
				else if (parentheses > 0)
					continue;
				else if (text == openText)
					depth++;
				else if (text == close && --depth == 0)
					return index;
			}
			return std::nullopt;
		}

		// [begin, end) split at commas outside brackets.
		std::vector<std::pair<size_t, size_t>> SplitTopLevel(const std::vector<Token>& tokens, size_t begin, size_t end)
		{
			std::vector<std::pair<size_t, size_t>> parts;
			int32_t depth = 0;
			size_t start = begin;
			for (size_t index = begin; index < end; index++)
			{
				const std::string& text = tokens[index].Text;
				if (text == "(" || text == "[" || text == "{" || text == "<")
					depth++;
				else if (text == ")" || text == "]" || text == "}" || text == ">")
					depth--;
				else if (text == "," && depth == 0)
				{
					parts.emplace_back(start, index);
					start = index + 1;
				}
			}
			if (start < end)
				parts.emplace_back(start, end);
			return parts;
		}

		// Skips an attribute specifier ("[[...]]" or "alignas(...)") starting at index; returns the index after it, or index.
		size_t SkipAttribute(const std::vector<Token>& tokens, size_t index, size_t end)
		{
			if (index + 1 < end && tokens[index].Text == "[" && tokens[index + 1].Text == "[")
			{
				const std::optional<size_t> close = MatchForward(tokens, index, "]");
				return close && *close < end ? *close + 1 : index;
			}
			if (index + 1 < end && (tokens[index].Text == "alignas" || tokens[index].Text == "__declspec" || tokens[index].Text == "__attribute__")
				&& tokens[index + 1].Text == "(")
			{
				const std::optional<size_t> close = MatchForward(tokens, index + 1, ")");
				return close && *close < end ? *close + 1 : index;
			}
			return index;
		}

		////////////////////////////////////////////////////////////////////////////////
		// Types
		////////////////////////////////////////////////////////////////////////////////

		TypeRef ParseType(const std::vector<Token>& tokens, size_t begin, size_t end)
		{
			while (begin < end && (IsSpecifier(tokens[begin].Text) || SkipAttribute(tokens, begin, end) != begin))
				begin = IsSpecifier(tokens[begin].Text) ? begin + 1 : SkipAttribute(tokens, begin, end);
			bool pointer = false;
			while (end > begin && (tokens[end - 1].Text == "&" || tokens[end - 1].Text == "*" || tokens[end - 1].Text == "const" || tokens[end - 1].Text == "volatile"))
			{
				if (tokens[end - 1].Text == "*")
				{
					if (pointer)
						return {};
					pointer = true;
				}
				end--;
			}
			if (begin >= end)
				return {};

			size_t nameEnd = end;
			std::optional<std::pair<size_t, size_t>> arguments;
			if (tokens[end - 1].Text == ">")
			{
				const std::optional<size_t> open = MatchBackward(tokens, end - 1, "<", begin);
				if (!open)
					return {};
				arguments = std::make_pair(*open + 1, end - 1);
				nameEnd = *open;
			}
			if (nameEnd == begin || !tokens[nameEnd - 1].Identifier || IsKeyword(tokens[nameEnd - 1].Text))
				return {};
			// The rest is a qualified name: [::] A :: B :: Name.
			for (size_t index = begin; index + 1 < nameEnd; index++)
			{
				const bool separator = (nameEnd - 1 - index) % 2 == 1;
				if (separator ? tokens[index].Text != "::" : !tokens[index].Identifier)
				{
					if (!(index == begin && tokens[index].Text == "::"))
						return {};
				}
			}

			const std::string& name = tokens[nameEnd - 1].Text;
			if (arguments && (name == "vector" || name == "optional"))
			{
				const std::vector<std::pair<size_t, size_t>> parts = SplitTopLevel(tokens, arguments->first, arguments->second);
				if (parts.size() != 1 || pointer)
					return {};
				const TypeRef element = ParseType(tokens, parts[0].first, parts[0].second);
				if (element.Shape != TypeRef::Kind::Value)
					return {};
				return { name == "vector" ? TypeRef::Kind::Vector : TypeRef::Kind::Optional, element.Name };
			}
			return { pointer ? TypeRef::Kind::Pointer : TypeRef::Kind::Value, name };
		}

		TypeRef SubstituteTemplateArguments(TypeRef type, const std::vector<std::string>& parameters, const std::vector<TypeRef>& arguments)
		{
			for (size_t index = 0; index < parameters.size(); index++)
			{
				if (type.Name != parameters[index])
					continue;
				if (index >= arguments.size() || arguments[index].Shape != TypeRef::Kind::Value)
					return {};
				type.Name = arguments[index].Name;
				return type;
			}
			return type;
		}

		////////////////////////////////////////////////////////////////////////////////
		// SDK declarations
		////////////////////////////////////////////////////////////////////////////////

		enum class ScopeKind
		{
			Namespace,
			Class,
			Other
		};

		struct CodeScope
		{
			ScopeKind Kind = ScopeKind::Other;
			std::string Name;
			bool Public = false;
		};

		// The scope a "{" opens, from the tokens [begin, end) of the statement before it.
		CodeScope ClassifyScope(const std::vector<Token>& tokens, size_t begin, size_t end)
		{
			int32_t angleDepth = 0;
			for (size_t index = begin; index < end; index++)
			{
				const std::string& text = tokens[index].Text;
				if (text == "(")
					return { ScopeKind::Other };
				if (text == "<")
					angleDepth++;
				else if (text == ">")
					angleDepth--;
				else if (text == "namespace")
				{
					std::string name;
					for (size_t part = index + 1; part < end; part++)
						name += tokens[part].Text;
					return { ScopeKind::Namespace, name };
				}
				else if (text == "enum")
					return { ScopeKind::Other };
				else if (angleDepth == 0 && (text == "class" || text == "struct" || text == "union") && index + 1 < end && tokens[index + 1].Identifier)
					return { ScopeKind::Class, tokens[index + 1].Text, text != "class" };
			}
			return { ScopeKind::Other };
		}

		// The namespaces enclosing a declaration, "::"-separated, or nullopt when it is not public SDK (outside namespace
		// Strata, or inside a namespace named Detail or an anonymous one).
		std::optional<std::string> GetPublicNamespacePath(const std::vector<CodeScope>& scopes)
		{
			std::vector<std::string> components;
			for (const CodeScope& scope : scopes)
			{
				if (scope.Kind != ScopeKind::Namespace)
					continue;
				if (scope.Name.empty())
					return std::nullopt;
				size_t start = 0;
				while (true)
				{
					const size_t separator = scope.Name.find("::", start);
					components.push_back(scope.Name.substr(start, separator == std::string::npos ? std::string::npos : separator - start));
					if (separator == std::string::npos)
						break;
					start = separator + 2;
				}
			}
			if (components.empty() || components.front() != "Strata" || std::find(components.begin(), components.end(), "Detail") != components.end())
				return std::nullopt;
			std::string path;
			for (size_t index = 1; index < components.size(); index++)
				path += (index > 1 ? "::" : "") + components[index];
			return path;
		}

		// The function a declaration statement [begin, end) declares, if it declares a named function.
		std::optional<SDKFunction> ParseFunctionDeclaration(const std::vector<Token>& tokens, size_t begin, size_t end, const std::string& className)
		{
			SDKFunction function;
			function.Class = className;
			while (begin < end && SkipAttribute(tokens, begin, end) != begin)
				begin = SkipAttribute(tokens, begin, end);
			if (begin + 1 < end && tokens[begin].Text == "template" && tokens[begin + 1].Text == "<")
			{
				const std::optional<size_t> close = MatchForward(tokens, begin + 1, ">");
				if (!close || *close >= end)
					return std::nullopt;
				for (size_t index = begin + 2; index < *close; index++)
				{
					if (tokens[index].Text != "typename" && tokens[index].Text != "class")
						continue;
					size_t name = index + 1;
					while (name < *close && tokens[name].Text == ".")
						name++;
					if (name < *close && tokens[name].Identifier)
						function.TemplateParameters.push_back(tokens[name].Text);
				}
				begin = *close + 1;
			}

			// The declarator: the first name followed by "(" outside brackets, before any initializer.
			int32_t parenDepth = 0;
			int32_t angleDepth = 0;
			for (size_t index = begin; index < end; index++)
			{
				const size_t afterAttribute = SkipAttribute(tokens, index, end);
				if (afterAttribute != index)
				{
					index = afterAttribute - 1;
					continue;
				}
				const Token& token = tokens[index];
				if (token.Text == "(")
					parenDepth++;
				else if (token.Text == ")")
					parenDepth--;
				else if (parenDepth == 0 && token.Text == "<")
					angleDepth++;
				else if (parenDepth == 0 && token.Text == ">")
					angleDepth--;
				else if (parenDepth == 0 && angleDepth == 0 && (token.Text == "=" || token.Text == "operator"))
					return std::nullopt;
				else if (parenDepth == 0 && angleDepth == 0 && token.Text == "static")
					function.Static = true;
				else if (parenDepth == 0 && angleDepth == 0 && token.Text == "virtual")
					function.Virtual = true;

				if (!token.Identifier || parenDepth != 0 || angleDepth != 0 || index + 1 >= end || tokens[index + 1].Text != "(")
					continue;
				if (IsKeyword(token.Text) || token.Text.rfind("__", 0) == 0)
					continue; // decltype(...), noexcept(...), static_assert(...): the parentheses that follow are skipped
				const std::string& previous = index > begin ? tokens[index - 1].Text : std::string();
				if (previous == "~" || previous == "::" || token.Text == className)
					return std::nullopt; // Destructors, out-of-class definitions and constructors
				function.Name = token.Text;
				function.ReturnType = ParseType(tokens, begin, index);

				// "auto Name(...) -> Type"
				if (const std::optional<size_t> close = MatchForward(tokens, index + 1, ")"))
				{
					size_t arrow = *close + 1;
					while (arrow < end && (tokens[arrow].Text == "const" || tokens[arrow].Text == "&" || tokens[arrow].Text == "noexcept"))
						arrow++;
					if (arrow < end && tokens[arrow].Text == "->")
					{
						size_t typeEnd = arrow + 1;
						while (typeEnd < end && tokens[typeEnd].Text != "=" && tokens[typeEnd].Text != "override" && tokens[typeEnd].Text != "final"
							&& tokens[typeEnd].Text != "requires")
							typeEnd++;
						function.ReturnType = ParseType(tokens, arrow + 1, typeEnd);
					}
				}
				return function;
			}
			return std::nullopt;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// Public functions
	////////////////////////////////////////////////////////////////////////////////

	SourceTokens Tokenize(std::string_view source)
	{
		return TokenizeSource(source, true);
	}

	namespace
	{

		std::vector<Token> Expand(const std::vector<Token>& tokens, const MacroTable& macros, std::set<std::string, std::less<>>& active);

		// Arguments of the invocation whose "(" is tokens[open]; returns the index of the closing ")".
		std::optional<size_t> ReadArguments(const std::vector<Token>& tokens, size_t open, std::vector<std::vector<Token>>& arguments)
		{
			arguments.assign(1, {});
			int32_t depth = 0;
			for (size_t index = open + 1; index < tokens.size(); index++)
			{
				const std::string& text = tokens[index].Text;
				if (text == ")" && depth == 0)
				{
					if (arguments.size() == 1 && arguments[0].empty())
						arguments.clear();
					return index;
				}
				if (text == "(")
					depth++;
				else if (text == ")")
					depth--;
				else if (text == "," && depth == 0)
				{
					arguments.emplace_back();
					continue;
				}
				arguments.back().push_back(tokens[index]);
			}
			return std::nullopt;
		}

		std::vector<Token> Substitute(const MacroDefinition& macro, std::vector<std::vector<Token>> arguments, const MacroTable& macros,
			std::set<std::string, std::less<>>& active, int32_t line)
		{
			// Extra arguments of a variadic macro belong to __VA_ARGS__.
			if (!macro.Parameters.empty() && macro.Parameters.back() == "__VA_ARGS__" && arguments.size() > macro.Parameters.size())
			{
				std::vector<Token>& variadic = arguments[macro.Parameters.size() - 1];
				for (size_t index = macro.Parameters.size(); index < arguments.size(); index++)
				{
					variadic.push_back({ ",", false, line });
					variadic.insert(variadic.end(), arguments[index].begin(), arguments[index].end());
				}
				arguments.resize(macro.Parameters.size());
			}
			auto findParameter = [&](const Token& token) -> std::optional<size_t>
			{
				if (!token.Identifier)
					return std::nullopt;
				const auto found = std::find(macro.Parameters.begin(), macro.Parameters.end(), token.Text);
				if (found == macro.Parameters.end())
					return std::nullopt;
				return static_cast<size_t>(found - macro.Parameters.begin());
			};
			auto argument = [&](size_t index) { return index < arguments.size() ? arguments[index] : std::vector<Token>(); };

			std::vector<Token> result;
			const std::vector<Token>& body = macro.Body;
			for (size_t index = 0; index < body.size(); index++)
			{
				const Token& part = body[index];
				if (part.Text == "#" && index + 1 < body.size() && findParameter(body[index + 1]))
				{
					result.push_back({ "0", false, line }); // A stringized argument is a literal
					index++;
					continue;
				}
				if (part.Text == "##" && !result.empty() && index + 1 < body.size())
				{
					const std::optional<size_t> parameter = findParameter(body[index + 1]);
					std::vector<Token> operand = parameter ? argument(*parameter) : std::vector<Token> { body[index + 1] };
					index++;
					if (!operand.empty())
					{
						result.back().Text += operand.front().Text;
						result.insert(result.end(), operand.begin() + 1, operand.end());
					}
					continue;
				}
				if (const std::optional<size_t> parameter = findParameter(part))
				{
					const bool pasted = index + 1 < body.size() && body[index + 1].Text == "##";
					const std::vector<Token> value = pasted ? argument(*parameter) : Expand(argument(*parameter), macros, active);
					result.insert(result.end(), value.begin(), value.end());
					continue;
				}
				Token copy = part;
				copy.Line = line;
				result.push_back(std::move(copy));
			}
			return result;
		}

		std::vector<Token> Expand(const std::vector<Token>& tokens, const MacroTable& macros, std::set<std::string, std::less<>>& active)
		{
			std::vector<Token> out;
			for (size_t index = 0; index < tokens.size(); index++)
			{
				const Token& token = tokens[index];
				const auto macro = token.Identifier && !active.contains(token.Text) ? macros.find(token.Text) : macros.end();
				if (macro == macros.end())
				{
					out.push_back(token);
					continue;
				}

				std::vector<std::vector<Token>> arguments;
				if (macro->second.FunctionLike)
				{
					// The name of a function-like macro without arguments is no invocation.
					std::optional<size_t> close;
					if (index + 1 < tokens.size() && tokens[index + 1].Text == "(")
						close = ReadArguments(tokens, index + 1, arguments);
					if (!close)
					{
						out.push_back(token);
						continue;
					}
					index = *close;
				}
				const std::vector<Token> replacement = Substitute(macro->second, std::move(arguments), macros, active, token.Line);
				active.insert(macro->first);
				const std::vector<Token> expanded = Expand(replacement, macros, active);
				active.erase(macro->first);
				out.insert(out.end(), expanded.begin(), expanded.end());
			}
			return out;
		}

	}

	std::vector<Token> ExpandMacros(const std::vector<Token>& tokens, const MacroTable& macros)
	{
		std::set<std::string, std::less<>> active;
		return Expand(tokens, macros, active);
	}

	std::string SDKFunction::GetQualifiedName() const
	{
		if (!Class.empty())
			return Class + "::" + Name;
		if (!Namespace.empty())
			return Namespace + "::" + Name;
		return Name;
	}

	std::vector<SDKFunction> CollectFunctions(const std::vector<Token>& tokens)
	{
		std::vector<SDKFunction> functions;
		std::vector<CodeScope> scopes;
		size_t statementStart = 0;
		for (size_t index = 0; index < tokens.size(); index++)
		{
			const std::string& text = tokens[index].Text;
			const size_t afterAttribute = SkipAttribute(tokens, index, tokens.size());
			if (afterAttribute != index)
			{
				index = afterAttribute - 1; // "[[" ... "]]" may contain braces or semicolons in strings: never statement ends
				continue;
			}
			CodeScope* scope = scopes.empty() ? nullptr : &scopes.back();
			if (scope && scope->Kind == ScopeKind::Class && index + 1 < tokens.size() && tokens[index + 1].Text == ":"
				&& (text == "public" || text == "private" || text == "protected"))
			{
				scope->Public = text == "public";
				index++;
				statementStart = index + 1;
				continue;
			}
			if (text != "{" && text != "}" && text != ";")
				continue;

			// A statement ended: a declaration directly in a public class section or in a namespace may declare a function.
			const std::optional<std::string> namespacePath = GetPublicNamespacePath(scopes);
			const bool inClass = scope && scope->Kind == ScopeKind::Class && scope->Public;
			const bool inNamespace = scope && scope->Kind == ScopeKind::Namespace;
			if (namespacePath && (inClass || inNamespace))
			{
				if (std::optional<SDKFunction> function = ParseFunctionDeclaration(tokens, statementStart, index, inClass ? scope->Name : std::string()))
				{
					if (!inClass)
						function->Namespace = *namespacePath;
					// Classes nested in other classes count only when every enclosing class section is public.
					const bool hidden = std::any_of(scopes.begin(), scopes.end(), [](const CodeScope& outer) { return outer.Kind == ScopeKind::Class && !outer.Public; });
					if (!hidden)
						functions.push_back(std::move(*function));
				}
			}

			if (text == "{")
				scopes.push_back(ClassifyScope(tokens, statementStart, index));
			else if (text == "}" && !scopes.empty())
				scopes.pop_back();
			statementStart = index + 1;
		}
		return functions;
	}

	std::string NormalizeTypeSpelling(const std::vector<Token>& tokens)
	{
		std::string spelling;
		for (size_t index = 0; index < tokens.size(); index++)
		{
			const std::string& text = tokens[index].Text;
			if (text == "const" || text == "volatile" || text == "&" || text == "typename")
				continue;
			if ((text == "std" || text == "Strata") && index + 1 < tokens.size() && tokens[index + 1].Text == "::")
			{
				index++;
				continue;
			}
			if (text == "::" && spelling.empty())
				continue;
			spelling += text;
		}
		return spelling;
	}

	std::vector<std::string> CollectFieldTypes(const std::vector<Token>& tokens, std::string_view listName)
	{
		std::vector<std::string> types;
		for (size_t index = 0; index + 1 < tokens.size(); index++)
		{
			if (tokens[index].Text != listName || tokens[index + 1].Text != "=")
				continue;
			int32_t depth = 0;
			for (size_t part = index + 2; part < tokens.size(); part++)
			{
				const std::string& text = tokens[part].Text;
				if (text == "(")
					depth++;
				else if (text == ")")
					depth--;
				else if (text == ";" && depth == 0)
					break;
				if (text != "is_same_v" || part + 1 >= tokens.size() || tokens[part + 1].Text != "<")
					continue;
				const std::optional<size_t> close = MatchForward(tokens, part + 1, ">");
				if (!close)
					break;
				const std::vector<std::pair<size_t, size_t>> arguments = SplitTopLevel(tokens, part + 2, *close);
				if (arguments.size() == 2)
				{
					const std::vector<Token> type(tokens.begin() + static_cast<std::ptrdiff_t>(arguments[1].first), tokens.begin() + static_cast<std::ptrdiff_t>(arguments[1].second));
					const std::string spelling = NormalizeTypeSpelling(type);
					if (std::find(types.begin(), types.end(), spelling) == types.end())
						types.push_back(spelling);
				}
				part = *close;
			}
		}
		return types;
	}

	void SDKSurface::AddHeader(std::string_view text)
	{
		SourceTokens tokens = Tokenize(text);
		for (SDKFunction& function : CollectFunctions(tokens.Tokens))
		{
			if (!function.Class.empty())
				Classes.insert(function.Class);
			Functions.push_back(std::move(function));
		}
		for (auto& [name, macro] : tokens.Macros)
		{
			if (name.rfind("ST_SCRIPT_", 0) == 0 && name.rfind("ST_SCRIPT_DETAIL_", 0) != 0)
				PublicMacros.insert(name);
			Macros[name] = std::move(macro);
		}
		for (const std::string& type : CollectFieldTypes(tokens.Tokens, "c_IsFieldType"))
		{
			if (std::find(FieldTypes.begin(), FieldTypes.end(), type) == FieldTypes.end())
				FieldTypes.push_back(type);
		}
	}

	const SDKFunction* SDKSurface::FindFunction(std::string_view className, std::string_view name) const
	{
		for (const SDKFunction& function : Functions)
		{
			if (function.Class == className && function.Name == name)
				return &function;
		}
		return nullptr;
	}

	bool SDKSurface::IsClass(std::string_view name) const
	{
		return Classes.contains(std::string(name));
	}

	const ScriptField* ScriptUsage::FindField(std::string_view className, std::string_view fieldName) const
	{
		// Bases are followed a bounded number of levels, so cyclic declarations cannot loop.
		std::vector<std::string> pending { std::string(className) };
		for (size_t visited = 0; visited < pending.size() && visited < 64; visited++)
		{
			const auto found = Classes.find(pending[visited]);
			if (found == Classes.end())
				continue;
			for (const ScriptField& field : found->second.Fields)
			{
				if (field.Name == fieldName)
					return &field;
			}
			pending.insert(pending.end(), found->second.Bases.begin(), found->second.Bases.end());
		}
		return nullptr;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Script analysis
	////////////////////////////////////////////////////////////////////////////////

	namespace
	{

		// What a member access resolves to.
		struct MemberLookup
		{
			std::string SDKClass; // The SDK class declaring the function; empty for the scripts' own members and std members
			bool Static = false;
			bool Virtual = false;
			TypeRef Type;         // Return type (functions) or type (fields)
			std::vector<std::string> TemplateParameters;
		};

		// The type of an expression and the index of its first token.
		struct ExpressionType
		{
			TypeRef Type;
			size_t Start = 0;
		};

		class ScriptAnalyzer
		{
		public:
			enum class Pass
			{
				Declarations, // Script classes, their fields and functions, free functions
				Uses          // SDK functions the scripts use
			};

			ScriptAnalyzer(const SDKSurface& surface, ScriptUsage& usage)
				: m_Surface(surface), m_Usage(usage)
			{
				for (const SDKFunction& function : surface.Functions)
					m_SDKFunctionNames.insert(function.Name);
			}

			void Walk(const std::vector<Token>& tokens, Pass pass, const std::string& source)
			{
				m_Tokens = &tokens;
				m_Pass = pass;
				m_Source = source;
				m_Scopes.assign(1, Scope { RegionKind::Namespace });
				m_Statement = Statement {};

				for (size_t index = 0; index < tokens.size(); index++)
				{
					const Token& token = tokens[index];
					const std::string& text = token.Text;
					if (text == "(" || text == "[")
						m_Statement.ParenDepth++;
					else if (text == ")" || text == "]")
						m_Statement.ParenDepth = std::max(0, m_Statement.ParenDepth - 1);
					else if (text == "{")
						OpenScope(index);
					else if (text == "}")
						CloseScope(index);
					else if (text == ";" && m_Statement.ParenDepth == 0)
						EndStatement(index);
					else if (text == ":" && m_Scopes.back().Kind == RegionKind::Class && index == m_Statement.Start + 1)
						m_Statement.Start = index + 1; // After an access specifier
					else if (token.Identifier)
					{
						DetectDeclaration(index);
						if (m_Pass == Pass::Uses)
							DetectUse(index);
					}
				}
			}
		private:
			enum class RegionKind
			{
				Namespace,
				Class,
				Function,
				Block,
				Nested, // Braces inside an expression (initializer lists, lambda bodies): the statement around them continues
				Enum
			};

			struct Statement
			{
				size_t Start = 0;
				int32_t ParenDepth = 0;
				std::map<std::string, TypeRef, std::less<>> Pending; // Declared inside parentheses: parameters, for-range variables
				std::vector<std::pair<std::string, size_t>> AutoDeclarations; // "auto name =": name and the index of "="
			};

			struct Scope
			{
				RegionKind Kind = RegionKind::Namespace;
				std::string ClassContext; // The script class whose members are visible here
				std::map<std::string, TypeRef, std::less<>> Variables;
				Statement Saved;          // Nested scopes: the enclosing statement
			};

			const std::vector<Token>& GetTokens() const { return *m_Tokens; }

			bool IsScriptClass(std::string_view name) const { return m_Usage.Classes.contains(name); }

			void OpenScope(size_t index)
			{
				const std::vector<Token>& tokens = GetTokens();
				Scope scope;
				scope.ClassContext = m_Scopes.back().ClassContext;
				const std::string previous = index > 0 ? tokens[index - 1].Text : std::string();
				bool nested = m_Statement.ParenDepth > 0 || previous == "=" || previous == "," || previous == "(" || previous == "{" || previous == "return"
					|| previous == "[" || previous == "?" || (m_Scopes.back().Kind == RegionKind::Nested && index == m_Statement.Start);
				if (!nested)
				{
					const size_t begin = m_Statement.Start;
					const CodeScope header = ClassifyScope(tokens, begin, index);
					const std::string first = begin < index ? tokens[begin].Text : std::string();
					const bool hasParentheses = std::any_of(tokens.begin() + static_cast<std::ptrdiff_t>(begin), tokens.begin() + static_cast<std::ptrdiff_t>(index),
						[](const Token& token) { return token.Text == "("; });
					if (header.Kind == ScopeKind::Namespace)
					{
						scope.Kind = RegionKind::Namespace;
						scope.ClassContext.clear();
					}
					else if (std::any_of(tokens.begin() + static_cast<std::ptrdiff_t>(begin), tokens.begin() + static_cast<std::ptrdiff_t>(index), [](const Token& token) { return token.Text == "enum"; }))
					{
						scope.Kind = RegionKind::Enum;
					}
					else if (header.Kind == ScopeKind::Class)
					{
						scope.Kind = RegionKind::Class;
						scope.ClassContext = header.Name;
						if (m_Pass == Pass::Declarations)
							RegisterClass(header.Name, begin, index);
					}
					else if (begin == index || first == "else" || first == "do" || first == "try")
					{
						scope.Kind = RegionKind::Block;
					}
					else if (hasParentheses && (first == "if" || first == "for" || first == "while" || first == "switch" || first == "catch"))
					{
						scope.Kind = RegionKind::Block;
					}
					else if (hasParentheses && !IsLambda(begin, index))
					{
						scope.Kind = RegionKind::Function;
						if (const std::optional<std::string> owner = GetDefinitionOwner(begin, index))
							scope.ClassContext = *owner;
					}
					else
					{
						nested = true;
					}
				}

				if (nested)
				{
					scope.Kind = RegionKind::Nested;
					scope.Variables = m_Statement.Pending; // Lambda parameters
					scope.Saved = m_Statement;
				}
				else if (scope.Kind == RegionKind::Function || scope.Kind == RegionKind::Block)
				{
					scope.Variables = m_Statement.Pending; // Parameters, for-range and condition variables
				}
				m_Scopes.push_back(std::move(scope));
				m_Statement = Statement {};
				m_Statement.Start = index + 1;
			}

			void CloseScope(size_t index)
			{
				if (m_Scopes.size() <= 1)
				{
					m_Statement = Statement {};
					m_Statement.Start = index + 1;
					return;
				}
				Scope scope = std::move(m_Scopes.back());
				m_Scopes.pop_back();
				if (scope.Kind == RegionKind::Nested)
				{
					m_Statement = std::move(scope.Saved);
					return;
				}
				m_Statement = Statement {};
				m_Statement.Start = index + 1;
			}

			void EndStatement(size_t index)
			{
				// "auto name = <expression>;": the type of the expression, if it is one expression of known type.
				for (const auto& [name, equals] : m_Statement.AutoDeclarations)
				{
					const ExpressionType initializer = TypeOf(index - 1, equals + 1);
					if (initializer.Start == equals + 1 && initializer.Type.IsKnown())
						m_Scopes.back().Variables[name] = initializer.Type;
				}
				m_Statement = Statement {};
				m_Statement.Start = index + 1;
			}

			// "[captures](parameters) {": a lambda whose body is no function scope of its own name.
			bool IsLambda(size_t begin, size_t end) const
			{
				const std::vector<Token>& tokens = GetTokens();
				for (size_t index = begin; index + 1 < end; index++)
				{
					if (tokens[index].Text == "]" && tokens[index + 1].Text == "(")
						return true;
				}
				return false;
			}

			// The class an out-of-class member function definition ("void Game::Player::OnCreate() {") belongs to.
			std::optional<std::string> GetDefinitionOwner(size_t begin, size_t end) const
			{
				const std::vector<Token>& tokens = GetTokens();
				for (size_t index = begin; index + 1 < end; index++)
				{
					if (tokens[index + 1].Text != "(" || !tokens[index].Identifier)
						continue;
					if (index >= begin + 2 && tokens[index - 1].Text == "::" && tokens[index - 2].Identifier && IsScriptClass(tokens[index - 2].Text))
						return tokens[index - 2].Text;
					return std::nullopt;
				}
				return std::nullopt;
			}

			void RegisterClass(const std::string& name, size_t begin, size_t end)
			{
				const std::vector<Token>& tokens = GetTokens();
				ScriptClass& scriptClass = m_Usage.Classes[name];
				scriptClass.Name = name;
				size_t colon = begin;
				while (colon < end && tokens[colon].Text != ":")
					colon++;
				if (colon >= end)
					return;
				for (const auto& [partBegin, partEnd] : SplitTopLevel(tokens, colon + 1, end))
				{
					size_t typeBegin = partBegin;
					while (typeBegin < partEnd && (tokens[typeBegin].Text == "public" || tokens[typeBegin].Text == "protected" || tokens[typeBegin].Text == "private"
						|| tokens[typeBegin].Text == "virtual"))
						typeBegin++;
					const TypeRef base = ParseType(tokens, typeBegin, partEnd);
					if (base.Shape == TypeRef::Kind::Value && std::find(scriptClass.Bases.begin(), scriptClass.Bases.end(), base.Name) == scriptClass.Bases.end())
						scriptClass.Bases.push_back(base.Name);
				}
			}

			// The first token of the type before the declared name at nameIndex, if the tokens before it form one.
			std::optional<size_t> FindTypeBegin(size_t nameIndex) const
			{
				const std::vector<Token>& tokens = GetTokens();
				const size_t lower = m_Statement.Start;
				size_t index = nameIndex;
				while (index > lower && (tokens[index - 1].Text == "*" || tokens[index - 1].Text == "&" || tokens[index - 1].Text == "const"))
					index--;
				if (index == lower)
					return std::nullopt;
				if (tokens[index - 1].Text == ">")
				{
					const std::optional<size_t> open = MatchBackward(tokens, index - 1, "<", lower);
					if (!open)
						return std::nullopt;
					index = *open;
				}
				if (index == lower || !tokens[index - 1].Identifier || IsKeyword(tokens[index - 1].Text) || IsSpecifier(tokens[index - 1].Text))
					return std::nullopt;
				index--;
				while (index >= lower + 2 && tokens[index - 1].Text == "::" && tokens[index - 2].Identifier)
					index -= 2;
				if (index > lower && tokens[index - 1].Text == "::")
					index--;
				while (index > lower && IsSpecifier(tokens[index - 1].Text))
					index--;
				// A declaration starts the statement, or follows "(", ",", "<", ">" (template headers) or an attribute.
				if (index > lower)
				{
					const std::string& before = tokens[index - 1].Text;
					if (before != "(" && before != "," && before != "<" && before != ">" && before != "]")
						return std::nullopt;
				}
				return index;
			}

			void DetectDeclaration(size_t index)
			{
				const std::vector<Token>& tokens = GetTokens();
				if (index + 1 >= tokens.size() || IsKeyword(tokens[index].Text) || IsSpecifier(tokens[index].Text))
					return;
				const std::string& next = tokens[index + 1].Text;
				const bool function = next == "(";
				const bool variable = next == "=" || next == ";" || next == "," || next == ")" || next == "{" || next == "[" || next == ":";
				if (!function && !variable)
					return;
				const std::optional<size_t> typeBegin = FindTypeBegin(index);
				if (!typeBegin)
					return;
				const TypeRef type = ParseType(tokens, *typeBegin, index);
				if (!type.IsKnown())
					return;

				const std::string& name = tokens[index].Text;
				Scope& scope = m_Scopes.back();
				if (function)
				{
					if (m_Statement.ParenDepth != 0)
						return;
					if (scope.Kind == RegionKind::Class)
					{
						if (m_Pass == Pass::Declarations && name != scope.ClassContext)
							m_Usage.Classes[scope.ClassContext].Functions[name] = type;
					}
					else if (scope.Kind == RegionKind::Namespace)
					{
						if (m_Pass == Pass::Declarations)
							m_FreeFunctions[name] = type;
					}
					else
					{
						scope.Variables[name] = type; // "Type name(arguments);" in a function body
					}
					return;
				}

				if (m_Statement.ParenDepth > 0)
				{
					m_Statement.Pending[name] = type;
				}
				else if (scope.Kind == RegionKind::Class)
				{
					if (m_Pass == Pass::Declarations)
					{
						ScriptClass& owner = m_Usage.Classes[scope.ClassContext];
						size_t spellingBegin = *typeBegin;
						while (spellingBegin < index && IsSpecifier(tokens[spellingBegin].Text))
							spellingBegin++;
						std::vector<Token> spelling(tokens.begin() + static_cast<std::ptrdiff_t>(spellingBegin), tokens.begin() + static_cast<std::ptrdiff_t>(index));
						owner.Fields.push_back({ name, NormalizeTypeSpelling(spelling), type, HasStaticSpecifier(index) });
					}
				}
				else if (scope.Kind != RegionKind::Enum)
				{
					scope.Variables[name] = type;
					if (type.Shape == TypeRef::Kind::Value && type.Name == "auto" && next == "=")
						m_Statement.AutoDeclarations.emplace_back(name, index + 1);
				}
			}

			bool HasStaticSpecifier(size_t index) const
			{
				const std::vector<Token>& tokens = GetTokens();
				for (size_t part = m_Statement.Start; part < index; part++)
				{
					if (tokens[part].Text == "static")
						return true;
				}
				return false;
			}

			bool InFunctionBody() const
			{
				const RegionKind kind = m_Scopes.back().Kind;
				return kind == RegionKind::Function || kind == RegionKind::Block || kind == RegionKind::Nested;
			}

			void Use(const std::string& qualifiedName)
			{
				m_Usage.Functions.insert(qualifiedName);
			}

			void DetectUse(size_t index)
			{
				const std::vector<Token>& tokens = GetTokens();
				const std::string& name = tokens[index].Text;
				if (name == "override")
				{
					DetectOverride(index);
					return;
				}
				if (!m_SDKFunctionNames.contains(name))
					return;
				const std::string previous = index > 0 ? tokens[index - 1].Text : std::string();
				const bool call = index + 1 < tokens.size() && (tokens[index + 1].Text == "(" || tokens[index + 1].Text == "<");

				// receiver.Name, receiver->Name, receiver.template Name
				size_t access = index - 1;
				if (previous == "template" && index >= 2)
					access = index - 2;
				if (index > m_Statement.Start && access >= m_Statement.Start && (tokens[access].Text == "." || tokens[access].Text == "->"))
				{
					const ExpressionType receiver = access > m_Statement.Start ? TypeOf(access - 1, m_Statement.Start) : ExpressionType {};
					if (!receiver.Type.IsKnown())
					{
						m_Usage.UntypedAccesses.push_back({ name, m_Source, tokens[index].Line });
						return;
					}
					const std::optional<MemberLookup> member = FindMember(receiver.Type, name);
					if (member && !member->SDKClass.empty() && !member->Static)
						Use(member->SDKClass + "::" + name);
					return;
				}

				// Qualifier::Name
				if (previous == "::" && index >= 2 && tokens[index - 2].Identifier)
				{
					const std::string& qualifier = tokens[index - 2].Text;
					if (m_Surface.FindFunction(qualifier, name))
					{
						Use(qualifier + "::" + name);
					}
					else if (IsScriptClass(qualifier))
					{
						if (const std::optional<MemberLookup> member = FindMember({ TypeRef::Kind::Value, qualifier }, name); member && !member->SDKClass.empty())
							Use(member->SDKClass + "::" + name);
					}
					else if (const SDKFunction* function = FindFreeFunction(qualifier, name))
					{
						Use(function->GetQualifiedName());
					}
					return;
				}

				// An unqualified call in a function body: an inherited SDK member function or a free function of namespace Strata.
				if (!call || !InFunctionBody() || previous == "." || previous == "->")
					return;
				const std::string& context = m_Scopes.back().ClassContext;
				if (!context.empty() && IsScriptClass(context))
				{
					if (const std::optional<MemberLookup> member = FindMember({ TypeRef::Kind::Value, context }, name))
					{
						if (!member->SDKClass.empty() && !member->Static)
							Use(member->SDKClass + "::" + name);
						return;
					}
				}
				if (!m_FreeFunctions.contains(name))
				{
					if (const SDKFunction* function = FindFreeFunction("Strata", name))
						Use(function->GetQualifiedName());
				}
			}

			// "Name(...) ... override" in a class body: the SDK virtual function it overrides is used.
			void DetectOverride(size_t index)
			{
				const std::vector<Token>& tokens = GetTokens();
				const Scope& scope = m_Scopes.back();
				if (scope.Kind != RegionKind::Class || !IsScriptClass(scope.ClassContext))
					return;
				for (size_t part = m_Statement.Start; part + 1 < index; part++)
				{
					if (!tokens[part].Identifier || tokens[part + 1].Text != "(")
						continue;
					const ScriptClass& owner = m_Usage.Classes.find(scope.ClassContext)->second;
					for (const std::string& base : owner.Bases)
					{
						if (const std::optional<MemberLookup> member = FindMember({ TypeRef::Kind::Value, base }, tokens[part].Text); member && member->Virtual)
							Use(member->SDKClass + "::" + tokens[part].Text);
					}
					return;
				}
			}

			// A free SDK function named `name` in the namespace `qualifier` ("Strata" for namespace Strata itself).
			const SDKFunction* FindFreeFunction(std::string_view qualifier, std::string_view name) const
			{
				for (const SDKFunction& function : m_Surface.Functions)
				{
					if (!function.Class.empty() || function.Name != name)
						continue;
					const std::string_view innermost = function.Namespace.empty() ? std::string_view("Strata")
						: std::string_view(function.Namespace).substr(function.Namespace.rfind("::") == std::string::npos ? 0 : function.Namespace.rfind("::") + 2);
					if (innermost == qualifier)
						return &function;
				}
				return nullptr;
			}

			std::optional<MemberLookup> FindMember(const TypeRef& receiver, std::string_view name, int32_t depth = 0) const
			{
				if (depth > 32)
					return std::nullopt;
				switch (receiver.Shape)
				{
					case TypeRef::Kind::Optional:
						if (name == "value" || name == "value_or")
							return MemberLookup { {}, false, false, { TypeRef::Kind::Value, receiver.Name }, {} };
						return std::nullopt;
					case TypeRef::Kind::Vector:
						if (name == "front" || name == "back" || name == "at")
							return MemberLookup { {}, false, false, { TypeRef::Kind::Value, receiver.Name }, {} };
						return std::nullopt;
					case TypeRef::Kind::Value:
					case TypeRef::Kind::Pointer:
						break;
					case TypeRef::Kind::Unknown:
						return std::nullopt;
				}

				// SDK classes: overloads agree on the return type, or it is unknown.
				std::optional<MemberLookup> found;
				for (const SDKFunction& function : m_Surface.Functions)
				{
					if (function.Class != receiver.Name || function.Name != name)
						continue;
					if (!found)
					{
						found = MemberLookup { function.Class, function.Static, function.Virtual, function.ReturnType, function.TemplateParameters };
						continue;
					}
					found->Static = found->Static && function.Static;
					found->Virtual = found->Virtual || function.Virtual;
					if (found->Type != function.ReturnType)
						found->Type = {};
				}
				if (found)
					return found;

				const auto scriptClass = m_Usage.Classes.find(receiver.Name);
				if (scriptClass == m_Usage.Classes.end())
					return std::nullopt;
				for (const ScriptField& field : scriptClass->second.Fields)
				{
					if (field.Name == name)
						return MemberLookup { {}, field.Static, false, field.Type, {} };
				}
				if (const auto function = scriptClass->second.Functions.find(name); function != scriptClass->second.Functions.end())
					return MemberLookup { {}, false, false, function->second, {} };
				for (const std::string& base : scriptClass->second.Bases)
				{
					if (std::optional<MemberLookup> inherited = FindMember({ TypeRef::Kind::Value, base }, name, depth + 1))
						return inherited;
				}
				return std::nullopt;
			}

			TypeRef LookupVariable(std::string_view name) const
			{
				if (const auto pending = m_Statement.Pending.find(name); pending != m_Statement.Pending.end())
					return pending->second;
				for (auto scope = m_Scopes.rbegin(); scope != m_Scopes.rend(); ++scope)
				{
					if (scope->Kind == RegionKind::Namespace)
						break;
					if (const auto variable = scope->Variables.find(name); variable != scope->Variables.end())
						return variable->second;
				}
				const std::string& context = m_Scopes.back().ClassContext;
				if (const ScriptField* field = context.empty() ? nullptr : m_Usage.FindField(context, name))
					return field->Type;
				for (auto scope = m_Scopes.rbegin(); scope != m_Scopes.rend(); ++scope)
				{
					if (scope->Kind != RegionKind::Namespace)
						continue;
					if (const auto variable = scope->Variables.find(name); variable != scope->Variables.end())
						return variable->second;
				}
				return {};
			}

			// The first token of a qualified name "A::B::C" ending at end.
			size_t QualifiedStart(size_t end, size_t lower) const
			{
				const std::vector<Token>& tokens = GetTokens();
				size_t start = end;
				while (start >= lower + 2 && tokens[start - 1].Text == "::" && tokens[start - 2].Identifier)
					start -= 2;
				if (start > lower && tokens[start - 1].Text == "::")
					start--;
				return start;
			}

			TypeRef ReturnTypeOfCall(const std::string& callee, const std::vector<TypeRef>& templateArguments) const
			{
				if (m_Surface.IsClass(callee) || IsScriptClass(callee))
					return { TypeRef::Kind::Value, callee }; // A constructor
				const std::string& context = m_Scopes.back().ClassContext;
				if (!context.empty() && IsScriptClass(context))
				{
					if (const std::optional<MemberLookup> member = FindMember({ TypeRef::Kind::Value, context }, callee))
						return SubstituteTemplateArguments(member->Type, member->TemplateParameters, templateArguments);
				}
				if (const auto function = m_FreeFunctions.find(callee); function != m_FreeFunctions.end())
					return function->second;
				if (const SDKFunction* function = FindFreeFunction("Strata", callee))
					return SubstituteTemplateArguments(function->ReturnType, function->TemplateParameters, templateArguments);
				return {};
			}

			// The type of the expression whose last token is tokens[end], not reaching before `lower`.
			ExpressionType TypeOf(size_t end, size_t lower) const
			{
				const std::vector<Token>& tokens = GetTokens();
				const ExpressionType unknown { {}, end };
				if (end < lower || end >= tokens.size())
					return unknown;
				const Token& last = tokens[end];
				if (last.Identifier)
				{
					if (last.Text == "this")
						return { m_Scopes.back().ClassContext.empty() ? TypeRef {} : TypeRef { TypeRef::Kind::Pointer, m_Scopes.back().ClassContext }, end };
					if (end >= lower + 2 && (tokens[end - 1].Text == "." || tokens[end - 1].Text == "->"))
					{
						const ExpressionType receiver = TypeOf(end - 2, lower);
						const std::optional<MemberLookup> member = receiver.Type.IsKnown() ? FindMember(receiver.Type, last.Text) : std::nullopt;
						return { member && member->SDKClass.empty() ? member->Type : TypeRef {}, receiver.Start };
					}
					if (end > lower && tokens[end - 1].Text == "::")
						return { {}, QualifiedStart(end, lower) };
					return { LookupVariable(last.Text), end };
				}

				if (last.Text == ")")
				{
					const std::optional<size_t> open = MatchBackward(tokens, end, "(", lower);
					if (!open)
						return unknown;
					if (*open > lower)
					{
						size_t callee = *open - 1;
						std::vector<TypeRef> templateArguments;
						if (tokens[callee].Text == ">")
						{
							const std::optional<size_t> angle = MatchBackward(tokens, callee, "<", lower);
							if (!angle || *angle == lower || !tokens[*angle - 1].Identifier)
								return { {}, *open };
							for (const auto& [argumentBegin, argumentEnd] : SplitTopLevel(tokens, *angle + 1, callee))
								templateArguments.push_back(ParseType(tokens, argumentBegin, argumentEnd));
							callee = *angle - 1;
						}
						const std::string& calleeName = tokens[callee].Text;
						if (calleeName == "static_cast" || calleeName == "const_cast" || calleeName == "reinterpret_cast" || calleeName == "dynamic_cast")
							return { templateArguments.empty() ? TypeRef {} : templateArguments.front(), callee };
						if (tokens[callee].Identifier && !IsKeyword(calleeName))
						{
							size_t access = callee - 1;
							if (callee > lower && tokens[access].Text == "template" && access > lower)
								access--;
							if (callee > lower && (tokens[access].Text == "." || tokens[access].Text == "->"))
							{
								if (access == lower)
									return { {}, lower };
								const ExpressionType receiver = TypeOf(access - 1, lower);
								if (!receiver.Type.IsKnown())
									return { {}, receiver.Start };
								const std::optional<MemberLookup> member = FindMember(receiver.Type, calleeName);
								if (!member || member->Static)
									return { {}, receiver.Start };
								return { SubstituteTemplateArguments(member->Type, member->TemplateParameters, templateArguments), receiver.Start };
							}
							if (callee >= lower + 2 && tokens[callee - 1].Text == "::" && tokens[callee - 2].Identifier)
							{
								const std::string& qualifier = tokens[callee - 2].Text;
								const size_t start = QualifiedStart(callee, lower);
								if (m_Surface.FindFunction(qualifier, calleeName) || IsScriptClass(qualifier))
								{
									const std::optional<MemberLookup> member = FindMember({ TypeRef::Kind::Value, qualifier }, calleeName);
									return { member ? SubstituteTemplateArguments(member->Type, member->TemplateParameters, templateArguments) : TypeRef {}, start };
								}
								if (const SDKFunction* function = FindFreeFunction(qualifier, calleeName))
									return { SubstituteTemplateArguments(function->ReturnType, function->TemplateParameters, templateArguments), start };
								return { { TypeRef::Kind::Value, calleeName }, start }; // A constructor such as Strata::Entity() or std::string()
							}
							return { ReturnTypeOfCall(calleeName, templateArguments), callee };
						}
						if (tokens[callee].Identifier || tokens[callee].Text == ")" || tokens[callee].Text == "]")
							return { {}, callee }; // Keywords, or calls of call results
					}
					// Grouping parentheses around one expression.
					if (*open + 1 < end)
					{
						const ExpressionType inner = TypeOf(end - 1, *open + 1);
						if (inner.Start == *open + 1)
							return { inner.Type, *open };
					}
					return { {}, *open };
				}

				if (last.Text == "]")
				{
					const std::optional<size_t> open = MatchBackward(tokens, end, "[", lower);
					if (!open || *open == lower)
						return unknown;
					const ExpressionType container = TypeOf(*open - 1, lower);
					if (container.Type.Shape == TypeRef::Kind::Vector)
						return { { TypeRef::Kind::Value, container.Type.Name }, container.Start };
					return { {}, container.Start };
				}
				return unknown;
			}
		private:
			const SDKSurface& m_Surface;
			ScriptUsage& m_Usage;
			std::set<std::string, std::less<>> m_SDKFunctionNames;
			std::map<std::string, TypeRef, std::less<>> m_FreeFunctions; // The scripts' own free functions

			const std::vector<Token>* m_Tokens = nullptr;
			Pass m_Pass = Pass::Declarations;
			std::string m_Source;
			std::vector<Scope> m_Scopes;
			Statement m_Statement;
		};

		// ST_SCRIPT_CLASS(Name) { ST_SCRIPT_FIELD(Member); ... } in tokens before macro expansion.
		void CollectRegistrations(const std::vector<Token>& tokens, std::vector<ScriptRegistration>& out)
		{
			for (size_t index = 0; index + 1 < tokens.size(); index++)
			{
				if (tokens[index].Text != "ST_SCRIPT_CLASS" || tokens[index + 1].Text != "(")
					continue;
				const std::optional<size_t> close = MatchForward(tokens, index + 1, ")");
				if (!close || *close + 1 >= tokens.size() || tokens[*close + 1].Text != "{")
					continue;
				ScriptRegistration registration;
				for (size_t part = index + 2; part < *close; part++)
					registration.ClassName += tokens[part].Text;
				const std::optional<size_t> bodyEnd = MatchForward(tokens, *close + 1, "}");
				const size_t end = bodyEnd ? *bodyEnd : tokens.size();
				for (size_t part = *close + 2; part + 3 < end; part++)
				{
					if (tokens[part].Text == "ST_SCRIPT_FIELD" && tokens[part + 1].Text == "(" && tokens[part + 2].Identifier && tokens[part + 3].Text == ")")
						registration.Fields.push_back(tokens[part + 2].Text);
				}
				out.push_back(std::move(registration));
				index = end;
			}
		}

	}

	ScriptUsage AnalyzeScripts(const SDKSurface& surface, const std::vector<ScriptSource>& sources)
	{
		ScriptUsage usage;
		std::vector<std::vector<Token>> expanded;
		for (const ScriptSource& source : sources)
		{
			SourceTokens tokens = Tokenize(source.Text);
			for (const Token& token : tokens.Tokens)
			{
				if (token.Identifier)
					usage.Identifiers.insert(token.Text);
			}
			CollectRegistrations(tokens.Tokens, usage.Registrations);
			MacroTable macros = surface.Macros;
			for (auto& [name, macro] : tokens.Macros)
				macros[name] = std::move(macro);
			expanded.push_back(ExpandMacros(tokens.Tokens, macros));
			for (const Token& token : expanded.back())
			{
				if (token.Identifier)
					usage.Identifiers.insert(token.Text);
			}
		}

		ScriptAnalyzer analyzer(surface, usage);
		for (size_t index = 0; index < sources.size(); index++)
			analyzer.Walk(expanded[index], ScriptAnalyzer::Pass::Declarations, sources[index].Name);
		for (size_t index = 0; index < sources.size(); index++)
			analyzer.Walk(expanded[index], ScriptAnalyzer::Pass::Uses, sources[index].Name);
		return usage;
	}

}
