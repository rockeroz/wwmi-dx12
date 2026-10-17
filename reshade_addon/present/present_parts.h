#pragma once
// The present path's parts, called in order by on_present_body (present_hook.cpp).
#include <cstdint>
#include <mutex>
#include <reshade.hpp>
#include <string>
#include "../instrumentation/diag.h"

void               PresentQueueDiag(reshade::api::command_queue* queue);                             // present_diag.cpp
void               PortProbeTick(reshade::api::command_queue* queue, reshade::api::swapchain* sc);   // present_engine.cpp
void               ConfigWatchTick();                                                                // present_diag.cpp
void               SeedHeartbeat();                                                                  // present_diag.cpp
// The per-frame engine step; with no back buffer bound, [Present]'s draws to it are left to EngineRedrawTick.
void               EngineFrameTick(reshade::api::command_queue* queue, reshade::api::swapchain* sc, bool bind_back_buffer); // present_engine.cpp
void               EngineRedrawTick(reshade::api::command_queue* queue, reshade::api::swapchain* sc); // present_engine.cpp
// Held through a step and a redraw: they share the port's frame context, and with frame generation run on two threads.
std::mutex&        EngineFrameMutex(); // present_engine.cpp
void               PresenceTick();                                                                   // presence_tick.cpp
void               ModTexturesTick();                                                                // present_mod_load.cpp
void               Heartbeat(uint32_t texinj_last);                                                  // present_diag.cpp
void               HealthHeartbeat();                                                                // present_diag.cpp
void               LoadGeometryMods();                                                               // present_mod_load.cpp

extern std::wstring g_cfg_engine_path;
extern std::wstring g_cfg_db_path;

void on_present_body(reshade::api::command_queue* queue, reshade::api::swapchain* sc);
// The game submitted the list that ends its frame, on its own thread while presents come from another one (frame
// generation): the per-frame step runs here, before that list reaches the queue (present_hook.cpp).
void FrameEndStepGuarded(reshade::api::command_queue* queue);
// The swapchain is gone: no step reaches it any more (present_hook.cpp).
void FrameEndForgetSwapchain();
