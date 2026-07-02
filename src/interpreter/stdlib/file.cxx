#include "../evaluator.hxx"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace kex::interpreter {

// Helper: open mode from an Atom (Read/Write/Append/ReadWrite)
static auto modeFlags(const ValuePtr& modeVal) -> std::ios::openmode {
    if (auto* atom = std::get_if<AtomValue>(&modeVal->data)) {
        if (atom->name == "Write")     return std::ios::out | std::ios::trunc;
        if (atom->name == "Append")    return std::ios::out | std::ios::app;
        if (atom->name == "ReadWrite") return std::ios::in  | std::ios::out;
    }
    return std::ios::in;
}

// Helper: split a string into lines (no trailing newline per line)
static auto splitLines(const std::string& content) -> std::vector<ValuePtr> {
    std::vector<ValuePtr> result;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        result.push_back(Value::string(line));
    }
    return result;
}

auto Evaluator::registerFileBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    m_globalEnv->define("File", Value::module("File"));
    m_globalEnv->define("FileHandle", Value::module("FileHandle"));

    // File.open(path, mode) -> FileHandle?
    // File.open(path, mode, block) -> block result or None on failure
    reg("File::open", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        const auto& path = pathStr->value;

        // Check mock first: if mocked, create an in-memory-backed handle
        auto mockIt = m_mockFiles.find(path);
        bool isMocked = mockIt != m_mockFiles.end();

        std::shared_ptr<std::fstream> fs;
        if (!isMocked) {
            auto flags = modeFlags(args[1]);
            fs = std::make_shared<std::fstream>(path, flags | std::ios::binary);
            if (!fs->is_open()) return Value::none();
        } else {
            // Mocked: open an in-memory stringstream masquerading as fstream.
            // We use a real fstream opened on /dev/null so the handle value
            // exists; actual reads/writes go through the mock registry methods.
            fs = std::make_shared<std::fstream>();
        }

        auto handle = std::make_shared<Value>();
        handle->data = FileHandleValue{fs, path};

        if (args.size() >= 3) {
            auto* fn = std::get_if<FunctionValue>(&args[2]->data);
            if (!fn || !fn->native) return Value::none();
            auto result = fn->native({handle});
            if (fs->is_open()) fs->close();
            return result;
        }
        return handle;
    });

    // FileHandle.readLine(handle) -> String?
    reg("FileHandle::readLine", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* h = std::get_if<FileHandleValue>(&args[0]->data);
        if (!h) return Value::none();

        auto mockIt = m_mockFiles.find(h->path);
        if (mockIt != m_mockFiles.end()) {
            // Mocked: use stream offset stored externally — use stream pos hack:
            // we store a stringstream in h->stream but that doesn't work.
            // Simplest: the mock handle reads from a stringstream we create here.
            // We need per-handle state — use a cursor in the path-keyed mock.
            // For now: return the first line and pop it (not ideal but functional).
            // A better approach is to use a position cursor.
            //
            // Better: use the fstream's tellg as a cursor into the mock content.
            // We stuff the mock content into the fstream if it's not open.
            // Actually, let's store cursor in the stream itself using seekg/tellg
            // on a stringbuf. Since std::fstream can't use a stringbuf, we'll
            // simulate with a cursor embedded in the path: __mock_pos__path.
            std::string cursorKey = "__mock_pos__" + h->path;
            auto posIt = m_mockFiles.find(cursorKey);
            size_t pos = 0;
            if (posIt != m_mockFiles.end()) {
                try { pos = static_cast<size_t>(std::stoul(posIt->second)); } catch (...) {}
            }
            const auto& content = mockIt->second;
            if (pos >= content.size()) return Value::none();
            auto end = content.find('\n', pos);
            std::string line;
            if (end == std::string::npos) {
                line = content.substr(pos);
                pos = content.size();
            } else {
                line = content.substr(pos, end - pos);
                pos = end + 1;
            }
            m_mockFiles[cursorKey] = std::to_string(pos);
            return Value::string(line);
        }

        if (!h->stream || !h->stream->is_open()) return Value::none();
        std::string line;
        if (!std::getline(*h->stream, line)) return Value::none();
        return Value::string(line);
    });

    // FileHandle.read(handle) -> String?  (read remaining content)
    reg("FileHandle::read", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* h = std::get_if<FileHandleValue>(&args[0]->data);
        if (!h) return Value::none();

        auto mockIt = m_mockFiles.find(h->path);
        if (mockIt != m_mockFiles.end()) {
            std::string cursorKey = "__mock_pos__" + h->path;
            size_t pos = 0;
            auto posIt = m_mockFiles.find(cursorKey);
            if (posIt != m_mockFiles.end()) {
                try { pos = static_cast<size_t>(std::stoul(posIt->second)); } catch (...) {}
            }
            const auto& content = mockIt->second;
            if (pos >= content.size()) return Value::string("");
            auto remaining = content.substr(pos);
            m_mockFiles[cursorKey] = std::to_string(content.size());
            return Value::string(remaining);
        }

        if (!h->stream || !h->stream->is_open()) return Value::none();
        std::ostringstream buf;
        buf << h->stream->rdbuf();
        return Value::string(buf.str());
    });

    // FileHandle.write(handle, content) -> Bool
    reg("FileHandle::write", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* h = std::get_if<FileHandleValue>(&args[0]->data);
        if (!h) return Value::boolean(false);
        std::string content = args[1]->toString();

        auto mockIt = m_mockFiles.find(h->path);
        if (mockIt != m_mockFiles.end()) {
            mockIt->second += content;
            return Value::boolean(true);
        }

        if (!h->stream || !h->stream->is_open()) return Value::boolean(false);
        *h->stream << content;
        return Value::boolean(static_cast<bool>(*h->stream));
    });

    // FileHandle.writeLine(handle, content) -> Bool
    reg("FileHandle::writeLine", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* h = std::get_if<FileHandleValue>(&args[0]->data);
        if (!h) return Value::boolean(false);
        std::string content = args[1]->toString();

        auto mockIt = m_mockFiles.find(h->path);
        if (mockIt != m_mockFiles.end()) {
            mockIt->second += content + "\n";
            return Value::boolean(true);
        }

        if (!h->stream || !h->stream->is_open()) return Value::boolean(false);
        *h->stream << content << "\n";
        return Value::boolean(static_cast<bool>(*h->stream));
    });

    // FileHandle.eof?(handle) -> Bool
    reg("FileHandle::eof?", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(true);
        auto* h = std::get_if<FileHandleValue>(&args[0]->data);
        if (!h) return Value::boolean(true);

        auto mockIt = m_mockFiles.find(h->path);
        if (mockIt != m_mockFiles.end()) {
            std::string cursorKey = "__mock_pos__" + h->path;
            size_t pos = 0;
            auto posIt = m_mockFiles.find(cursorKey);
            if (posIt != m_mockFiles.end()) {
                try { pos = static_cast<size_t>(std::stoul(posIt->second)); } catch (...) {}
            }
            return Value::boolean(pos >= mockIt->second.size());
        }

        if (!h->stream) return Value::boolean(true);
        return Value::boolean(h->stream->eof());
    });

    // FileHandle.close(handle) -> Unit
    reg("FileHandle::close", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (!args.empty()) {
            if (auto* h = std::get_if<FileHandleValue>(&args[0]->data)) {
                if (h->stream && h->stream->is_open()) h->stream->close();
            }
        }
        return Value::unit();
    });

    // File.read(path) -> String?
    reg("File::read", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        auto it = m_mockFiles.find(pathStr->value);
        if (it != m_mockFiles.end()) return Value::string(it->second);
        std::ifstream file(pathStr->value, std::ios::binary);
        if (!file.is_open()) return Value::none();
        std::ostringstream buf;
        buf << file.rdbuf();
        if (file.bad()) return Value::none();
        return Value::string(buf.str());
    });

    // File.write(path, content) -> Bool
    reg("File::write", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        std::string content = args[1]->toString();
        if (m_mockFiles.count(pathStr->value)) {
            m_mockFiles[pathStr->value] = content;
            return Value::boolean(true);
        }
        std::ofstream file(pathStr->value, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return Value::boolean(false);
        file << content;
        return Value::boolean(static_cast<bool>(file));
    });

    // File.append(path, content) -> Bool
    reg("File::append", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        std::string content = args[1]->toString();
        if (m_mockFiles.count(pathStr->value)) {
            m_mockFiles[pathStr->value] += content;
            return Value::boolean(true);
        }
        std::ofstream file(pathStr->value, std::ios::binary | std::ios::app);
        if (!file.is_open()) return Value::boolean(false);
        file << content;
        return Value::boolean(static_cast<bool>(file));
    });

    // File.exists?(path) -> Bool  /  File.file?(path) -> Bool
    auto fileExistsFn = [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        if (m_mockFiles.count(pathStr->value)) return Value::boolean(true);
        std::error_code ec;
        return Value::boolean(std::filesystem::exists(pathStr->value, ec) &&
                              std::filesystem::is_regular_file(pathStr->value, ec));
    };
    reg("File::exists?", fileExistsFn);
    reg("File::file?",   fileExistsFn);

    // File.dir?(path) -> Bool
    reg("File::directory?", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        if (m_mockDirs.count(pathStr->value)) return Value::boolean(true);
        std::error_code ec;
        return Value::boolean(std::filesystem::is_directory(pathStr->value, ec));
    });

    // File.delete(path) -> Bool
    reg("File::delete", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        if (m_mockFiles.count(pathStr->value)) {
            m_mockFiles.erase(pathStr->value);
            m_mockFiles.erase("__mock_pos__" + pathStr->value);
            return Value::boolean(true);
        }
        std::error_code ec;
        bool removed = std::filesystem::remove(pathStr->value, ec);
        return Value::boolean(removed && !ec);
    });

    // File.copy(src, dst) -> Bool
    reg("File::copy", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* src = std::get_if<StringValue>(&args[0]->data);
        auto* dst = std::get_if<StringValue>(&args[1]->data);
        if (!src || !dst) return Value::boolean(false);
        auto srcMock = m_mockFiles.find(src->value);
        if (srcMock != m_mockFiles.end()) {
            m_mockFiles[dst->value] = srcMock->second;
            return Value::boolean(true);
        }
        std::error_code ec;
        std::filesystem::copy_file(src->value, dst->value,
            std::filesystem::copy_options::overwrite_existing, ec);
        return Value::boolean(!ec);
    });

    // File.rename(src, dst) -> Bool
    reg("File::rename", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* src = std::get_if<StringValue>(&args[0]->data);
        auto* dst = std::get_if<StringValue>(&args[1]->data);
        if (!src || !dst) return Value::boolean(false);
        auto srcMock = m_mockFiles.find(src->value);
        if (srcMock != m_mockFiles.end()) {
            m_mockFiles[dst->value] = std::move(srcMock->second);
            m_mockFiles.erase(srcMock);
            return Value::boolean(true);
        }
        std::error_code ec;
        std::filesystem::rename(src->value, dst->value, ec);
        return Value::boolean(!ec);
    });

    // File.lines(path) -> [String]?
    reg("File::lines", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        auto mockIt = m_mockFiles.find(pathStr->value);
        if (mockIt != m_mockFiles.end())
            return Value::list(splitLines(mockIt->second));
        std::ifstream file(pathStr->value, std::ios::binary);
        if (!file.is_open()) return Value::none();
        std::vector<ValuePtr> result;
        std::string line;
        while (std::getline(file, line)) result.push_back(Value::string(line));
        if (file.bad()) return Value::none();
        return Value::list(std::move(result));
    });

    // File.feed(path) -> Stream<String>?
    reg("File::feed", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        auto lines = std::make_shared<std::vector<std::string>>();
        auto mockIt = m_mockFiles.find(pathStr->value);
        if (mockIt != m_mockFiles.end()) {
            std::istringstream ss(mockIt->second);
            std::string line;
            while (std::getline(ss, line)) lines->push_back(line);
        } else {
            std::ifstream file(pathStr->value, std::ios::binary);
            if (!file.is_open()) return Value::none();
            std::string line;
            while (std::getline(file, line)) lines->push_back(line);
            if (file.bad()) return Value::none();
        }
        auto stream = std::make_shared<Value>();
        stream->data = StreamValue{[lines](int64_t index) -> ValuePtr {
            if (index < 0 || static_cast<size_t>(index) >= lines->size())
                return Value::none();
            return Value::string((*lines)[index]);
        }, 0};
        return stream;
    });

    // File.size(path) -> Int?
    reg("File::size", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        auto mockIt = m_mockFiles.find(pathStr->value);
        if (mockIt != m_mockFiles.end())
            return Value::integer(static_cast<int64_t>(mockIt->second.size()));
        std::error_code ec;
        auto sz = std::filesystem::file_size(pathStr->value, ec);
        if (ec) return Value::none();
        return Value::integer(static_cast<int64_t>(sz));
    });

    // File.basename(path) -> String
    reg("File::basename", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::string("");
        return Value::string(std::filesystem::path(pathStr->value).filename().string());
    });

    // File.dirname(path) -> String
    reg("File::dirname", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string(".");
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::string(".");
        auto parent = std::filesystem::path(pathStr->value).parent_path();
        return Value::string(parent.empty() ? "." : parent.string());
    });

    // File.extension(path) -> String
    reg("File::extension", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::string("");
        return Value::string(std::filesystem::path(pathStr->value).extension().string());
    });

    // File.join(base, part) -> String
    reg("File::join", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return args.empty() ? Value::string("") : Value::string(args[0]->toString());
        auto* base = std::get_if<StringValue>(&args[0]->data);
        auto* part = std::get_if<StringValue>(&args[1]->data);
        if (!base || !part) return Value::string("");
        return Value::string((std::filesystem::path(base->value) / part->value).string());
    });

    // File.absolute(path) -> String?
    reg("File::absolute", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        std::error_code ec;
        auto abs = std::filesystem::absolute(pathStr->value, ec);
        if (ec) return Value::none();
        auto canon = std::filesystem::weakly_canonical(abs, ec);
        return Value::string(ec ? abs.string() : canon.string());
    });
}

