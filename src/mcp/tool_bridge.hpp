#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.hpp"
#include "mcp/client.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::mcp {

class ToolBridge {
   public:
    ToolBridge();
    ~ToolBridge();
    ToolBridge(const ToolBridge&) = delete;
    ToolBridge& operator=(const ToolBridge&) = delete;
    ToolBridge(ToolBridge&&) noexcept;
    ToolBridge& operator=(ToolBridge&&) noexcept;

    void set_manager(std::shared_ptr<Manager> manager);
    std::shared_ptr<Manager> manager() const;

    Error refresh(runtime::CancellationToken cancellation = {});

    std::vector<provider::FunctionDefinition> definitions() const;
    const std::vector<std::string>& last_errors() const;

    bool is_mcp_tool(const std::string& name) const;
    std::string execute(const std::string& qualified_name,
                        const std::string& arguments_json,
                        runtime::CancellationToken cancellation = {}) const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ainiux::mcp
