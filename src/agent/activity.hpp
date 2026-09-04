#pragma once

#include <cstddef>
#include <string>

namespace ainiux::agent {

// Provider/UI-neutral lifecycle of an active agent turn. Idle/ready is owned by
// the surface hosting the runtime.
enum class AgentActivityPhase { Thinking, Working };

enum class PreparationPhase {
    IndexProbe,
    ToolSetup,
    SessionDatabase,
    History,
    ProjectInstructions,
};

const char* preparation_phase_name(PreparationPhase phase);

struct PreparationProgress {
    PreparationPhase phase = PreparationPhase::IndexProbe;
    bool completed = false;
    long long phase_elapsed_ms = 0;
    long long total_elapsed_ms = 0;
};

enum class AgentProgressAction { Upsert, Append, Commit, Discard };
enum class AgentProgressKind { Thinking, Tool, Notice, Response };

struct AgentProgressUpdate {
    AgentProgressAction action = AgentProgressAction::Upsert;
    AgentProgressKind kind = AgentProgressKind::Notice;
    std::size_t round_id = 0;
    std::size_t tool_id = 0;
    std::string text;
    long long created_at_ms = 0;
};

}  // namespace ainiux::agent
