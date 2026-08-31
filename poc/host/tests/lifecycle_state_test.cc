#include <array>

#include <gtest/gtest.h>

#include "frame_poc/lifecycle_state.hh"

using frame::poc::lifecycle_event;
using frame::poc::plugin_state;
using frame::poc::transition;

namespace {

struct step {
    lifecycle_event event;
    plugin_state expected;
};

template <std::size_t Size>
void expect_path(plugin_state initial, const std::array<step, Size>& steps) {
    plugin_state current = initial;
    for (const step& item : steps) {
        const auto result = transition(current, item.event);
        ASSERT_EQ(result.error, FRAME_OK);
        ASSERT_EQ(result.next, item.expected);
        current = result.next;
    }
}

TEST(LifecycleState, FollowsColdStartAndUnload) {
    constexpr std::array cold_start{
        step{lifecycle_event::begin_load, plugin_state::loading},
        step{lifecycle_event::prepare_succeeded, plugin_state::prepared},
        step{lifecycle_event::activation_committed, plugin_state::active},
        step{lifecycle_event::begin_quiesce, plugin_state::quiescing},
        step{lifecycle_event::quiesce_succeeded, plugin_state::unloading},
        step{lifecycle_event::unload_succeeded, plugin_state::unloaded},
    };
    expect_path(plugin_state::staged, cold_start);
}

TEST(LifecycleState, SupportsPauseResumeAndCommittedUpdate) {
    constexpr std::array aborted{
        step{lifecycle_event::begin_pause, plugin_state::pausing},
        step{lifecycle_event::pause_succeeded, plugin_state::paused},
        step{lifecycle_event::resume_succeeded, plugin_state::active},
    };
    expect_path(plugin_state::active, aborted);

    constexpr std::array committed{
        step{lifecycle_event::begin_pause, plugin_state::pausing},
        step{lifecycle_event::pause_succeeded, plugin_state::paused},
        step{lifecycle_event::begin_quiesce, plugin_state::quiescing},
        step{lifecycle_event::quiesce_succeeded, plugin_state::unloading},
        step{lifecycle_event::unload_succeeded, plugin_state::unloaded},
    };
    expect_path(plugin_state::active, committed);
}

TEST(LifecycleState, RejectsEveryUnspecifiedTransition) {
    constexpr std::array states{
        plugin_state::staged,    plugin_state::loading,
        plugin_state::prepared,  plugin_state::active,
        plugin_state::pausing,   plugin_state::paused,
        plugin_state::quiescing, plugin_state::unloading,
        plugin_state::unloaded,  plugin_state::failed_restart_required,
    };
    constexpr std::array events{
        lifecycle_event::begin_load,
        lifecycle_event::prepare_succeeded,
        lifecycle_event::activation_committed,
        lifecycle_event::begin_pause,
        lifecycle_event::pause_succeeded,
        lifecycle_event::resume_succeeded,
        lifecycle_event::begin_quiesce,
        lifecycle_event::quiesce_succeeded,
        lifecycle_event::unload_succeeded,
        lifecycle_event::discard_candidate,
        lifecycle_event::fail_restart_required,
    };

    for (const plugin_state state : states) {
        for (const lifecycle_event event : events) {
            const auto result = transition(state, event);
            if (result.error != FRAME_OK) {
                EXPECT_EQ(result.error, FRAME_ERR_INVALID_ARGUMENT);
                EXPECT_EQ(result.next, state);
            }
        }
    }
}

TEST(LifecycleState, RestartRequiredIsTerminal) {
    const auto restart = transition(plugin_state::pausing, lifecycle_event::fail_restart_required);
    ASSERT_EQ(restart.error, FRAME_OK);
    ASSERT_EQ(restart.next, plugin_state::failed_restart_required);

    const auto terminal =
        transition(plugin_state::failed_restart_required, lifecycle_event::begin_quiesce);
    EXPECT_EQ(terminal.error, FRAME_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(terminal.next, plugin_state::failed_restart_required);
}

} // namespace
