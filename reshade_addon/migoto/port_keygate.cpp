// ===========================================================================
//  port_keygate -- OURS.
// ===========================================================================
#include "port_keygate.h"
#include "port_api.h"
#include "port_api_internal.h" // LoadDeferredNow
#include "dx12_wrap.h"         // NoteTextureUse
#include "prof_core.h"
#include "CommandList.h" // the parsed command types the typed observer reads (theirs, unmodified)
#include "input.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <typeinfo>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>
#include "../../src/log.h" // SHIM-UNMET goes to the addon log, not the trace

// Their global, defined in port_globals.cpp: the module their paths are relative to.
extern HINSTANCE migoto_handle;

namespace MigotoPort {

namespace {

std::mutex& Mtx()
{
    static std::mutex* m = new std::mutex();
    return *m;
}
bool (*g_gate)(const wchar_t*) = nullptr;
std::atomic<size_t> g_gated{ 0 };
std::atomic<size_t> g_wrapped_quiet{ 0 };
thread_local bool   t_bound_loading = false; // PortApi::BoundLoading

} // namespace

namespace {
std::wstring& IniRootStore()
{
    static auto* s = new std::wstring();
    return *s;
}
} // namespace

void SetIniRoot(const wchar_t* dir)
{
    std::lock_guard<std::mutex> l(Mtx());
    if (!dir || !*dir) {
        IniRootStore().clear();
        return;
    }
    std::wstring d(dir);
    if (d.back() != L'\\' && d.back() != L'/') d += L'\\'; // their code appends the filename directly
    IniRootStore() = d;
}

const wchar_t* IniRoot()
{
    std::lock_guard<std::mutex> l(Mtx());
    return IniRootStore().empty() ? nullptr : IniRootStore().c_str();
}

void MigotoDir(wchar_t* out, unsigned long count)
{
    if (!out || !count) return;
    if (const wchar_t* root = IniRoot()) {
        wcsncpy_s(out, count, root, _TRUNCATE);
        return;
    }
    if (!PortRealGetModuleFileNameW(migoto_handle, out, count)) {
        out[0] = 0;
        return;
    }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash)
        slash[1] = 0;
    else
        out[0] = 0;
}

namespace {
thread_local CommandObserver g_observer = nullptr;
thread_local void*           g_observer_user = nullptr;
// Same thread-locality requirement, same reason: it points at the caller's stack frame.
thread_local TypedCommandObserver g_typed = nullptr;
thread_local void*                g_typed_user = nullptr;
} // namespace

void SetCommandObserver(CommandObserver fn, void* user)
{
    g_observer = fn;
    g_observer_user = user;
}

void SetTypedCommandObserver(TypedCommandObserver fn, void* user)
{
    g_typed = fn;
    g_typed_user = user;
}

static bool ClassifyCommand(const CommandListCommand* cmd, ::CommandListState* state, ObservedCmd& o)
{
    if (const RunExplicitCommandList* e = dynamic_cast<const RunExplicitCommandList*>(cmd)) {
        o.kind = ObservedCmd::Kind::RunList;
        o.run_explicit = (const void*)e->command_list_section;
    } else if (const RunLinkedCommandList* r = dynamic_cast<const RunLinkedCommandList*>(cmd)) {
        o.kind = ObservedCmd::Kind::RunList;
        if (r->link) o.run_target = r->link->ini_section.c_str();
    } else if (const ResourceCopyOperation* c = dynamic_cast<const ResourceCopyOperation*>(cmd)) {
        o.kind = ObservedCmd::Kind::ResourceCopy;
        o.dst_type = (int)c->dst.type;
        o.dst_shader = c->dst.shader_type;
        o.dst_slot = !c->dst.slot_expression ? c->dst.slot
            : state                          ? const_cast<ResourceCopyTarget&>(c->dst).ResolveSlot(state)
                                             : UINT_MAX;
        o.dst_custom = state ? (const void*)const_cast<ResourceCopyTarget&>(c->dst).GetCustomResource(state)
                             : nullptr;
        o.src_type = (int)c->src.type;
        o.src_custom = state ? (const void*)const_cast<ResourceCopyTarget&>(c->src).GetCustomResource(state)
                             : nullptr;
    } else {
        return false; // nothing the addon acts on
    }
    return true;
}

static bool StaticTargets(const CommandListCommand* cmd)
{
    const ResourceCopyOperation* c = dynamic_cast<const ResourceCopyOperation*>(cmd);
    return !c || (!c->dst.pool_dynamic_index_expression && !c->src.pool_dynamic_index_expression && !c->dst.slot_expression && !c->src.slot_expression);
}

