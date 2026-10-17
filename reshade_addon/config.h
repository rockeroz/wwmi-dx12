#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "instrumentation/diag.h"

// ---- master -----------------------------------------------------------------------------------
inline std::atomic<bool> g_addon_disabled{ false }; // every hot handler early-outs: A/B against vanilla
inline std::atomic<bool> g_rt_blocked{ false };     // the game ray traces and this build has no RT module: off until restart
inline std::atomic<bool> g_faulted{ false };        // a guarded handler faulted (crash_handler.cpp): off until restart
inline std::wstring      g_cso_dir;                 // where our compiled shaders live
inline std::wstring      g_shader_cache_dir;        // patched shaders kept across launches; empty = no cache
inline std::wstring      g_data_dir;                // <Win64>\wwmi_dx12: our caches and crawls; empty = none

// ---- injection --------------------------------------------------------------------------------
inline bool                  g_disable_vb = false;
inline uint64_t              g_hide_vs_hash = 0;      // hide every draw with this VS hash (0 = off)
inline bool                  g_wwmi_geom = true;      // bind the mod's shipped buffers for DIRECT comps
inline bool                  g_port_draw_list = true; // give their engine the draw's command list
inline bool                  g_crash_handler = true;  // last-chance reporter -> wwmi_dx12_crash.log
inline bool                  g_stall_watchdog = true;
inline unsigned              g_stall_ms = 1000;
inline std::atomic<uint32_t> g_stall_count{ 0 };
inline std::atomic<bool>     g_skeldump_req{ false };  // overlay button -> dump the merged skeleton
inline std::atomic<bool>     g_skelchain_req{ false }; // overlay button -> dump the whole skeleton chain
inline std::atomic<bool>     g_chaintrace_req{ false };
inline std::atomic<int>      g_chaintrace_frames{ 0 };
inline FrameCounter          g_port_fellback_count;
inline FrameCounter          g_port_sect_runs;
inline FrameCounter          g_port_sect_nocb;
inline bool                  g_lod_pin = false;           // recognise the char's lower-LOD draws (log only)
inline bool                  g_dump_game_textures = true; // frame dump includes the game's bindless textures
inline bool                  g_shader_replace = false;    // swap a game shader for one of ours by hash

inline bool              g_decensor = false;
inline bool              g_hide_uid = false;         // A5: the UID cut out of the UI text (hud/uid_hide.h)
inline std::wstring      g_addon_ini_path;           // wwmi_dx12.ini, read at startup and written back by the overlay
inline bool              g_skip_engine_tick = false; // A9: perf A/B only -- no [Present] lists, no present pass; mods stop updating
inline bool              g_frame_end_step = true;    // presents from another thread (frame generation): the per-frame step at the game's own frame end (Numpad 7)
inline bool              g_hide_outline = false;
inline std::atomic<bool> g_reload_requested{ false }; // overlay -> reload at the next frame boundary

inline std::atomic<uint32_t> g_rfx_glow_count{ 0 };  // patched draws that ran with the glow block's maps bound
inline std::atomic<uint32_t> g_rfx_glow_params{ 0 }; // components the ini armed (h/brightness/GlowMap)
inline bool                  g_rfx_patch = true;
inline std::atomic<uint32_t> g_rfx_patch_draws{ 0 };  // draws issued on a patched variant
inline std::atomic<uint32_t> g_rfx_patch_nobind{ 0 }; // variant existed, its maps could not be bound: drawn unpatched

// ---- shape keys / morphs ----------------------------------------------------------------------
inline uint64_t              g_morph_cs_hash = 0x4e91ecddc203a6cfULL;
inline uint64_t              g_clear_cs_hash = 0x099222f33b1f30e6ULL;
inline bool                  g_shader_hash_alias = true;
inline bool                  g_publish_shader_state = true;
inline bool                  g_publish_cs_u0 = true;
inline bool                  g_sk_run_passes = true;    // OFF explodes Applier mods: these passes write vb0
inline bool                  g_sk_freeze = false;       // run the chain, zero the deltas: the morph blocker
inline bool                  g_health_heartbeat = true; // log the counters that GROW, every 600 frames
inline std::atomic<uint32_t> g_wwmi_geom_count{ 0 };

// ---- custom-material sub-draws ----------------------------------------------------------------
inline bool                  g_custom_subdraw = true; // render `run = [CustomShader*]` parts
inline bool                  g_custom_blend = true;   // clone the material PSO with the ini's blend
inline bool                  g_wcl_draw = true;       // execute ini command lists (off = none run)
inline std::atomic<uint32_t> g_custom_blend_count{ 0 };
inline std::atomic<uint32_t> g_custom_blend_bound_count{ 0 };
inline float                 g_custom_blend_factor = -1.0f;     // -1 = use the ini's factor
inline bool                  g_custom_blend_debug_zero = false; // src=0,dst=1: show the background behind the piece

// ---- shadow -----------------------------------------------------------------------------------
inline bool             g_shadow_vanilla = false;  // release depth-only passes to the game
inline bool             g_shadow_via_claim = true; // claim them instead; needs g_shadow_vanilla off
inline std::atomic<int> g_shadow_recog_watch{ 0 }; // frames left on the SHADOWRECOG watch
inline std::atomic<int> g_sectex_watch{ 0 };       // frames left logging every texture a section sees (SECTEX)

inline bool g_reloc_forget_on_destroy = true;

inline bool                  g_push_game_vb = true;
inline std::atomic<uint64_t> g_vb6_readers_claimed{ 0 };
inline std::atomic<uint64_t> g_vb6_readers_unclaimed{ 0 };

