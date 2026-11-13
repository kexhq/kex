// `kex -s <Name>` is how a person — or a model writing Kex for the first
// time — finds out what the standard library is called. The index is tested
// on a small source of its own, so these cases pin what is collected and how
// it is shown rather than the wording of any one stdlib doc comment; the last
// group asks the real binary about the real library.
#include "test.hxx"
#include "../src/help/api_help.hxx"
#include <array>
#include <cstdio>
#include <sstream>
#include <string>

using namespace test;

namespace {

const std::string kSource = R"(module Shapes

# A point on the plane.
record Point do
  x : Float = 0.0
  # The vertical coordinate.
  y : Float = 0.0
end

# What can be measured.
trait Measurable do
  # The size of the thing.
  size :> Float

  let double = size * 2.0
end

# Builders for +Point+.
module Point do
  # The point at +(0, 0)+: start with +using Shapes+.
  #
  # @example
  #   Point.origin.x + 1.0   # => 1.0
  let origin -> Point = Point {}

  let scaled(p: Point, by: Point -> Float) -> Float = by(p)
end

make Point, implement: Measurable do
  # Whether the point sits at the origin. A second sentence.
  origin? :> Bool
  let origin? = x == 0.0

  # The distance from the origin, i.e. the length. Never negative.
  let length -> Float = x

  # Moves the point by either kind of offset.
  let moved(by: (Point | Float) -> Float) -> Point = this

  # Keeps the points the test accepts.
  keep :> (Point -> Bool) -> Point?
  let keep(test) = test(this) then Just(this) else None

  private do
    secret :> Integer
    let secret = 42
  end
end

make [Point] do
  # The first point.
  lead :> Point?
  let lead = first
end
)";

auto indexOf(const std::string& usingName) -> kex::help::ApiIndex {
    kex::help::ApiIndex index;
    index.addSource("shapes.kex", kSource, usingName);
    return index;
}

auto topic(const kex::help::ApiIndex& index, const std::string& query)
    -> std::string {
    std::ostringstream out, err;
    index.printTopic(query, out, err);
    return out.str() + err.str();
}

auto has(const std::string& text, const std::string& part) -> bool {
    return text.find(part) != std::string::npos;
}

auto runKex(const std::string& args) -> std::string {
    std::string cmd = std::string(KEX_BINARY_PATH) + " " + args + " 2>&1";
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    std::array<char, 4096> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
        result.append(buf.data(), n);
    pclose(pipe);
    return result;
}

} // namespace

int main() {
    const auto index = indexOf("shapes");

    // Cases run after every describe has returned, so what they share lives
    // here rather than in a describe.
    const auto point = topic(index, "Shapes.Point");

    describe("an owner", [&]() {
        it("shows a record with its fields and without their comments", [&]() {
            assertTrue(has(point, "record Point do\n  x : Float = 0.0\n"
                                  "  y : Float = 0.0\nend"));
        });

        it("says which using it needs, spelled as the source spells it",
           [&]() { assertTrue(has(point, "needs `using Shapes`")); });

        it("lists the module of the same name with the type", [&]() {
            assertTrue(has(point, "origin : Point"));
        });

        it("gives a function typed inline the signature line it implies",
           [&]() {
            assertTrue(has(point, "scaled : Point -> (Point -> Float) -> Float"));
        });

        it("keeps the parens that make a parameter a function", [&]() {
            assertTrue(has(point, "keep :> (Point -> Bool) -> Point?"));
        });

        it("puts the first sentence of the doc beside each member", [&]() {
            assertTrue(has(point, "Whether the point sits at the origin.\n"));
            assertFalse(has(point, "A second sentence"));
        });

        it("does not end a sentence at an abbreviation", [&]() {
            assertTrue(has(point, "The distance from the origin, i.e. the "
                                  "length.\n"));
            assertFalse(has(point, "Never negative"));
        });

        it("keeps the parens around a union a function takes", [&]() {
            assertTrue(has(point, "moved :> ((Point | Float) -> Float) -> Point"));
        });

        it("leaves private members out", [&]() {
            assertFalse(has(point, "secret"));
        });

        it("names what its traits add", [&]() {
            assertTrue(has(point, "from Shapes.Measurable"));
            assertTrue(has(point, "size, double"));
        });

        it("is found by its last segment and in any case", [&]() {
            assertEqual(topic(index, "point"), point);
        });

        it("files a make on a list under List", [&]() {
            assertTrue(has(topic(index, "List"), "lead :> Point?"));
            assertTrue(has(topic(index, "List"), "make [Point]"));
        });
    });

    describe("a member", [&]() {
        it("is shown in full, with its examples", [&]() {
            const auto origin = topic(index, "Point.origin");
            assertTrue(has(origin, "Shapes.Point.origin\n  origin : Point"));
            assertTrue(has(origin, "Point.origin.x + 1.0   # => 1.0"));
        });

        it("respells RDoc code marks as backticks, in prose only", [&]() {
            const auto origin = topic(index, "Point.origin");
            assertTrue(has(origin, "The point at `(0, 0)`: start with "
                                   "`using Shapes`."));
        });

        it("is found without the ? a shell would have eaten", [&]() {
            assertTrue(has(topic(index, "Point.origin"), "origin? :> Bool"));
        });

        it("is found on a trait the owner implements", [&]() {
            assertTrue(has(topic(index, "Point.size"),
                           "Shapes.Measurable.size"));
        });

        it("is found by its bare name", [&]() {
            assertTrue(has(topic(index, "lead"), "List.lead  (make [Point])"));
        });
    });

    describe("a miss", [&]() {
        it("fails, and offers the names that contain the word", [&]() {
            std::ostringstream out, err;
            assertFalse(index.printTopic("orig", out, err));
            assertTrue(out.str().empty());
            assertTrue(has(err.str(), "Shapes.Point.origin"));
        });
    });

    describe("a prelude source", [&]() {
        it("needs no using", [&]() {
            assertFalse(has(topic(indexOf(""), "Point"), "needs `using"));
        });
    });

    describe("kex -s", [&]() {
        it("lists the library when given nothing", [&]() {
            const auto overview = runKex("-s");
            assertTrue(has(overview, "Always available (the prelude):"));
            assertTrue(has(overview, "\n  List "));
            assertTrue(has(overview, "\n  FS "));
        });

        it("answers for a name", [&]() {
            const auto list = runKex("-s List");
            assertTrue(has(list, "type List<X> = [X]"));
            assertTrue(has(list, "first :> X?"));
        });

        it("still summarises a source file", [&]() {
            assertEqual(runKex("-s examples/fact.kex"),
                        std::string("factorial : Integer -> Integer\n"));
        });
    });

    return runAll();
}
