#pragma once

#if defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <array>
#include <cstddef>
#include <csignal>
#include <stdexcept>
#endif

namespace grok {

// Only the bridge entry point installs these handlers. Runtime destruction can
// then settle its direct children and owned paths before the same signal ends
// the original root process and lets the terminal owner settle descendants.
class Wake_termination
{
public:
#if defined(__linux__) || defined(__APPLE__)
    Wake_termination()
    {
        s_signal = 0;
        s_active = this;
        struct sigaction action{};
        action.sa_handler = remember_signal;
        ::sigemptyset(&action.sa_mask);
        for (const int signal : s_signals) {
            ::sigaddset(&action.sa_mask, signal);
        }
        for (std::size_t i = 0; i < s_signals.size(); ++i) {
            if (::sigaction(s_signals[i], nullptr, &m_previous[i]) != 0 ||
                (m_previous[i].sa_handler != SIG_IGN &&
                    ::sigaction(s_signals[i], &action, nullptr) != 0))
            {
                restore();
                throw std::runtime_error("Cannot install Grok bridge termination handlers.");
            }
            ++m_installed;
        }
    }

    ~Wake_termination() { restore(); }
    Wake_termination(const Wake_termination&) = delete;
    Wake_termination& operator=(const Wake_termination&) = delete;

    static bool requested() noexcept { return s_signal != 0; }

    static void before_standalone() noexcept
    {
        // exec does not unwind the entry-point scope. Restore first so a signal
        // arriving between the final stop check and exec cannot be swallowed.
        if (s_active) {
            s_active->restore();
        }
    }

private:
    static void remember_signal(int signal) noexcept
    {
        if (s_signal == 0) {
            s_signal = signal;
        }
    }

    void restore() noexcept
    {
        if (s_active != this) {
            return;
        }
        while (m_installed > 0) {
            --m_installed;
            (void)::sigaction(s_signals[m_installed], &m_previous[m_installed], nullptr);
        }
        s_active = nullptr;
        const int signal = s_signal;
        s_signal = 0;
        if (signal != 0) {
            (void)::raise(signal);
        }
    }

    static constexpr std::array<int, 3> s_signals{SIGHUP, SIGINT, SIGTERM};
    static inline volatile std::sig_atomic_t s_signal = 0;
    static inline Wake_termination* s_active = nullptr;
    std::array<struct sigaction, s_signals.size()> m_previous{};
    std::size_t m_installed = 0;
#else
    static constexpr bool requested() noexcept { return false; }
    static void before_standalone() noexcept {}
#endif
};

} // namespace grok
