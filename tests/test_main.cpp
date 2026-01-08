#define LM_TESTING
#include "../main.cpp"

#include <filesystem>
#include <iostream>

static int g_failures = 0;

static void expectTrue(bool cond, const std::string& name) {
  if (!cond) {
    std::cerr << "FAIL: " << name << "\n";
    g_failures++;
  }
}

static void expectEq(const std::string& got, const std::string& want, const std::string& name) {
  if (got != want) {
    std::cerr << "FAIL: " << name << " (got='" << got << "' want='" << want << "')\n";
    g_failures++;
  }
}

static void testExtractStreamDeltaContent() {
  std::string payload = "{\"choices\":[{\"delta\":{\"content\":\"hi\"},\"index\":0,\"finish_reason\":null}]}";
  auto out = extractStreamDeltaContent(payload);
  expectTrue(out.has_value(), "extractStreamDeltaContent has value");
  expectEq(out.value_or(""), "hi", "extractStreamDeltaContent value");
}

static void testExtractStreamDeltaReasoning() {
  std::string payload = "{\"choices\":[{\"delta\":{\"reasoning\":\"step 1\"},\"index\":0,\"finish_reason\":null}]}";
  auto out = extractStreamDeltaReasoning(payload);
  expectTrue(out.has_value(), "extractStreamDeltaReasoning has value");
  expectEq(out.value_or(""), "step 1", "extractStreamDeltaReasoning value");
}

static void testReasoningWindowWrap() {
  ReasoningWindow w(5, 3);
  w.append("abcd");
  auto view1 = w.snapshotLines();
  expectEq(view1[2], "abcd", "reasoning window initial line");

  w.append("efghij");
  auto view2 = w.snapshotLines();
  expectEq(view2[1], "abcde", "reasoning window line 2");
  expectEq(view2[2], "fghij", "reasoning window line 3");

  w.append("klmno");
  auto view3 = w.snapshotLines();
  expectEq(view3[0], "abcde", "reasoning window line 1 stays");
  expectEq(view3[2], "klmno", "reasoning window line 3 updated");

  w.append("p");
  auto view4 = w.snapshotLines();
  expectEq(view4[0], "fghij", "reasoning window scroll 1");
  expectEq(view4[1], "klmno", "reasoning window scroll 2");
  expectEq(view4[2], "p", "reasoning window current line");
}

static void testBuildChatRequestJson() {
  std::vector<std::pair<std::string, std::string>> history = {
      {"user", "hi"},
      {"assistant", "yo"}
  };
  auto body = buildChatRequestJson("m", std::string("sys"), history, "next", 0.5, true);
  expectTrue(body.find("\"model\":\"m\"") != std::string::npos, "request includes model");
  expectTrue(body.find("\"role\":\"system\"") != std::string::npos, "request includes system role");
  expectTrue(body.find("\"content\":\"sys\"") != std::string::npos, "request includes system content");
  expectTrue(body.find("\"role\":\"user\"") != std::string::npos, "request includes user role");
  expectTrue(body.find("\"content\":\"hi\"") != std::string::npos, "request includes history content");
  expectTrue(body.find("\"role\":\"assistant\"") != std::string::npos, "request includes assistant role");
  expectTrue(body.find("\"content\":\"yo\"") != std::string::npos, "request includes assistant content");
  expectTrue(body.find("\"content\":\"next\"") != std::string::npos, "request includes prompt content");
  expectTrue(body.find("\"temperature\":0.5") != std::string::npos, "request includes temperature");
  expectTrue(body.find("\"stream\":true") != std::string::npos, "request includes stream");
}

static void testHistoryRoundTrip() {
  std::filesystem::path tmp = std::filesystem::temp_directory_path() / "lm_cli_test_history.txt";
  std::filesystem::remove(tmp);

  expectTrue(wipeHistory(tmp.string()), "wipeHistory works");
  expectTrue(appendTurn(tmp.string(), "hello", "world"), "appendTurn works");

  auto history = loadHistory(tmp.string());
  expectTrue(history.size() == 2, "history size 2");
  if (history.size() == 2) {
    expectEq(history[0].first, "user", "history role user");
    expectEq(history[0].second, "hello", "history content user");
    expectEq(history[1].first, "assistant", "history role assistant");
    expectEq(history[1].second, "world", "history content assistant");
  }

  std::filesystem::remove(tmp);
}

static void testTrimTrailingSlash() {
  expectEq(trimTrailingSlash("http://x/y/"), "http://x/y", "trim trailing slash");
  expectEq(trimTrailingSlash("http://x/y"), "http://x/y", "trim no slash");
}

static void testJsonEscape() {
  expectEq(jsonEscape("a\"b\n"), "a\\\"b\\n", "json escape quotes/newline");
}

int main() {
  testExtractStreamDeltaContent();
  testExtractStreamDeltaReasoning();
  testReasoningWindowWrap();
  testBuildChatRequestJson();
  testHistoryRoundTrip();
  testTrimTrailingSlash();
  testJsonEscape();

  if (g_failures == 0) {
    std::cout << "ok\n";
    return 0;
  }
  std::cerr << g_failures << " tests failed\n";
  return 1;
}
