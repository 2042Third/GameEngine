#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Reads the script SDK headers and the feature scripts as C++ text for the feature test's coverage checks
// (SDKCoverageTests.cpp). It is no compiler: a tokenizer strips comments, literals and directives (keeping #define
// bodies), a scope-tracking pass collects the SDK's public declarations, and a second pass follows the scripts' variable
// declarations to find which SDK functions they call on which class.
//
// What the script analysis understands (anything else counts as no use, so a check can fail but never pass wrongly):
// - "Class::Name" for SDK classes (required for static members) and "Namespace::Name" for free functions in a namespace;
// - member access "receiver.Name" / "receiver->Name" when the receiver's type is known: a variable, parameter or field
//   declared with its type (or "auto x = <expression of known type>"), "this", a call of a function whose return type
//   is known (SDK functions, the scripts' own functions, constructors, static_cast), "vector[index]",
//   "optional.value()"/"value_or()" and parenthesized expressions of those;
// - unqualified calls of functions a script class inherits from an SDK class, and SDK virtual functions overridden with
//   the "override" keyword;
// - SDK macros, expanded with their arguments before the analysis.
// Overloads are not told apart (a use of one counts for every overload of the name); members called on a receiver of
// unknown type are listed in ScriptUsage::UntypedAccesses so a failing check can point at them.
namespace Strata::Tests::SDKReader
{

	struct Token
	{
		std::string Text;
		bool Identifier = false;
		int32_t Line = 0;
	};

	struct MacroDefinition
	{
		bool FunctionLike = false;
		std::vector<std::string> Parameters; // "__VA_ARGS__" stands for "..."
		std::vector<Token> Body;
	};

	using MacroTable = std::map<std::string, MacroDefinition, std::less<>>;

	struct SourceTokens
	{
		std::vector<Token> Tokens;
		MacroTable Macros;
	};

	// Identifiers and punctuation of C++ source without comments, literals (each becomes the token "0") and preprocessor
	// directives (#define bodies go into Macros). "::", "->" and "##" are single tokens; numbers may contain digit
	// separators; raw string literals are understood.
	SourceTokens Tokenize(std::string_view source);

	// Expands the macros in tokens: object-like and function-like (arguments are expanded first, "#" makes a literal,
	// "##" pastes tokens). An expansion is rescanned for further macros on its own, without the tokens after it.
	std::vector<Token> ExpandMacros(const std::vector<Token>& tokens, const MacroTable& macros);

	// A type as far as the coverage checks need it: its shape and the last identifier of the (element) type, e.g. Value
	// "Entity" for "const Strata::Entity&", Vector "Entity" for "std::vector<Entity>", Pointer "T" for "T*".
	struct TypeRef
	{
		enum class Kind
		{
			Unknown,
			Value,
			Pointer,
			Vector,
			Optional
		};

		Kind Shape = Kind::Unknown;
		std::string Name;

		bool IsKnown() const { return Shape != Kind::Unknown; }
		bool operator==(const TypeRef& other) const = default;
	};

	// A public function of the SDK: a member function of a class or a free function (outside namespaces named Detail).
	struct SDKFunction
	{
		std::string Class;     // Empty for free functions
		std::string Namespace; // Free functions: the namespaces below Strata ("Key" for Strata::Key::Name, empty for Strata)
		std::string Name;
		bool Static = false;
		bool Virtual = false;
		std::vector<std::string> TemplateParameters;
		TypeRef ReturnType;

		// "Class::Name", "Namespace::Name" or "Name".
		std::string GetQualifiedName() const;
	};

	std::vector<SDKFunction> CollectFunctions(const std::vector<Token>& tokens);

	// The types of the SDK's script field type list: every X of "std::is_same_v<T, X>" in the initializer of the
	// variable `listName`, spelled as NormalizeTypeSpelling does.
	std::vector<std::string> CollectFieldTypes(const std::vector<Token>& tokens, std::string_view listName);

	// A type's spelling without spaces, cv-qualifiers, references and the "std::" and "Strata::" qualifiers
	// ("glm::vec2", "string", "Entity").
	std::string NormalizeTypeSpelling(const std::vector<Token>& tokens);

	struct SDKSurface
	{
		std::vector<SDKFunction> Functions;
		std::set<std::string> Classes; // Classes with public functions
		MacroTable Macros;             // Every macro of the headers
		std::set<std::string> PublicMacros;
		std::vector<std::string> FieldTypes; // Detail::c_IsFieldType

		// Adds the declarations of one header. Public macros are those named ST_SCRIPT_* but not ST_SCRIPT_DETAIL_*.
		void AddHeader(std::string_view text);

		const SDKFunction* FindFunction(std::string_view className, std::string_view name) const;
		bool IsClass(std::string_view name) const;
	};

	struct ScriptSource
	{
		std::string Name; // For messages
		std::string Text;
	};

	// A field (data member) a script class declares.
	struct ScriptField
	{
		std::string Name;
		std::string TypeSpelling; // As NormalizeTypeSpelling spells it
		TypeRef Type;
		bool Static = false;
	};

	struct ScriptClass
	{
		std::string Name;
		std::vector<std::string> Bases; // Last identifiers of the base class names
		std::vector<ScriptField> Fields;
		std::map<std::string, TypeRef, std::less<>> Functions; // Member functions and their return types
	};

	// ST_SCRIPT_CLASS(ClassName) and the members its body registers with ST_SCRIPT_FIELD.
	struct ScriptRegistration
	{
		std::string ClassName; // As spelled ("Player" or "Game::Player"), the name the engine knows the class by
		std::vector<std::string> Fields;
	};

	struct UntypedAccess
	{
		std::string Name;
		std::string Source;
		int32_t Line = 0;
	};

	struct ScriptUsage
	{
		std::set<std::string> Functions;   // Qualified names (SDKFunction::GetQualifiedName) of the SDK functions used
		std::set<std::string> Identifiers; // Every identifier of the scripts, before and after macro expansion
		std::vector<UntypedAccess> UntypedAccesses; // Accesses of SDK function names whose receiver type is unknown
		std::map<std::string, ScriptClass, std::less<>> Classes;
		std::vector<ScriptRegistration> Registrations;

		// The field of a script class or of its script base classes, or null.
		const ScriptField* FindField(std::string_view className, std::string_view fieldName) const;
	};

	ScriptUsage AnalyzeScripts(const SDKSurface& surface, const std::vector<ScriptSource>& sources);

}