void ObserveCommand(const wchar_t* ini_line)
{
    if (g_observer && ini_line) g_observer(ini_line, g_observer_user);
}

static thread_local const wchar_t* t_cur_ini_line = nullptr;
const wchar_t*                     CurrentIniLine()
{
    return t_cur_ini_line;
}

static thread_local std::vector<CommandListCommand*>* t_trace = nullptr;
static bool (*g_run_explicit_hook)(CommandListCommand*, ::CommandListState*) = nullptr;
void SetRunExplicitHook(bool (*fn)(CommandListCommand*, ::CommandListState*))
{
    g_run_explicit_hook = fn;
}
std::vector<CommandListCommand*>* SetCommandTrace(std::vector<CommandListCommand*>* trace)
{
    std::vector<CommandListCommand*>* const prev = t_trace;
    t_trace = trace;
    return prev;
}

bool ObserverActive()
{
    return g_observer != nullptr;
}

bool AnyObserver()
{
    return g_observer != nullptr || g_typed != nullptr;
}

namespace {
std::atomic<unsigned>            g_sel_inputs{ 1 };
std::atomic<unsigned>            g_sel_vars_gen{ 1 };
std::mutex                       g_sel_vars_mtx;
std::unordered_set<const float*> g_sel_vars;
struct SeenVar {
    const float* v;
    uint32_t     bits;
};
std::vector<SeenVar>            g_sel_seen;
std::unordered_set<const void*> g_sel_res;
}
unsigned SelectionInputsGen()
{
    return g_sel_inputs.load(std::memory_order_acquire);
}
void BumpSelectionInputs()
{
    g_sel_inputs.fetch_add(1, std::memory_order_acq_rel);
}
void WatchSelectionVar(const float* v)
{
    std::lock_guard<std::mutex> l(g_sel_vars_mtx);
    if (!g_sel_vars.insert(v).second) return;
    SeenVar s{ v, 0 };
    memcpy(&s.bits, v, sizeof s.bits);
    g_sel_seen.push_back(s);
    g_sel_vars_gen.fetch_add(1, std::memory_order_acq_rel);
}
bool SelectionVarWatched(const float* v)
{
    std::lock_guard<std::mutex> l(g_sel_vars_mtx);
    return g_sel_vars.count(v) != 0;
}
unsigned SelectionVarsGen()
{
    return g_sel_vars_gen.load(std::memory_order_acquire);
}
void SweepSelectionVars()
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> l(g_sel_vars_mtx);
        for (SeenVar& s : g_sel_seen) {
            uint32_t now;
            memcpy(&now, s.v, sizeof now);
            if (now == s.bits) continue;
            s.bits = now;
            changed = true;
        }
    }
    if (changed) BumpSelectionInputs();
}
void WatchSelectionRes(const void* cr)
{
    std::lock_guard<std::mutex> l(g_sel_vars_mtx);
    if (g_sel_res.insert(cr).second) g_sel_vars_gen.fetch_add(1, std::memory_order_acq_rel);
}
static bool SelectionResWatched(const void* cr)
{
    std::lock_guard<std::mutex> l(g_sel_vars_mtx);
    return g_sel_res.count(cr) != 0;
}
void ClearSelectionVars()
{
    {
        std::lock_guard<std::mutex> l(g_sel_vars_mtx);
        g_sel_vars.clear();
        g_sel_seen.clear();
        g_sel_res.clear();
    }
    g_sel_vars_gen.fetch_add(1, std::memory_order_acq_rel);
    BumpSelectionInputs();
}

namespace {
std::mutex& UnmetMtx()
{
    static std::mutex* m = new std::mutex();
    return *m;
}
std::unordered_map<std::wstring, unsigned>& UnmetSeen()
{
    static auto* m = new std::unordered_map<std::wstring, unsigned>();
    return *m;
}
std::atomic<unsigned> g_unmet_frame{ 0 };
} // namespace

void ShimUnmet(const wchar_t* what, const wchar_t* detail)
{
    // The count feeds only the debug heartbeat and the key/mutex/map only the log -- both off with logging off.
    if (!logger::enabled(1)) return;
    g_unmet_frame.fetch_add(1, std::memory_order_relaxed);
    std::wstring key = std::wstring(what ? what : L"?") + L"|" + (detail ? detail : L"");
    bool         first;
    {
        std::lock_guard<std::mutex> l(UnmetMtx());
        first = (++UnmetSeen()[key] == 1);
    }
    if (first)
        logger::write("SHIM-UNMET: %S (%S) -- the mod asked and we bound nothing; the game's own binding stays",
            what ? what : L"?", detail ? detail : L"");
}

