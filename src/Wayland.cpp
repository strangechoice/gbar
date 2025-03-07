#include "Wayland.h"

#include "Common.h"
#include "Config.h"
#include <wayland-client.h>
#include <ext-workspace-unstable-v1.h>

namespace Wayland
{
    // There's probably a better way to avoid the LUTs
    static std::unordered_map<uint32_t, Monitor> monitors;
    static std::unordered_map<zext_workspace_group_handle_v1*, WorkspaceGroup> workspaceGroups;
    static std::unordered_map<zext_workspace_handle_v1*, Workspace> workspaces;

    static uint32_t curID = 0;

    static wl_display* display;
    static wl_registry* registry;
    static zext_workspace_manager_v1* workspaceManager;

    static bool registeredMonitors = false;
    static bool registeredGroup = false;
    static bool registeredWorkspace = false;
    static bool registeredWorkspaceInfo = false;

    // Wayland callbacks

    // Workspace Callbacks
    static void OnWorkspaceName(void*, zext_workspace_handle_v1* workspace, const char* name)
    {
        workspaces[workspace].id = std::stoul(name);
        LOG("Workspace ID: " << workspaces[workspace].id);
        registeredWorkspaceInfo = true;
    }
    static void OnWorkspaceGeometry(void*, zext_workspace_handle_v1*, wl_array*) {}
    static void OnWorkspaceState(void*, zext_workspace_handle_v1* ws, wl_array* arrState)
    {
        Workspace& workspace = workspaces[ws];
        ASSERT(workspace.parent, "Wayland: Workspace not registered!");
        WorkspaceGroup& group = workspaceGroups[workspace.parent];

        workspace.active = false;
        // Manual wl_array_for_each, since that's broken for C++
        for (zext_workspace_handle_v1_state* state = (zext_workspace_handle_v1_state*)arrState->data;
             (uint8_t*)state < (uint8_t*)arrState->data + arrState->size; state += 1)
        {
            if (*state == ZEXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
            {
                LOG("Wayland: Activate Workspace " << workspace.id);
                group.lastActiveWorkspace = ws;
                workspace.active = true;
            }
        }
        if (!workspace.active)
        {
            LOG("Wayland: Deactivate Workspace " << workspace.id);
        }
    }
    static void OnWorkspaceRemove(void*, zext_workspace_handle_v1* ws)
    {
        Workspace& workspace = workspaces[ws];
        ASSERT(workspace.parent, "Wayland: Workspace not registered!");
        WorkspaceGroup& group = workspaceGroups[workspace.parent];
        auto it = std::find(group.workspaces.begin(), group.workspaces.end(), ws);
        group.workspaces.erase(it);

        workspaces.erase(ws);

        LOG("Wayland: Removed workspace!");
    }
    zext_workspace_handle_v1_listener workspaceListener = {OnWorkspaceName, OnWorkspaceGeometry, OnWorkspaceState, OnWorkspaceRemove};

    // Workspace Group callbacks
    static void OnWSGroupOutputEnter(void*, zext_workspace_group_handle_v1* group, wl_output* output)
    {
        auto monitor = std::find_if(monitors.begin(), monitors.end(),
                                    [&](const std::pair<uint32_t, Monitor>& mon)
                                    {
                                        return mon.second.output == output;
                                    });
        ASSERT(monitor != monitors.end(), "Wayland: Registered WS group before monitor!");
        LOG("Wayland: Added group to monitor");
        monitor->second.workspaceGroup = group;
    }
    static void OnWSGroupOutputLeave(void*, zext_workspace_group_handle_v1*, wl_output* output)
    {
        auto monitor = std::find_if(monitors.begin(), monitors.end(),
                                    [&](const std::pair<uint32_t, Monitor>& mon)
                                    {
                                        return mon.second.output == output;
                                    });
        ASSERT(monitor != monitors.end(), "Wayland: Registered WS group before monitor!");
        LOG("Wayland: Added group to monitor");
        monitor->second.workspaceGroup = nullptr;
    }
    static void OnWSGroupWorkspaceAdded(void*, zext_workspace_group_handle_v1* workspace, zext_workspace_handle_v1* ws)
    {
        LOG("Wayland: Added workspace!");
        workspaceGroups[workspace].workspaces.push_back(ws);
        workspaces[ws] = {workspace, (uint32_t)-1, false};
        zext_workspace_handle_v1_add_listener(ws, &workspaceListener, nullptr);
        registeredWorkspace = true;
    }
    static void OnWSGroupRemove(void*, zext_workspace_group_handle_v1* workspaceGroup)
    {
        workspaceGroups.erase(workspaceGroup);
    }
    zext_workspace_group_handle_v1_listener workspaceGroupListener = {OnWSGroupOutputEnter, OnWSGroupOutputLeave, OnWSGroupWorkspaceAdded,
                                                                      OnWSGroupRemove};

    // Workspace Manager Callbacks
    static void OnWSManagerNewGroup(void*, zext_workspace_manager_v1*, zext_workspace_group_handle_v1* group)
    {
        // Register callbacks for the group.
        registeredGroup = true;
        zext_workspace_group_handle_v1_add_listener(group, &workspaceGroupListener, nullptr);
    }
    static void OnWSManagerDone(void*, zext_workspace_manager_v1*) {}
    static void OnWSManagerFinished(void*, zext_workspace_manager_v1*)
    {
        LOG("Wayland: Workspace manager finished. Disabling workspaces!");
        RuntimeConfig::Get().hasWorkspaces = false;
    }
    zext_workspace_manager_v1_listener workspaceManagerListener = {OnWSManagerNewGroup, OnWSManagerDone, OnWSManagerFinished};

    // Output Callbacks
    // Very bloated, indeed
    static void OnOutputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
    static void OnOutputMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
    static void OnOutputDone(void*, wl_output*) {}
    static void OnOutputScale(void*, wl_output*, int32_t) {}
    static void OnOutputName(void*, wl_output* output, const char* name)
    {
        LOG("Wayland: Registering monitor " << name << " at ID " << curID);
        registeredMonitors = true;
        monitors.try_emplace(curID++, Monitor{name, output, nullptr});
    }
    static void OnOutputDescription(void*, wl_output*, const char*) {}
    wl_output_listener outputListener = {OnOutputGeometry, OnOutputMode, OnOutputDone, OnOutputScale, OnOutputName, OnOutputDescription};

    // Registry Callbacks
    static void OnRegistryAdd(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
    {
        if (strcmp(interface, "wl_output") == 0)
        {
            wl_output* output = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 4);
            wl_output_add_listener(output, &outputListener, nullptr);
        }
        if (strcmp(interface, "zext_workspace_manager_v1") == 0 && !Config::Get().useHyprlandIPC)
        {
            workspaceManager = (zext_workspace_manager_v1*)wl_registry_bind(registry, name, &zext_workspace_manager_v1_interface, version);
            zext_workspace_manager_v1_add_listener(workspaceManager, &workspaceManagerListener, nullptr);
        }
    }
    static void OnRegistryRemove(void*, wl_registry*, uint32_t) {}
    wl_registry_listener registryListener = {OnRegistryAdd, OnRegistryRemove};

    // Dispatch events.
    static void Dispatch()
    {
        wl_display_roundtrip(display);
    }
    static void WaitFor(bool& condition)
    {
        while (!condition && wl_display_dispatch(display) != -1)
        {
        }
    }

    void Init()
    {
        display = wl_display_connect(nullptr);
        ASSERT(display, "Cannot connect to wayland compositor!");
        registry = wl_display_get_registry(display);
        ASSERT(registry, "Cannot get wayland registry!");

        wl_registry_add_listener(registry, &registryListener, nullptr);
        wl_display_roundtrip(display);

        WaitFor(registeredMonitors);
        registeredMonitors = false;

        if (!workspaceManager && !Config::Get().useHyprlandIPC)
        {
            LOG("Compositor doesn't implement zext_workspace_manager_v1, disabling workspaces!");
            LOG("Note: Hyprland v0.30.0 removed support for zext_workspace_manager_v1, please enable UseHyprlandIPC instead!");
            RuntimeConfig::Get().hasWorkspaces = false;
            return;
        }

        // Hack: manually activate workspace for each monitor
        for (auto& monitor : monitors)
        {
            // Find group
            auto& group = workspaceGroups[monitor.second.workspaceGroup];

            // Find ws with monitor index + 1
            auto workspaceIt = std::find_if(workspaces.begin(), workspaces.end(),
                                            [&](const std::pair<zext_workspace_handle_v1*, Workspace>& ws)
                                            {
                                                return ws.second.id == monitor.first + 1;
                                            });
            if (workspaceIt != workspaces.end())
            {
                LOG("Forcefully activate workspace " << workspaceIt->second.id)
                if (workspaceIt->second.id == 1)
                {
                    // Activate first workspace
                    workspaceIt->second.active = true;
                }
                // Make it visible
                group.lastActiveWorkspace = workspaceIt->first;
            }
        }
    }

    void PollEvents()
    {
        // Dispatch events
        Dispatch();
        if (registeredGroup)
        {
            // New Group, wait for workspaces to be registered.
            WaitFor(registeredWorkspace);
        }
        if (registeredWorkspace)
        {
            // New workspace added, need info
            WaitFor(registeredWorkspaceInfo);
        }
        registeredGroup = false;
        registeredWorkspace = false;
        registeredWorkspaceInfo = false;
        return;
    }

    void Shutdown()
    {
        if (display)
            wl_display_disconnect(display);
    }

    const std::unordered_map<uint32_t, Monitor>& GetMonitors()
    {
        return monitors;
    }
    const std::unordered_map<zext_workspace_group_handle_v1*, WorkspaceGroup>& GetWorkspaceGroups()
    {
        return workspaceGroups;
    }
    const std::unordered_map<zext_workspace_handle_v1*, Workspace>& GetWorkspaces()
    {
        return workspaces;
    }
}
