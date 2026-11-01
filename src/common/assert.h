#ifndef KYTY_COMMON_ASSERT_H_
#define KYTY_COMMON_ASSERT_H_

#include "common/common.h"
#include "common/logging/log.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace Common {

#ifdef __clang__
int DbgExitHandler(char const* file, int line, std::string_view text)
    __attribute__((analyzer_noreturn));
int DbgExitHandler(char const* file, int line, fmt::text_style style, std::string_view text)
    __attribute__((analyzer_noreturn));
int DbgExitIfHandler(char const* expr, char const* file, int line)
    __attribute__((analyzer_noreturn));
int DbgNotImplementedHandler(char const* expr, char const* file, int line)
    __attribute__((analyzer_noreturn));
void DbgExit(int status) __attribute__((analyzer_noreturn));
#else
int  DbgExitHandler(char const* file, int line, std::string_view text);
int  DbgExitHandler(char const* file, int line, fmt::text_style style, std::string_view text);
int  DbgExitIfHandler(char const* expr, char const* file, int line);
int  DbgNotImplementedHandler(char const* expr, char const* file, int line);
void DbgExit(int status);
#endif

// What DbgExit runs before the process ends (output written asynchronously reaching its file), and RunExitDrain
// for the other ends that skip exit handlers (std::_Exit, TerminateProcess).
void SetExitDrain(void (*drain)()) noexcept;
void RunExitDrain() noexcept;

// A report a thread is writing (the device-loss report): a fatal exit on another thread ends the process after it
// (DbgExit waits, 5 s at most), not in its middle.
void BeginExitBlockingReport() noexcept;
void EndExitBlockingReport() noexcept;

// While one is alive, EXIT on this thread throws RecoverableExit instead of ending the process: a
// precompile tool (PipelineCache::WarmSeeds) compiling inputs the game may never use skips them.
struct RecoverableExit {
	std::string message;
};
class RecoverableExitScope {
public:
	RecoverableExitScope();
	~RecoverableExitScope();
	RecoverableExitScope(const RecoverableExitScope&)            = delete;
	RecoverableExitScope& operator=(const RecoverableExitScope&) = delete;
};

} // namespace Common

#define EXIT_HALT() (Common::DbgExit(321), 1)

#ifndef KYTY_FINAL
#define EXIT_IF(x)                                                                                 \
	((void)((x) && Common::DbgExitIfHandler(#x, __FILE__, __LINE__) != 0 && (EXIT_HALT(), 1) != 0))
#else
#define EXIT_IF(x)                                                                                 \
	do {                                                                                           \
		constexpr bool kyty_exit_if_disabled = false && (x);                                       \
		(void)kyty_exit_if_disabled;                                                               \
	} while (0)
#endif

#define EXIT(...)                                                                                  \
	do {                                                                                           \
		((void)(Common::DbgExitHandler(__FILE__, __LINE__, ::fmt::sprintf(__VA_ARGS__)) &&         \
		        (EXIT_HALT(), 1)));                                                                \
	} while (0)

#define EXIT_COLOR(style, ...)                                                                     \
	do {                                                                                           \
		((void)(Common::DbgExitHandler(__FILE__, __LINE__, (style),                                \
		                               ::fmt::sprintf(__VA_ARGS__)) &&                             \
		        (EXIT_HALT(), 1)));                                                                \
	} while (0)

#define EXIT_NOT_IMPLEMENTED(x)                                                                    \
	((void)((x) && Common::DbgNotImplementedHandler(#x, __FILE__, __LINE__) != 0 &&                \
	        (EXIT_HALT(), 1) != 0))
#define KYTY_NOT_IMPLEMENTED EXIT_NOT_IMPLEMENTED(true)

#endif /* KYTY_COMMON_ASSERT_H_ */