unsigned ShimUnmetTakeCount()
{
    return g_unmet_frame.exchange(0, std::memory_order_relaxed);
}

namespace {
class GatedKeyListener : public InputListener {
    std::shared_ptr<InputListener> inner_;
    std::wstring                   ns_;

public:
    GatedKeyListener(std::shared_ptr<InputListener> inner, const std::wstring& ns)
        : inner_(std::move(inner)),
          ns_(ns)
    {}
    void DownEvent(HackerDevice* device) override
    {
        bool (*gate)(const wchar_t*);
        {
            std::lock_guard<std::mutex> l(Mtx());
            gate = g_gate;
        }
        if (gate && !gate(ns_.c_str())) return;
        inner_->DownEvent(device);
    }
    void UpEvent(HackerDevice* device) override { inner_->UpEvent(device); }
};

struct CmdCost {
    std::atomic<uint64_t> calls{ 0 };
    std::atomic<uint64_t> incl{ 0 };
    std::atomic<uint64_t> self{ 0 };
};

std::mutex& CostMtx()
{
    static std::mutex* m = new std::mutex();
    return *m;
}
std::deque<CmdCost>& Costs()
{
    static auto* d = new std::deque<CmdCost>();
    return *d;
}
std::vector<std::string>& CostNames()
{
    static auto* v = new std::vector<std::string>();
    return *v;
}
std::vector<uint64_t>& CostBase()
{
    static auto* v = new std::vector<uint64_t>();
    return *v;
}
thread_local uint64_t t_cmd_child = 0;

const char* TargetName(ResourceCopyTargetType t)
{
    switch (t) {
    case ResourceCopyTargetType::EMPTY: return "null";
    case ResourceCopyTargetType::CONSTANT_BUFFER: return "cb";
    case ResourceCopyTargetType::SHADER_RESOURCE: return "srv";
    case ResourceCopyTargetType::VERTEX_BUFFER: return "vb";
    case ResourceCopyTargetType::INDEX_BUFFER: return "ib";
    case ResourceCopyTargetType::RENDER_TARGET: return "rt";
    case ResourceCopyTargetType::DEPTH_STENCIL_TARGET: return "dsv";
    case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW: return "uav";
    case ResourceCopyTargetType::CUSTOM_RESOURCE: return "Resource";
    case ResourceCopyTargetType::INI_PARAMS: return "IniParams";
    case ResourceCopyTargetType::THIS_RESOURCE: return "this";
    default: return "other";
    }
}

std::string CostKindName(CommandListCommand* c)
{
    if (dynamic_cast<IfCommand*>(c)) return "if";
    if (dynamic_cast<RunExplicitCommandList*>(c)) return "run = CommandList";
    if (dynamic_cast<RunCustomShaderCommand*>(c)) return "run = CustomShader";
    if (dynamic_cast<ParamOverride*>(c)) return "IniParam = expr";
    if (dynamic_cast<VariableAssignment*>(c)) return "$var = expr";
    if (dynamic_cast<DrawCommand*>(c)) return "draw";
    if (dynamic_cast<CheckTextureOverrideCommand*>(c)) return "checktextureoverride";
    if (dynamic_cast<ClearViewCommand*>(c)) return "clear";
    if (const ResourceCopyOperation* rc = dynamic_cast<const ResourceCopyOperation*>(c)) {
        const int   opts = (int)rc->options;
        const char* mode = (opts & (int)ResourceCopyOptions::REFERENCE) ? "ref"
            : (opts & (int)ResourceCopyOptions::COPY_DESC)              ? "copy_desc"
            : (opts & (int)ResourceCopyOptions::COPY_MASK)              ? "copy"
                                                                        : "assign";
        return std::string("resource ") + mode + " " + TargetName(rc->src.type) + " -> " + TargetName(rc->dst.type);
    }
    std::string n = typeid(*c).name();
    if (n.rfind("class ", 0) == 0) n.erase(0, 6);
    return n;
}

CmdCost* CostKind(CommandListCommand* c)
{
    const std::string           name = CostKindName(c);
    std::lock_guard<std::mutex> l(CostMtx());
    auto&                       names = CostNames();
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return &Costs()[i];
    names.push_back(name);
    return &Costs().emplace_back();
}

void CostEnd(CmdCost& c, int64_t t0, uint64_t saved)
{
    const uint64_t el = (uint64_t)(prof_qpc_now() - t0);
    const uint64_t self = el > t_cmd_child ? el - t_cmd_child : 0;
    c.calls.fetch_add(1, std::memory_order_relaxed);
    c.incl.fetch_add(el, std::memory_order_relaxed);
    c.self.fetch_add(self, std::memory_order_relaxed);
    t_cmd_child = saved + el;
}

class ObservedCommand : public CommandListCommand {
public:
    std::shared_ptr<CommandListCommand> inner;
    ObservedCmd                         cached;            // classified once here, when the targets are static
    bool                                relevant = false;  // the typed observer hears of it at all
    bool                                cached_ok = false; // `cached` stands; else classified per run with the state
    const float*                        sel_var = nullptr;
    bool                                sel_always = false;
    bool                                sel_watched = false;
    unsigned                            sel_watch_gen = 0;
    CustomResource*                     sel_res = nullptr;
    CustomResource*                     file_src = nullptr; // a file texture this copies somewhere a draw does not sample it
    CustomResource*                     slot_src = nullptr; // ... or into a draw's slot
    ResourceCopyOperation*              this_copy = nullptr;
    bool                                is_if = false;
    bool                                is_run = false;
    CmdCost* cost = nullptr;
    explicit ObservedCommand(std::shared_ptr<CommandListCommand> c)
        : inner(std::move(c))
    {
        ini_line = inner->ini_line;
        CommandListCommand* ic = inner.get();
        cost = CostKind(ic);
        is_if = dynamic_cast<IfCommand*>(ic) != nullptr;
        is_run = dynamic_cast<RunExplicitCommandList*>(ic) != nullptr;
        if (VariableAssignment* va = dynamic_cast<VariableAssignment*>(ic)) {
            if (va->var)
                sel_var = &va->var->fval;
            else
                sel_always = true;
        } else if (StoreCommand* sc = dynamic_cast<StoreCommand*>(ic)) {
            if (sc->var)
                sel_var = &sc->var->fval;
            else
                sel_always = true;
        } else if (dynamic_cast<PoolVariableOperation*>(ic) || dynamic_cast<CopyCommandListCommand*>(ic) ||
            dynamic_cast<SlotRangeCopyOperation*>(ic)) {
            sel_always = true;
        } else if (const ResourceCopyOperation* rc = dynamic_cast<const ResourceCopyOperation*>(ic)) {
            sel_always = rc->dst.type == ResourceCopyTargetType::POOL || rc->dst.custom_resource_pool ||
                rc->src.custom_resource_pool;
            if (!sel_always && rc->dst.type == ResourceCopyTargetType::CUSTOM_RESOURCE && !rc->dst.pool_dynamic_index_expression)
                sel_res = const_cast<ResourceCopyTarget&>(rc->dst).GetCustomResource(nullptr);
            if (rc->dst.type == ResourceCopyTargetType::THIS_RESOURCE) this_copy = const_cast<ResourceCopyOperation*>(rc);
            const bool draw_slot = (rc->dst.type == ResourceCopyTargetType::SHADER_RESOURCE && rc->dst.shader_type != L'c') ||
                rc->dst.type == ResourceCopyTargetType::THIS_RESOURCE;
            if (rc->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE && !rc->src.custom_resource_pool) {
                CustomResource* src = const_cast<ResourceCopyTarget&>(rc->src).GetCustomResource(nullptr);
                if (src && !src->filename.empty() && draw_slot)
                    slot_src = src;
                else if (src && !src->filename.empty())
                    file_src = src;
            }
        }
        relevant = ClassifyCommand(inner.get(), nullptr, cached);
        cached_ok = relevant && StaticTargets(inner.get());
        if (cached_ok) {
            if (const ResourceCopyOperation* rc = dynamic_cast<const ResourceCopyOperation*>(inner.get())) {
                cached.dst_custom = const_cast<ResourceCopyTarget&>(rc->dst).GetCustomResource(nullptr);
                cached.src_custom = const_cast<ResourceCopyTarget&>(rc->src).GetCustomResource(nullptr);
            }
        }
    }
    void run(CommandListState* state) override
    {
        const bool timed = g_prof_enabled;
        uint64_t   saved = 0;
        int64_t    t0 = 0;
        if (timed) {
            saved = t_cmd_child;
            t_cmd_child = 0;
            t0 = prof_qpc_now();
        }
        if (g_observer) g_observer(ini_line.c_str(), g_observer_user);
        if (g_typed && relevant) {
            if (cached_ok) {
                ObservedCmd o = cached;
                o.post = state->post;
                g_typed(o, g_typed_user);
            } else {
                ObservedCmd o;
                o.post = state->post;
                if (ClassifyCommand(inner.get(), state, o)) g_typed(o, g_typed_user);
            }
        }
        const wchar_t* outer = t_cur_ini_line;
        t_cur_ini_line = ini_line.c_str();
        uint32_t before = 0;
        if (sel_var) memcpy(&before, sel_var, sizeof before);
        ID3D11Resource*                         res_before = sel_res ? sel_res->resource : nullptr;
        const bool                              null_before = sel_res && sel_res->is_null;
        std::vector<CommandListCommand*>* const trace = is_if ? nullptr : t_trace;
        if (trace) {
            trace->push_back(this);
            t_trace = nullptr;
        }
        if (file_src) {
            PortApi::LoadDeferredNow(file_src);
            NoteTextureUse(file_src->resource);
        }
        if (slot_src) {
            if (!slot_src->substantiated)
                PortApi::DeferTexture(slot_src);
            else
                NoteTextureUse(slot_src->resource);
            if (!slot_src->resource && PortApi::DeferredPending(slot_src)) t_bound_loading = true;
        }
        ResourceCopyOperation* const held = this_copy && state->resource && !state->this_target ? this_copy : nullptr;
        DeferredBinding              bound;
        if (held) held->deferred = &bound;
        if (!is_run || !g_run_explicit_hook || !g_run_explicit_hook(inner.get(), state)) inner->run(state);
        if (held) {
            held->deferred = nullptr;
            if (bound.assigned) *state->resource = bound.resource;
            if (bound.resource) bound.resource->Release();
            if (bound.view) bound.view->Release();
        }
        if (trace) t_trace = trace;
        t_cur_ini_line = outer;
        if (sel_always)
            BumpSelectionInputs();
        else if (sel_res) {
            if ((sel_res->resource != res_before || sel_res->is_null != null_before) && SelectionResWatched(sel_res))
                BumpSelectionInputs();
        } else if (sel_var) {
            uint32_t after;
            memcpy(&after, sel_var, sizeof after);
            if (after != before) {
                const unsigned g = SelectionVarsGen();
                if (g != sel_watch_gen) {
                    sel_watched = SelectionVarWatched(sel_var);
                    sel_watch_gen = g;
                }
                if (sel_watched) BumpSelectionInputs();
            }
        }
        if (timed) CostEnd(*cost, t0, saved);
    }
    bool optimise(HackerDevice* device) override { return inner->optimise(device); }
    bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override
    {
        return inner->noop(post, ignore_cto_pre, ignore_cto_post);
    }
};
} // namespace

