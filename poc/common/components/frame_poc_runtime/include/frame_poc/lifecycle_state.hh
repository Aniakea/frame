#ifndef FRAME_POC_LIFECYCLE_STATE_HH
#define FRAME_POC_LIFECYCLE_STATE_HH

#include <cstdint>

#include "frame_poc/frame_abi.h"

namespace frame::poc {

enum class plugin_state : uint8_t {
    staged,
    loading,
    prepared,
    active,
    pausing,
    paused,
    quiescing,
    unloading,
    unloaded,
    failed_restart_required,
};

enum class lifecycle_event : uint8_t {
    begin_load,
    prepare_succeeded,
    activation_committed,
    begin_pause,
    pause_succeeded,
    resume_succeeded,
    begin_quiesce,
    quiesce_succeeded,
    unload_succeeded,
    discard_candidate,
    fail_restart_required,
};

struct transition_result {
    frame_err_t error;
    plugin_state next;
};

constexpr bool contains_plugin_code(plugin_state state) {
    return state != plugin_state::staged && state != plugin_state::unloaded &&
           state != plugin_state::failed_restart_required;
}

constexpr transition_result transition(plugin_state current, lifecycle_event event) {
    if (event == lifecycle_event::fail_restart_required && contains_plugin_code(current)) {
        return {FRAME_OK, plugin_state::failed_restart_required};
    }

    switch (current) {
    case plugin_state::staged:
        if (event == lifecycle_event::begin_load) {
            return {FRAME_OK, plugin_state::loading};
        }
        if (event == lifecycle_event::discard_candidate) {
            return {FRAME_OK, plugin_state::unloaded};
        }
        break;
    case plugin_state::loading:
        if (event == lifecycle_event::prepare_succeeded) {
            return {FRAME_OK, plugin_state::prepared};
        }
        if (event == lifecycle_event::discard_candidate) {
            return {FRAME_OK, plugin_state::unloaded};
        }
        break;
    case plugin_state::prepared:
        if (event == lifecycle_event::activation_committed) {
            return {FRAME_OK, plugin_state::active};
        }
        if (event == lifecycle_event::discard_candidate) {
            return {FRAME_OK, plugin_state::unloaded};
        }
        break;
    case plugin_state::active:
        if (event == lifecycle_event::begin_pause) {
            return {FRAME_OK, plugin_state::pausing};
        }
        if (event == lifecycle_event::begin_quiesce) {
            return {FRAME_OK, plugin_state::quiescing};
        }
        break;
    case plugin_state::pausing:
        if (event == lifecycle_event::pause_succeeded) {
            return {FRAME_OK, plugin_state::paused};
        }
        break;
    case plugin_state::paused:
        if (event == lifecycle_event::resume_succeeded) {
            return {FRAME_OK, plugin_state::active};
        }
        if (event == lifecycle_event::begin_quiesce) {
            return {FRAME_OK, plugin_state::quiescing};
        }
        break;
    case plugin_state::quiescing:
        if (event == lifecycle_event::quiesce_succeeded) {
            return {FRAME_OK, plugin_state::unloading};
        }
        break;
    case plugin_state::unloading:
        if (event == lifecycle_event::unload_succeeded) {
            return {FRAME_OK, plugin_state::unloaded};
        }
        break;
    case plugin_state::unloaded:
    case plugin_state::failed_restart_required:
        break;
    }

    return {FRAME_ERR_INVALID_ARGUMENT, current};
}

} // namespace frame::poc

#endif