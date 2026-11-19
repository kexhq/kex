#include "common/version.hxx"

namespace kex {

// CMake passes these two to this file alone (see CMakeLists.txt).
#ifdef KEX_GIT_REVISION
const char* const kGitRevision = KEX_GIT_REVISION;
#else
const char* const kGitRevision = "";
#endif

#ifdef KEX_BUILD_DATE
const char* const kBuildDate = KEX_BUILD_DATE;
#else
const char* const kBuildDate = "";
#endif

} // namespace kex