std::shared_ptr<InputListener> GateKeyListener(std::shared_ptr<InputListener> inner, const std::wstring& ns)
{
    if (!inner) return inner;
    g_gated.fetch_add(1, std::memory_order_relaxed);
    return std::make_shared<GatedKeyListener>(std::move(inner), ns);
}

namespace {
bool RefFileBackedCopy(ResourceCopyOperation* c)
{
    if (!c || c->dst.type != ResourceCopyTargetType::CUSTOM_RESOURCE ||
        c->src.type != ResourceCopyTargetType::CUSTOM_RESOURCE) return false;
    if (c->dst.custom_resource_pool || c->src.custom_resource_pool) return false;
    const int opts = (int)c->options;
    const int plain = (int)ResourceCopyOptions::COPY | (int)ResourceCopyOptions::UNLESS_NULL;
    if (!(opts & (int)ResourceCopyOptions::COPY) || (opts & ~plain)) return false;
    CustomResource* src = c->src.GetCustomResource(nullptr, true);
    CustomResource* dst = c->dst.GetCustomResource(nullptr, true);
    if (!src || !dst || src->filename.empty()) return false;
    const UINT writes = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT;
    if (dst->bind_flags & writes) return false;
    D3D11_RESOURCE_MISC_FLAG misc = (D3D11_RESOURCE_MISC_FLAG)0;
    if (!src->AddFlags(c->dst.BindFlags(nullptr, &misc), misc, true)) return false; // their own `ref` check
    c->options = (ResourceCopyOptions)((opts & ~(int)ResourceCopyOptions::COPY) | (int)ResourceCopyOptions::REFERENCE);
    logger::write("PORT-REF: '%ls' copies a file-backed texture into a read-only resource -- made a reference",
        c->ini_line.c_str());
    return true;
}

size_t WrapList(CommandList* list)
{
    size_t n = 0;
    if (!list) return 0;
    for (auto& cmd : list->commands) {
        if (!cmd || dynamic_cast<ObservedCommand*>(cmd.get())) continue;
        RefFileBackedCopy(dynamic_cast<ResourceCopyOperation*>(cmd.get()));
        cmd = std::make_shared<ObservedCommand>(cmd);
        ++n;
    }
    return n;
}
} // namespace

