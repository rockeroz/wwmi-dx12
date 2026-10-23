#pragma once
#include <cstdint>
#include <string>
#include <vector>

void TexLodInit(const std::wstring& cache_dir = std::wstring());

void TexLodTargetsReady(bool (*is_target)(uint32_t hash, void* user), void* user);
void TexLodOnExit();
void TexLodFlushIfQuiet(unsigned quiet_ms);
void TexLodSetResemblance(bool on);
void TexLodSetCache(bool on);

uint32_t TexLodLookup(uint32_t hash);
uint32_t TexLodKnown(uint32_t hash);
unsigned TexLodMipOf(uint32_t hash); // a proven low: which mip of its target it is; 0 = not a proven low, or not known
bool     TexLodLinked(uint32_t hash); // any link, exact or a look-alike
bool     TexLodDegenerate(uint32_t hash);
bool     TexLodLearnVisual(uint32_t low, uint32_t high, float ssim); // a tex_look match: a look-alike link
bool     TexLodLearnPlace(uint32_t low, uint32_t high);              // drawn where HD draws `high`: a look-alike link
bool     TexLodExport(const std::wstring& path, bool (*keep)(uint32_t high, void* user), void* user); // proofs, as a seed

void     TexLodFeeds(uint32_t lower, uint32_t big, uint32_t big_rule, uint32_t steps); // (`small` is a windows.h macro)
uint32_t TexLodAddMipExact(uint32_t low, uint32_t high, unsigned mip, bool* now_degenerate);
void     TexLodRefute(uint32_t low, uint32_t high, const char* why);
