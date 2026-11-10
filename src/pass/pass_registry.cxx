#include "pass_registry.hxx"

#include <cctype>

namespace kex::pass {

namespace {

class PipelineParser {
public:
    explicit PipelineParser(std::string_view text) : m_text(text) {}

    auto parse() -> ParsedPipeline {
        ParsedPipeline parsed;
        parsed.elements = elements();
        if (m_error.empty() && m_at < m_text.size())
            reportProblem("unexpected '" + std::string(1, m_text[m_at]) + "'");
        parsed.error = m_error;
        return parsed;
    }

private:
    auto elements() -> std::vector<PipelineElement> {
        std::vector<PipelineElement> parsed;
        for (;;) {
            auto element = this->element();
            if (!m_error.empty()) return parsed;
            parsed.push_back(std::move(element));
            skipSpace();
            if (m_at < m_text.size() && m_text[m_at] == ',') {
                ++m_at;
                continue;
            }
            return parsed;
        }
    }

    auto element() -> PipelineElement {
        skipSpace();
        PipelineElement parsed;
        while (m_at < m_text.size() && isNameChar(m_text[m_at])) parsed.name += m_text[m_at++];
        if (parsed.name.empty()) {
            reportProblem(m_at < m_text.size() ? "expected a pass name at '" + std::string(1, m_text[m_at]) + "'"
                                      : "expected a pass name at the end");
            return parsed;
        }
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == '(') {
            ++m_at;
            parsed.inner = elements();
            if (!m_error.empty()) return parsed;
            skipSpace();
            if (m_at >= m_text.size() || m_text[m_at] != ')') {
                reportProblem("missing ')' after '" + parsed.name + "('");
                return parsed;
            }
            ++m_at;
            if (parsed.inner.empty()) reportProblem("'" + parsed.name + "()' is empty");
        }
        return parsed;
    }

    static auto isNameChar(char c) -> bool {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
    }
    auto skipSpace() -> void {
        while (m_at < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_at]))) ++m_at;
    }
    auto reportProblem(std::string message) -> void {
        if (m_error.empty()) m_error = "pass pipeline: " + std::move(message);
    }

    std::string_view m_text;
    std::size_t m_at = 0;
    std::string m_error;
};

} // namespace

auto parsePipelineText(std::string_view text) -> ParsedPipeline {
    return PipelineParser(text).parse();
}

} // namespace kex::pass
