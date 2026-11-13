// `kex --skill` is read by a model that will believe every line of it, so a
// guide whose examples have rotted teaches the errors it exists to prevent.
// Every ```kex block in every topic is therefore a whole program, and this
// test puts each one through the checker and, where it has something to run,
// through the interpreter.
//
// A block may name its file after the fence, ```kex stack.spec.kex, which is
// how the testing topic shows a spec beside the source it loads.
#include "test.hxx"
#include "../src/skill/skill.hxx"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace test;

namespace {

struct Run {
    std::string output;
    int status = -1;
};

auto runKex(const std::string& args) -> Run {
    const std::string cmd =
        std::string(KEX_BINARY_PATH) + " --no-colors " + args + " 2>&1";
    Run run;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return run;
    std::array<char, 4096> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
        run.output.append(buf.data(), n);
    run.status = pclose(pipe);
    return run;
}

struct Example {
    std::string file;
    std::string source;
};

// The ```kex blocks of one topic, in order.
auto examplesOf(const kex::skill::Topic& topic) -> std::vector<Example> {
    constexpr std::string_view open = "```kex";
    constexpr std::string_view close = "\n```";
    std::vector<Example> examples;
    const auto text = topic.text;
    size_t at = 0;
    while (true) {
        const auto fence = text.find(open, at);
        if (fence == std::string_view::npos) break;
        const auto lineEnd = text.find('\n', fence);
        const auto end = text.find(close, lineEnd);
        if (lineEnd == std::string_view::npos ||
            end == std::string_view::npos)
            break;
        auto name = std::string(
            text.substr(fence + open.size(), lineEnd - fence - open.size()));
        while (!name.empty() && name.front() == ' ') name.erase(0, 1);
        if (name.empty())
            name = std::string(topic.name) + "_" +
                   std::to_string(examples.size() + 1) + ".kex";
        examples.push_back(
            {name, std::string(text.substr(lineEnd + 1, end - lineEnd))});
        at = end + close.size();
    }
    return examples;
}

auto has(std::string_view text, std::string_view part) -> bool {
    return text.find(part) != std::string_view::npos;
}

} // namespace

int main() {
    const auto& topics = kex::skill::topics();
    // Canonical, so a path reaches kex the way it does on Linux. macOS keeps
    // its temp directory behind a symlink, and two spellings of one file
    // once hid a spec that loaded its base twice from every Mac but not CI.
    const auto root =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) /
        "kex-skill-test";
    std::filesystem::remove_all(root);

    describe("the guide", [&]() {
        it("comes first and is a SKILL.md, front matter and all", [&]() {
            assertTrue(!topics.empty());
            assertEqual(topics.front().name, kex::skill::kGuideTopic);
            assertTrue(topics.front().text.starts_with("---\nname: kex\n"));
            assertTrue(has(topics.front().text, "\ndescription: "));
        });

        it("names every topic, so a reader can find each one", [&]() {
            for (const auto& topic : topics) {
                if (topic.name == kex::skill::kGuideTopic) continue;
                assertTrue(has(topics.front().text,
                               "kex --skill " + std::string(topic.name)),
                           std::string(topic.name));
            }
        });

        it("points at the lookup and the checker it tells a model to use",
           [&]() {
            assertTrue(has(topics.front().text, "kex -s List"));
            assertTrue(has(topics.front().text, "kex -C file.kex"));
        });
    });

    describe("every example", [&]() {
        for (const auto& topic : topics) {
            const auto directory = root / std::string(topic.name);
            std::filesystem::create_directories(directory);
            const auto examples = examplesOf(topic);
            // Written before any is checked: a spec loads the source beside it.
            for (const auto& example : examples)
                std::ofstream(directory / example.file) << example.source;

            it(std::string(topic.name) + " has one", [examples]() {
                assertTrue(!examples.empty());
            });

            for (const auto& example : examples) {
                const auto path = (directory / example.file).string();
                it(example.file + " checks", [path]() {
                    const auto run = runKex("-C '" + path + "'");
                    assertEqual(run.output,
                                std::string("No errors found.\n"));
                });
                const bool runs = has(example.source, "\nmain do\n") ||
                                  example.file.ends_with(".spec.kex");
                if (!runs) continue;
                it(example.file + " runs", [path]() {
                    const auto run = runKex("--run-walker '" + path + "'");
                    assertEqual(run.status, 0);
                    assertFalse(has(run.output, "error"), run.output);
                });
            }
        }
    });

    describe("kex --skill", [&]() {
        it("prints the guide, and nothing else", [&]() {
            assertEqual(runKex("--skill").output,
                        std::string(topics.front().text));
        });

        it("prints one topic by name, in any case", [&]() {
            const auto* errors = kex::skill::findTopic("errors");
            assertTrue(errors != nullptr);
            assertEqual(runKex("--skill Errors").output,
                        std::string(errors->text));
        });

        it("fails on a topic it does not have, and lists the ones it does",
           [&]() {
            const auto run = runKex("--skill nonsense");
            assertTrue(run.status != 0);
            assertTrue(has(run.output, "errors"));
        });

        it("is the first thing --help says", [&]() {
            const auto help = runKex("--help").output;
            const auto skill = help.find("--skill");
            assertTrue(skill != std::string::npos);
            assertTrue(skill < help.find("Options:"));
        });
    });

    return runAll();
}