CommandListCommand* UnwrapObserved(CommandListCommand* cmd)
{
    if (ObservedCommand* o = dynamic_cast<ObservedCommand*>(cmd)) return o->inner.get();
    return cmd;
}

void WrapObservedList(CommandList* list)
{
    g_wrapped_quiet.fetch_add(WrapList(list), std::memory_order_relaxed);
}

void WrapObservedCommands(std::vector<CommandList*>& lists)
{
    size_t n = 0;
    for (CommandList* list : lists) n += WrapList(list);
    logger::write("PORT-OBSERVE: %zu command(s) in %zu list(s) wrapped for the observer, plus %zu in override "
                  "lists",
        n, lists.size(), g_wrapped_quiet.exchange(0, std::memory_order_relaxed));
}

void SetKeyGate(bool (*fn)(const wchar_t* ns))
{
    std::lock_guard<std::mutex> l(Mtx());
    g_gate = fn;
}

size_t OverrideNamespaceCount()
{
    return g_gated.load(std::memory_order_relaxed);
}

} // namespace MigotoPort

namespace PortApi {

void ClearBoundLoading()
{
    MigotoPort::t_bound_loading = false;
}

bool BoundLoading()
{
    return MigotoPort::t_bound_loading;
}

void CommandCostMark()
{
    std::lock_guard<std::mutex> l(MigotoPort::CostMtx());
    auto&                       costs = MigotoPort::Costs();
    auto&                       base = MigotoPort::CostBase();
    base.assign(costs.size() * 3, 0);
    for (size_t i = 0; i < costs.size(); ++i) {
        base[i * 3 + 0] = costs[i].calls.load(std::memory_order_relaxed);
        base[i * 3 + 1] = costs[i].incl.load(std::memory_order_relaxed);
        base[i * 3 + 2] = costs[i].self.load(std::memory_order_relaxed);
    }
}

void CommandCostReport(FILE* f, int frames)
{
    struct Row {
        std::string name;
        uint64_t    calls;
        uint64_t    incl;
        uint64_t    self;
    };
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> l(MigotoPort::CostMtx());
        auto&                       costs = MigotoPort::Costs();
        auto&                       base = MigotoPort::CostBase();
        for (size_t i = 0; i < costs.size(); ++i) {
            Row r{ MigotoPort::CostNames()[i], costs[i].calls.load(std::memory_order_relaxed),
                costs[i].incl.load(std::memory_order_relaxed), costs[i].self.load(std::memory_order_relaxed) };
            if (i * 3 + 2 < base.size()) {
                r.calls -= base[i * 3 + 0];
                r.incl -= base[i * 3 + 1];
                r.self -= base[i * 3 + 2];
            }
            if (r.calls) rows.push_back(std::move(r));
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.self > b.self; });
    const double inv = frames > 0 ? 1.0 / frames : 0.0;
    const double ms = 1000.0 / (double)prof_qpc_freq();
    uint64_t     total = 0;
    for (const Row& r : rows) total += r.self;
    std::fprintf(f, "3DMIGOTO COMMANDS / frame -- time inside each command by kind, all threads (self leaves out the "
                    "commands it runs; %.3f ms/frame in all):\n",
        total * ms * inv);
    std::fprintf(f, "  %-44s %9s %10s %12s %10s\n", "kind", "calls/f", "self ms/f", "self us/call", "incl ms/f");
    for (const Row& r : rows)
        std::fprintf(f, "  %-44s %9.1f %10.3f %12.2f %10.3f\n", r.name.c_str(), r.calls * inv, r.self * ms * inv,
            r.self * ms * 1000.0 / (double)r.calls, r.incl * ms * inv);
}

} // namespace PortApi
