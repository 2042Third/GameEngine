#include <doctest/doctest.h>

#include "Strata/Network/JsonRpc.h"

#include <string>
#include <vector>

using namespace Strata;

namespace
{
	std::vector<std::string> DrainLines(JsonLineReader& reader)
	{
		std::vector<std::string> lines;
		while (std::optional<std::string> line = reader.NextLine())
			lines.push_back(*line);
		return lines;
	}
}

TEST_SUITE("Network.JsonRpc")
{
	TEST_CASE("Line reader reassembles messages split across chunks")
	{
		JsonLineReader reader;
		reader.Append(std::string_view("{\"a\":"));
		CHECK_FALSE(reader.NextLine().has_value());
		reader.Append(std::string_view("1}"));
		CHECK_FALSE(reader.NextLine().has_value());
		CHECK(reader.GetBufferedSize() == 7);
		reader.Append(std::string_view("\n{\"b\""));
		CHECK(reader.NextLine().value() == "{\"a\":1}");
		CHECK_FALSE(reader.NextLine().has_value());
		reader.Append(std::string_view(":2}\n"));
		CHECK(reader.NextLine().value() == "{\"b\":2}");
		CHECK(reader.GetBufferedSize() == 0);
		CHECK_FALSE(reader.HasError());
	}

	TEST_CASE("Line reader splits several messages in one chunk and skips blank lines")
	{
		JsonLineReader reader;
		const std::string data = "{\"id\":1}\n\n   \n{\"id\":2}\n{\"id\":3}\n{\"partial\"";
		const std::vector<uint8_t> bytes(data.begin(), data.end());
		reader.Append(std::span<const uint8_t>(bytes));
		CHECK(DrainLines(reader) == std::vector<std::string> { "{\"id\":1}", "{\"id\":2}", "{\"id\":3}" });
		CHECK(reader.GetBufferedSize() == std::string("{\"partial\"").size());
	}

	TEST_CASE("Line reader accepts CRLF terminators, including a split CR/LF pair")
	{
		JsonLineReader reader;
		reader.Append(std::string_view("{\"x\":1}\r\n{\"y\":2}\r"));
		CHECK(reader.NextLine().value() == "{\"x\":1}");
		CHECK_FALSE(reader.NextLine().has_value());
		reader.Append(std::string_view("\n"));
		CHECK(reader.NextLine().value() == "{\"y\":2}");
		CHECK_FALSE(reader.NextLine().has_value());
	}

	TEST_CASE("Line reader enforces the maximum message size")
	{
		SUBCASE("A message at the limit is accepted, including with CRLF")
		{
			JsonLineReader reader(8);
			reader.Append(std::string_view("12345678\r\n12345678\n"));
			CHECK(DrainLines(reader) == std::vector<std::string> { "12345678", "12345678" });
			CHECK_FALSE(reader.HasError());
		}

		SUBCASE("An unterminated message beyond the limit fails without waiting for its end")
		{
			JsonLineReader reader(8);
			reader.Append(std::string_view("1234567890"));
			CHECK(reader.HasError());
			CHECK(reader.GetError().find("maximum size") != std::string::npos);
			CHECK_FALSE(reader.NextLine().has_value());
			CHECK(reader.GetBufferedSize() == 0);

			// The error is permanent until Reset: the stream cannot be resynchronized.
			reader.Append(std::string_view("\n{}\n"));
			CHECK_FALSE(reader.NextLine().has_value());
			reader.Reset();
			CHECK_FALSE(reader.HasError());
			reader.Append(std::string_view("{}\n"));
			CHECK(reader.NextLine().value() == "{}");
		}

		SUBCASE("A complete oversized message after valid ones fails when reached")
		{
			JsonLineReader reader(8);
			reader.Append(std::string_view("{}\n123456789\n{}\n"));
			CHECK(reader.NextLine().value() == "{}");
			CHECK_FALSE(reader.NextLine().has_value());
			CHECK(reader.HasError());
		}

		SUBCASE("A message growing chunk by chunk fails once it crosses the limit")
		{
			JsonLineReader reader(16);
			for (int index = 0; index < 4 && !reader.HasError(); index++)
			{
				reader.Append(std::string_view("abcde"));
				CHECK_FALSE(reader.NextLine().has_value());
			}
			CHECK(reader.HasError());
		}
	}

	TEST_CASE("Message builders produce JSON-RPC 2.0 messages")
	{
		const nlohmann::json request = JsonRpc::MakeRequest(7, "scene.load", nlohmann::json { { "path", "a.scene" } });
		CHECK(request["jsonrpc"] == "2.0");
		CHECK(request["id"] == 7);
		CHECK(request["method"] == "scene.load");
		CHECK(request["params"]["path"] == "a.scene");

		const nlohmann::json notification = JsonRpc::MakeNotification("log.message", nullptr);
		CHECK_FALSE(notification.contains("id"));
		CHECK_FALSE(notification.contains("params"));

		const nlohmann::json result = JsonRpc::MakeResult("abc", nlohmann::json::array({ 1, 2 }));
		CHECK(result["id"] == "abc");
		CHECK(result["result"] == nlohmann::json::array({ 1, 2 }));
		CHECK_FALSE(result.contains("error"));

		const nlohmann::json error = JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::MethodNotFound, "nope");
		CHECK(error["id"].is_null());
		CHECK(error["error"]["code"] == -32601);
		CHECK(error["error"]["message"] == "nope");
		CHECK_FALSE(error["error"].contains("data"));
		CHECK(JsonRpc::MakeError(1, -1, "x", nlohmann::json { { "detail", 1 } })["error"]["data"]["detail"] == 1);

		CHECK(static_cast<int>(JsonRpc::ErrorCode::ParseError) == -32700);
		CHECK(static_cast<int>(JsonRpc::ErrorCode::InvalidRequest) == -32600);
		CHECK(static_cast<int>(JsonRpc::ErrorCode::InvalidParams) == -32602);
		CHECK(static_cast<int>(JsonRpc::ErrorCode::InternalError) == -32603);
		CHECK(static_cast<int>(JsonRpc::ErrorCode::Unauthorized) == -32001);
	}

	TEST_CASE("Serialization is a single line and never fails")
	{
		const nlohmann::json message = JsonRpc::MakeResult(1, nlohmann::json { { "text", "line one\nline two\r\n" } });
		const std::string serialized = JsonRpc::Serialize(message);
		CHECK(serialized.find('\n') == std::string::npos);
		CHECK(serialized.find('\r') == std::string::npos);
		CHECK(JsonRpc::Parse(serialized).value() == message);

		// Invalid UTF-8 is replaced instead of throwing.
		const nlohmann::json invalid = nlohmann::json { { "text", std::string("bad \xFF\xFE byte") } };
		const std::string replaced = JsonRpc::Serialize(invalid);
		CHECK(replaced.find("bad") != std::string::npos);
		CHECK(JsonRpc::Parse(replaced).has_value());
	}

	TEST_CASE("Parse rejects invalid JSON without throwing")
	{
		CHECK_FALSE(JsonRpc::Parse("{not json").has_value());
		CHECK_FALSE(JsonRpc::Parse("").has_value());
		CHECK_FALSE(JsonRpc::Parse("{} trailing").has_value());
		CHECK(JsonRpc::Parse(" {\"a\": [1, 2]} ").value()["a"][1] == 2);
	}

	TEST_CASE("Request validation")
	{
		auto validate = [](std::string_view text) { return JsonRpc::ValidateRequest(JsonRpc::Parse(text).value()); };

		JsonRpc::RequestValidation valid = validate(R"({"jsonrpc":"2.0","id":3,"method":"a.b","params":{"x":1}})");
		CHECK(valid.Valid);
		CHECK_FALSE(valid.IsNotification);
		CHECK(valid.Id == 3);

		JsonRpc::RequestValidation notification = validate(R"({"jsonrpc":"2.0","method":"a.b"})");
		CHECK(notification.Valid);
		CHECK(notification.IsNotification);

		CHECK(validate(R"({"jsonrpc":"2.0","id":"s","method":"m","params":[1,2]})").Valid);
		CHECK(validate(R"({"jsonrpc":"2.0","id":null,"method":"m"})").Valid);

		JsonRpc::RequestValidation wrongVersion = validate(R"({"jsonrpc":"1.0","id":5,"method":"m"})");
		CHECK_FALSE(wrongVersion.Valid);
		CHECK(wrongVersion.Id == 5); // The id is still recovered for the error response
		CHECK_FALSE(wrongVersion.Error.empty());

		CHECK_FALSE(validate(R"({"id":1,"method":"m"})").Valid);
		CHECK_FALSE(validate(R"({"jsonrpc":"2.0","id":1})").Valid);
		CHECK_FALSE(validate(R"({"jsonrpc":"2.0","id":1,"method":""})").Valid);
		CHECK_FALSE(validate(R"({"jsonrpc":"2.0","id":1,"method":42})").Valid);
		CHECK_FALSE(validate(R"({"jsonrpc":"2.0","id":1,"method":"m","params":"text"})").Valid);
		CHECK_FALSE(validate(R"({"jsonrpc":"2.0","id":{"x":1},"method":"m"})").Valid);
		CHECK_FALSE(validate("[1,2]").Valid);
		CHECK_FALSE(validate("42").Valid);

		CHECK(JsonRpc::IsResponse(JsonRpc::MakeResult(1, 2)));
		CHECK(JsonRpc::IsResponse(JsonRpc::MakeError(1, -1, "x")));
		CHECK_FALSE(JsonRpc::IsResponse(JsonRpc::MakeRequest(1, "m")));
		CHECK(JsonRpc::IsValidId("x"));
		CHECK(JsonRpc::IsValidId(1.5));
		CHECK_FALSE(JsonRpc::IsValidId(nlohmann::json::array()));
	}
}
