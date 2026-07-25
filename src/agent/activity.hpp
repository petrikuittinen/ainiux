#pragma once

namespace ainiux::agent {

// Provider/UI-neutral lifecycle of an active agent turn. Idle/ready is owned by
// the surface hosting the runtime.
enum class AgentActivityPhase { Thinking, Working };

}  // namespace ainiux::agent