inline std::atomic<const void*> g_claim_last_list{ nullptr };
inline std::atomic<const void*> g_claim_exec_queue{ nullptr };
inline std::atomic<uint32_t>    g_claim_thread{ 0 };
inline std::atomic<uint64_t>    g_claim_exec_seen{ 0 };
inline std::atomic<bool>        g_game_frame_submitted{ false }; // a list with a game draw went to the GPU since the last step
inline std::atomic<uint32_t>    g_game_submit_tid{ 0 };          // the thread that submits the game's drawing lists
inline std::atomic<uint64_t>    g_game_submit_queue{ 0 };        // ...and the queue they go to (native)
inline std::atomic<uint32_t>    g_presenter_tid{ 0 };
inline std::atomic<void*>       g_present_queue{ nullptr };      // native: the redraw's lists run there
inline std::atomic<bool>        g_frame_end_armed{ false };      // presents come from another thread: the game's frame end is watched for
inline std::atomic<uint32_t>    g_bb_width{ 0 };
inline std::atomic<uint32_t>    g_bb_height{ 0 };
inline std::atomic<uint64_t>    g_frame_end_step_ms{ 0 };        // when the step last ran at the game's frame end
inline std::atomic<bool>        g_frame_end_stepped{ false };    // ...and that it did since the last present
inline std::atomic<uint64_t>    g_presentq_same{ 0 };
inline std::atomic<uint64_t>    g_presentq_diff{ 0 };
inline bool                     g_present_on_game_queue = false;
inline bool                     g_present_flush_immediate_first = true;
inline bool                     g_ibhash_gpu_skip = false;
inline bool                     g_tex_deferred = true; // mod textures load off the engine lock, vanilla until in (A12)
inline std::atomic<uint64_t>    g_freeze_binds{ 0 };
inline std::atomic<uint64_t>    g_x32_mid_total{ 0 };
inline std::atomic<uint64_t>    g_x32_mid_skipped_total{ 0 };
// Drop shapekey passes whose dispatch named no mod, rather than guessing the owner by hash/name.
inline bool             g_sk_strict_owner = true;
inline std::atomic<int> g_vbdiag_frames{ 0 };
inline bool             g_skel_copies = true;             // one merged skeleton per copy of a character in a frame (B20)
inline int              g_dbg_comp5_suppress_rtv = -1;    // bow A/B: drop comp5 passes with this rtv count
inline bool             g_skip_translucent_cloth = false; // bow A/B: drop the modded comp's 2-RT pass

inline bool g_shader_override_dispatch = true;
// d3dx.ini's own allow_buffer_resize, read from their parsed config (PortProbeTick).
inline bool g_allow_buffer_resize = true;

// ---- shapekey chain ---------------------------------------------------------------------------
inline bool                  g_sk_cb0_snapshot = true;
inline bool                  g_sk_u0_size_recover = true;
inline bool                  g_sk_u0_clear = true;      // the Loader accumulates: u0 must start zeroed
inline bool                  g_sk_u1_clear = true;      // else u1 wraps negative and the Multiplier snaps
inline bool                  g_sk_clear_barrier = true; // a UAV barrier does not order the zero copy
inline bool                  g_sk_honour_skip = false;  // cancel the game's dispatch when the section asked
inline bool                  g_sk_engine_dispatch = false;
inline bool                  g_sk_cancel_game_clear = true;
inline std::atomic<unsigned> g_sk_clear_cancelled{ 0 };  // clears cancelled so far (overlay I18)
inline bool                  g_sk_u1_by_identity = true; // resolve u1 by declared ShapeKeyScale identity

inline bool                  g_tex_lineage = true;
inline std::atomic<uint32_t> g_tex_lineage_count{ 0 };
inline bool                  g_texlod_alias_off = false;
inline bool                  g_texlod_resemblance = true;  // A13 / ini lod_resemblance: look-alike LOD matches applied (always learned)
inline bool                  g_texlod_cache = true;        // A14 / ini lod_cache: LOD matches learned, kept and used; off: none read, learned or written
inline bool                  g_mip_exact = true;
inline bool                  g_dump_degenerates = false; // D15: each degenerate LOD low as <dump dir>\degenerates\<hash>.dds
inline bool                  g_rabbitfx_tex = true;
inline bool                  g_rabbitfx_tex_main = true;    // C4S: bind only into shaders that declare t0, t1 and t2
inline bool                  g_rabbitfx_tex_gbuffer = true; // C4G: bind only in GBuffer passes (4+ render targets), as E2
inline std::atomic<uint32_t> g_rabbitfx_tex_count{ 0 };
inline bool                  g_cmd_reset_clears = true;
inline std::atomic<uint32_t> g_cmd_resets{ 0 };
inline bool                  g_charvb_refuse_xmod = false;
inline std::atomic<uint32_t> g_charvb_claims{ 0 };
inline std::atomic<uint32_t> g_charvb_xmod{ 0 };
inline std::atomic<uint32_t> g_charvb_refused{ 0 };
inline bool                  g_ib_anchor_claim = true;
inline std::atomic<uint32_t> g_ib_anchor_learned{ 0 }; // index buffers anchored to a mod this session
inline std::atomic<uint32_t> g_ib_anchor_claims{ 0 };  // draws claimed because of one
inline bool                  g_ps_slot_stamp = true;
inline bool                  g_ps_slot_stamp_nulls = true;
inline std::atomic<uint32_t> g_ps_slot_stamp_count{ 0 };
inline bool                  g_ibhash_suppress = false;
inline std::atomic<uint32_t> g_ibhash_suppress_count{ 0 };

inline FrameCounter g_draw_replace_count;
inline uint64_t     g_last_replace_hash = 0;
inline uint32_t     g_last_replace_ib_count = 0;
inline bool         g_dx12_debug_layer = false; // takes effect on the next launch
