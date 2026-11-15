#pragma once
// The opt-in standard library's interfaces as the checker describes them:
// each module checked as a build checks it, against the prelude, and
// described from that check. They replace the ones read from the source
// (`sourceSemanticInterfaces`), which resolved names on their own.
#include "../semantic/imported_interfaces.hxx"
#include <optional>

namespace kex::driver {

// Every opt-in stdlib module, built as the units of one program over
// `prelude` and described: `prelude` with them over it. Nothing when the
// build could not run.
auto checkedStdlibInterfaces(const semantic::ImportedInterfaces& prelude)
    -> std::optional<semantic::ImportedInterfaces>;

// Makes `preludeSemanticInterfaces` take them from checkedStdlibInterfaces.
auto registerCheckedStdlibInterfaces() -> void;

} // namespace kex::driver