auto Evaluator::registerDirectoryBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    m_globalEnv->define("Directory", Value::module("Directory"));

    // Directory.exists?(path) -> Bool  /  Directory.dir?(path) -> Bool
    auto dirExistsFn = [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        if (m_mockDirs.count(pathStr->value)) return Value::boolean(true);
        std::error_code ec;
        return Value::boolean(std::filesystem::is_directory(pathStr->value, ec));
    };
    reg("Directory::exists?", dirExistsFn);
    reg("Directory::directory?",    dirExistsFn);

    // Directory.file?(path) -> Bool
    reg("Directory::file?", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        if (m_mockFiles.count(pathStr->value)) return Value::boolean(true);
        std::error_code ec;
        return Value::boolean(std::filesystem::exists(pathStr->value, ec) &&
                              std::filesystem::is_regular_file(pathStr->value, ec));
    });

    // Directory.create(path) -> Bool  (creates including parents)
    reg("Directory::create", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        if (m_mockDirs.count(pathStr->value)) return Value::boolean(true);
        std::error_code ec;
        std::filesystem::create_directories(pathStr->value, ec);
        return Value::boolean(!ec);
    });

    // Directory.delete(path) -> Bool  (empty directory only)
    reg("Directory::delete", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        if (m_mockDirs.count(pathStr->value)) {
            m_mockDirs.erase(pathStr->value);
            return Value::boolean(true);
        }
        std::error_code ec;
        bool removed = std::filesystem::remove(pathStr->value, ec);
        return Value::boolean(removed && !ec);
    });

    // Directory.deleteAll(path) -> Bool  (recursive)
    reg("Directory::deleteAll", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        if (m_mockDirs.count(pathStr->value)) {
            m_mockDirs.erase(pathStr->value);
            // Remove all mocked files under this directory
            const auto& dirPath = pathStr->value;
            std::string prefix = dirPath;
            if (!prefix.empty() && prefix.back() != '/') prefix += '/';
            for (auto it = m_mockFiles.begin(); it != m_mockFiles.end(); ) {
                if (it->first.rfind(prefix, 0) == 0) it = m_mockFiles.erase(it);
                else ++it;
            }
            return Value::boolean(true);
        }
        std::error_code ec;
        std::filesystem::remove_all(pathStr->value, ec);
        return Value::boolean(!ec);
    });

    // Directory.list(path) -> [String]?  (entry names, not full paths)
    reg("Directory::list", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        const auto& dirPath = pathStr->value;

        if (m_mockDirs.count(dirPath)) {
            // Collect names of mocked files/dirs directly under this path
            std::string prefix = dirPath;
            if (!prefix.empty() && prefix.back() != '/') prefix += '/';
            std::vector<ValuePtr> entries;
            for (const auto& [p, _] : m_mockFiles) {
                if (p.rfind(prefix, 0) == 0 && p.find('/', prefix.size()) == std::string::npos)
                    entries.push_back(Value::string(std::filesystem::path(p).filename().string()));
            }
            for (const auto& d : m_mockDirs) {
                if (d != dirPath && d.rfind(prefix, 0) == 0 &&
                    d.find('/', prefix.size()) == std::string::npos)
                    entries.push_back(Value::string(std::filesystem::path(d).filename().string()));
            }
            return Value::list(std::move(entries));
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(dirPath, ec)) return Value::none();
        std::vector<ValuePtr> entries;
        for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
            if (!ec) entries.push_back(Value::string(entry.path().filename().string()));
        }
        return Value::list(std::move(entries));
    });

    // Directory.files(path) -> [String]?
    reg("Directory::files", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        const auto& dirPath = pathStr->value;

        if (m_mockDirs.count(dirPath)) {
            std::string prefix = dirPath;
            if (!prefix.empty() && prefix.back() != '/') prefix += '/';
            std::vector<ValuePtr> files;
            for (const auto& [p, _] : m_mockFiles) {
                if (p.rfind(prefix, 0) == 0 && p.find('/', prefix.size()) == std::string::npos)
                    files.push_back(Value::string(std::filesystem::path(p).filename().string()));
            }
            return Value::list(std::move(files));
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(dirPath, ec)) return Value::none();
        std::vector<ValuePtr> files;
        for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
            if (!ec && std::filesystem::is_regular_file(entry.path(), ec))
                files.push_back(Value::string(entry.path().filename().string()));
        }
        return Value::list(std::move(files));
    });

    // Directory.dirs(path) -> [String]?
    reg("Directory::directories", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        const auto& dirPath = pathStr->value;

        if (m_mockDirs.count(dirPath)) {
            std::string prefix = dirPath;
            if (!prefix.empty() && prefix.back() != '/') prefix += '/';
            std::vector<ValuePtr> dirs;
            for (const auto& d : m_mockDirs) {
                if (d != dirPath && d.rfind(prefix, 0) == 0 &&
                    d.find('/', prefix.size()) == std::string::npos)
                    dirs.push_back(Value::string(std::filesystem::path(d).filename().string()));
            }
            return Value::list(std::move(dirs));
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(dirPath, ec)) return Value::none();
        std::vector<ValuePtr> dirs;
        for (const auto& entry : std::filesystem::directory_iterator(dirPath, ec)) {
            if (!ec && std::filesystem::is_directory(entry.path(), ec))
                dirs.push_back(Value::string(entry.path().filename().string()));
        }
        return Value::list(std::move(dirs));
    });

    // Directory.current() -> String
    reg("Directory::current", [](std::vector<ValuePtr>) -> ValuePtr {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        return Value::string(ec ? "" : cwd.string());
    });

    // Directory.home() -> String?
    reg("Directory::home", [](std::vector<ValuePtr>) -> ValuePtr {
        const char* home = std::getenv("HOME");
        if (home) return Value::string(home);
        return Value::none();
    });
}

auto Evaluator::registerMockBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    m_globalEnv->define("Mock", Value::module("Mock"));

    // Mock.FS — returns a sub-namespace placeholder so Mock.FS.File/Directory/clear work
    reg("Mock::FS", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::module("Mock::FS");
    });

    // Mock.FS.File(path, content) -> Unit  — register an in-memory file
    reg("Mock::FS::File", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::unit();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::unit();
        m_mockFiles[pathStr->value] = args[1]->toString();
        return Value::unit();
    });

    // Mock.FS.Directory(path) -> Unit  — register an in-memory directory
    reg("Mock::FS::Directory", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::unit();
        m_mockDirs.insert(pathStr->value);
        return Value::unit();
    });

    // Mock.FS.clear() -> Unit  — reset all FS mocks
    reg("Mock::FS::clear", [this](std::vector<ValuePtr>) -> ValuePtr {
        m_mockFiles.clear();
        m_mockDirs.clear();
        return Value::unit();
    });
}

} // namespace kex::interpreter
