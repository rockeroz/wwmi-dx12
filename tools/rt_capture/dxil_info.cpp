// dxil_info.cpp -- offline: what a dumped compute shader (bins\cs_<hash>.dxil) is, without DXC.
// Reuses the add-on's own minimal DXIL bitcode reader (src/dxil_cb.cpp, compiled into this file) and prints the
// entry points, every resource (SRVs, UAVs, CBVs, samplers: name, space, register, range) and the struct
// layouts the DXIL metadata annotates (cbuffer field names and offsets).
//
//   g++ -std=c++20 -O1 tools/rt_capture/dxil_info.cpp -o dxil_info && ./dxil_info cs_*.dxil
#include "../../src/dxil_cb.cpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>

namespace {

// The container's parts, and the bitcode of the reflection part (STAT) when the program part's names were
// stripped into it (DXC -Qstrip_reflect, as engines ship them), else the program part (DXIL).
const uint8_t* PartBitcode(const std::vector<uint8_t>& b, const char* fourcc, size_t& bc_len, std::string& parts)
{
    const uint8_t* p = b.data();
    const size_t   n = b.size();
    if (n < 32 || memcmp(p, "DXBC", 4) != 0) return nullptr;
    const uint32_t count = rd32(p + 28);
    for (uint32_t i = 0; i < count && 32 + 4 * i + 4 <= n; ++i) {
        const uint64_t po = rd32(p + 32 + 4 * i);
        if (po + 8 > n) break;
        const uint64_t psz = rd32(p + po + 4);
        if (po + 8 + psz > n) break;
        if (parts.size() < 200) parts += std::string(reinterpret_cast<const char*>(p + po), 4) + "(" + std::to_string(psz) + ") ";
        if (memcmp(p + po, fourcc, 4) != 0) continue;
        const uint8_t* part = p + po + 8;
        if (psz < 24 || memcmp(part + 8, "DXIL", 4) != 0) continue;
        const uint32_t off = rd32(part + 16), sz = rd32(part + 20);
        const uint8_t* bc = part + 8 + off;
        if ((size_t)(bc - part) + sz > psz || sz < 4 || bc[0] != 'B' || bc[1] != 'C') continue;
        bc_len = sz;
        return bc;
    }
    return nullptr;
}

bool LoadModule(const std::vector<uint8_t>& bytes, Module& M, std::string& why)
{
    size_t         bc_len = 0;
    std::string    parts;
    const uint8_t* bc = PartBitcode(bytes, "STAT", bc_len, parts);
    const char*    from = "STAT (reflection)";
    if (!bc) {
        std::string ignore;
        bc = PartBitcode(bytes, "DXIL", bc_len, ignore);
        from = "DXIL (program)";
    }
    if (!bc) {
        bc = find_bitcode(bytes.data(), bytes.size(), bc_len, &why);
        from = "raw";
    }
    std::printf("  parts: %s; metadata from %s\n", parts.c_str(), from);
    if (!bc) return false;
    Reader R(bc + 4, bc_len - 4);
    for (;;) {
        if (R.bits().eof()) break;
        const uint64_t id = R.bits().read(2);
        if (R.bits().bad() || id != 1) break;
        Block    blk;
        uint64_t w;
        if (!R.enter(blk, w)) {
            why = "bad block header";
            return false;
        }
        if (blk.id == MODULE_ID) return read_module(R, blk, M) || ((why = "module block did not parse"), false);
        if (blk.id == BLOCKINFO_ID) {
            if (!R.read_blockinfo(blk)) {
                why = "blockinfo did not parse";
                return false;
            }
            continue;
        }
        R.skip(w);
        if (R.bits().bad()) break;
    }
    why = "no module block";
    return false;
}

std::string Str(const Module& M, int64_t id)
{
    const MdNode* n = node(M, id);
    return n && n->kind == MdNode::String ? n->str : std::string();
}
bool Int(const Module& M, int64_t id, int64_t& out)
{
    const MdNode* n = node(M, id);
    if (!n || n->kind != MdNode::Value || n->value >= M.values.size() || !M.values[n->value].is_int) return false;
    out = M.values[n->value].ival;
    return true;
}
std::string IntS(const Module& M, int64_t id)
{
    int64_t v;
    return Int(M, id, v) ? std::to_string(v) : std::string("?");
}

// A metadata subtree, flattened to one line (strings quoted, ints, nested lists in braces), bounded.
void Dump(const Module& M, int64_t id, std::string& out, int depth, std::set<int64_t>& seen)
{
    if (out.size() > 4000) return;
    const MdNode* n = node(M, id);
    if (!n) {
        out += "null";
        return;
    }
    if (n->kind == MdNode::String) {
        out += '"' + n->str + '"';
        return;
    }
    if (n->kind == MdNode::Value) {
        int64_t v;
        if (Int(M, id, v))
            out += std::to_string(v);
        else {
            const std::string t = md_type_name(M, id);
            out += t.empty() ? "<value>" : "<" + t + ">";
        }
        return;
    }
    if (n->kind != MdNode::Node || depth > 6 || !seen.insert(id).second) {
        out += "...";
        return;
    }
    out += '{';
    for (size_t i = 0; i < n->elems.size(); ++i) {
        if (i) out += ", ";
        Dump(M, n->elems[i], out, depth + 1, seen);
    }
    out += '}';
    seen.erase(id);
}

void Resources(const Module& M)
{
    auto nm = M.named.find("dx.resources");
    if (nm == M.named.end() || nm->second.empty()) {
        std::printf("  resources: none\n");
        return;
    }
    const MdNode* res = node(M, (int64_t)nm->second[0]);
    static const char* kClass[] = { "SRV", "UAV", "CBV", "Sampler" };
    for (size_t k = 0; res && res->kind == MdNode::Node && k < res->elems.size() && k < 4; ++k) {
        const MdNode* list = node(M, res->elems[k]);
        if (!list || list->kind != MdNode::Node) continue;
        for (int64_t e : list->elems) {
            const MdNode* r = node(M, e);
            if (!r || r->kind != MdNode::Node || r->elems.size() < 6) continue;
            // {id, %type* undef, !"name", space, lower bound, range, <class-specific...>}
            std::string extra;
            for (size_t i = 6; i < r->elems.size(); ++i) {
                std::set<int64_t> seen;
                extra += i == 6 ? "" : " ";
                Dump(M, r->elems[i], extra, 0, seen);
            }
            std::printf("  %-7s %-40s type=%-40s space=%s reg=%s range=%s extra=[%s]\n", kClass[k], Str(M, r->elems[2]).c_str(),
                md_type_name(M, r->elems[1]).c_str(), IntS(M, r->elems[3]).c_str(), IntS(M, r->elems[4]).c_str(),
                IntS(M, r->elems[5]).c_str(), extra.c_str());
        }
    }
}

// dx.typeAnnotations: {i32 0, struct annotations: [%type undef, size?, field annots...]...}
void TypeAnnotations(const Module& M)
{
    auto nm = M.named.find("dx.typeAnnotations");
    if (nm == M.named.end()) return;
    for (uint64_t top : nm->second) {
        const MdNode* t = node(M, (int64_t)top);
        if (!t || t->kind != MdNode::Node || t->elems.empty()) continue;
        int64_t tag = -1;
        Int(M, t->elems[0], tag);
        if (tag != 0) continue; // 0 = struct annotations, 1 = function annotations
        for (size_t i = 1; i + 1 < t->elems.size(); i += 2) {
            const std::string name = md_type_name(M, t->elems[i]);
            const MdNode*     fields = node(M, t->elems[i + 1]);
            if (!fields || fields->kind != MdNode::Node) continue;
            std::printf("  struct %s (size %s):\n", name.c_str(), fields->elems.empty() ? "?" : IntS(M, fields->elems[0]).c_str());
            for (size_t f = 1; f < fields->elems.size(); ++f) {
                const MdNode* fa = node(M, fields->elems[f]);
                if (!fa || fa->kind != MdNode::Node) continue;
                // field annotation: pairs {i32 tag, value}: 6 = name, 3 = offset, 7 = component type, 5 = matrix...
                std::string fname, off = "?", ctype = "?", more;
                for (size_t a = 0; a + 1 < fa->elems.size(); a += 2) {
                    int64_t key = -1;
                    Int(M, fa->elems[a], key);
                    if (key == 6) fname = Str(M, fa->elems[a + 1]);
                    else if (key == 3) off = IntS(M, fa->elems[a + 1]);
                    else if (key == 7) ctype = IntS(M, fa->elems[a + 1]);
                    else {
                        std::set<int64_t> seen;
                        more += " k" + std::to_string(key) + "=";
                        Dump(M, fa->elems[a + 1], more, 0, seen);
                    }
                }
                std::printf("    +%-6s %-36s comp=%s%s\n", off.c_str(), fname.c_str(), ctype.c_str(), more.c_str());
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        std::ifstream f(argv[a], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::printf("== %s (%zu bytes)\n", argv[a], bytes.size());
        Module      M;
        std::string why;
        if (!LoadModule(bytes, M, why)) {
            std::printf("  not parsed: %s\n", why.c_str());
            rc = 1;
            continue;
        }
        auto ep = M.named.find("dx.entryPoints");
        if (ep != M.named.end())
            for (uint64_t id : ep->second) {
                const MdNode* e = node(M, (int64_t)id);
                if (!e || e->kind != MdNode::Node || e->elems.size() < 2) continue;
                std::string props;
                if (e->elems.size() >= 5) {
                    std::set<int64_t> seen;
                    Dump(M, e->elems[4], props, 0, seen);
                }
                std::printf("  entry: %s props=%s\n", Str(M, e->elems[1]).c_str(), props.c_str());
            }
        auto sm = M.named.find("dx.shaderModel");
        if (sm != M.named.end() && !sm->second.empty()) {
            std::string s;
            std::set<int64_t> seen;
            Dump(M, (int64_t)sm->second[0], s, 0, seen);
            std::printf("  shader model: %s\n", s.c_str());
        }
        Resources(M);
        TypeAnnotations(M);
    }
    return rc;
}
