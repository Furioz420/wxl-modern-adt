// Native split-ADT reader: internal contract shared across the reader's translation units.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

// The reader is split by responsibility (state / detect / parse / async / wdl / texture) but is ONE
// subsystem: every stage mutates the same per-tile SplitTile record, keyed by the tile-area object in
// one map.
// That model, the raw-byte helpers, the by-address native shims, and the telemetry counters live here
// (state defined once in AdtState.cpp) so each stage TU shares one view of a tile's lifetime.

#include "../ExtensionApi.hpp"

#include "game/Binding.hpp"
#include "offsets/engine/Io.hpp"
#include "offsets/game/ADT.hpp"
#include "offsets/game/World.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::runtime::adtsplit::detail
{
    namespace adt = wxl::offsets::game::adt;
    namespace wld = wxl::offsets::game::world;
    namespace io  = wxl::offsets::engine::io;

    // ---------------------------------------------------------------- map-header bits
    // Two bits of the live map header the reader cares about: the dataset says it has per-layer
    // height texturing, and the terrain build sizes layer coverage as one byte per texel.
    constexpr uint32_t kMapHeightTexturing = 0x80u;
    constexpr uint32_t kMapWideAlpha       = 0x4u;

    // ---------------------------------------------------------------- raw byte helpers
    inline uint16_t Rd16(const void* p)       { uint16_t v; std::memcpy(&v, p, 2); return v; }
    inline uint32_t Rd32(const void* p)       { uint32_t v; std::memcpy(&v, p, 4); return v; }
    inline uint64_t Rd64(const void* p)       { uint64_t v; std::memcpy(&v, p, 8); return v; }
    inline void     Wr32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }
    inline void     Wr16(void* p, uint16_t v) { std::memcpy(p, &v, 2); }

    /// FourCC as the client compares it: chunk tags are stored byte-reversed on disk, so the
    /// little-endian dword read equals ('M'<<24)|('C'<<16)|... for an "MC.." chunk.
    constexpr uint32_t FourCC(const char (&s)[5])
    {
        return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
               (uint32_t(uint8_t(s[2])) << 8)  |  uint32_t(uint8_t(s[3]));
    }

    /// True when the 4 bytes at p read back as an "MC.." sub-chunk tag (disk order is reversed).
    inline bool LooksLikeSubTag(const uint8_t* p) { return p[3] == 'M' && p[2] == 'C'; }

    template <class T>
    inline T& At(void* base, size_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

    /// Generic bounds-checked top-level chunk walk; cb(tag, chunkHeaderPtr, dataSize).
    template <class Fn>
    void WalkTop(uint8_t* buf, uint32_t size, Fn&& cb)
    {
        uint32_t off = 0;
        while (off + 8 <= size)
        {
            const uint32_t tag = Rd32(buf + off);
            const uint32_t sz  = Rd32(buf + off + 4);
            if (sz > size - off - 8) break;
            cb(tag, buf + off, sz);
            off += 8 + sz;
        }
    }

    // ---------------------------------------------------------------- native call shims
    // The storage entry points are called BY ADDRESS so the calls land in the installed detours
    // (StorageHook serves host files through them), same trick Phasing uses for its WDT probe.
    inline void* OpenFile(const char* name)
    {
        void* h = nullptr;
        if (!reinterpret_cast<io::Storage_FileOpenFn>(io::kFileOpen)(nullptr, name, 0, &h))
            return nullptr;
        return h;
    }
    inline uint32_t SizeOfFile(void* h) { return reinterpret_cast<io::Storage_FileSizeFn>(io::kFileSize)(h, nullptr); }
    inline void     CloseFile(void* h)  { reinterpret_cast<io::Storage_FileCloseFn>(io::kFileClose)(h); }
    inline bool     ReadBytes(void* h, void* dst, uint32_t len)
    {
        return reinterpret_cast<io::Storage_FileReadFn>(io::kFileRead)(h, dst, len, nullptr, nullptr, 0) != 0;
    }

    inline uint8_t* AllocRaw(uint32_t size)
    {
        return static_cast<uint8_t*>(wxl::game::Native<adt::AllocRawAreaDataFn>(adt::kAllocRawAreaData)(size));
    }
    inline void FreeRaw(void* p, uint32_t size)
    {
        wxl::game::Native<adt::FreeRawAreaDataFn>(adt::kFreeRawAreaData)(p, size);
    }
    inline void* AsyncAlloc()
    {
        return wxl::game::Native<wld::AsyncFileReadAllocObjectFn>(wld::kAsyncFileReadAllocObject)();
    }
    // Enqueue by address: streaming's round-robin detour on AsyncFileReadObject receives the call.
    inline void AsyncEnqueue(void* obj)
    {
        reinterpret_cast<wld::AsyncFileReadObjectFn>(wld::kAsyncFileReadObject)(obj, 0);
    }
    // AsyncFileReadDestroyObject: spin-waits an in-service read, unlinks a queued completion so it can
    // never run, closes the object's file handle and recycles the node.
    inline void AsyncRetire(void* obj)
    {
        reinterpret_cast<wld::AsyncDestroyFn>(wld::kAsyncDestroy)(obj);
    }

    /// RAII wrapper around a storage-seam handle for the open/probe/close idiom (existence checks,
    /// small bounded reads): closes on scope exit. Not for a handle handed off to an async read -- that
    /// owner outlives this scope and closes it itself (AsyncRetire), so StartStage/hkTileAreaLoad still
    /// use OpenFile/CloseFile directly.
    class ScopedFile
    {
    public:
        explicit ScopedFile(const char* name) : h_(OpenFile(name)) {}
        ~ScopedFile() { if (h_) CloseFile(h_); }
        ScopedFile(const ScopedFile&) = delete;
        ScopedFile& operator=(const ScopedFile&) = delete;

        explicit operator bool() const { return h_ != nullptr; }
        void* get() const { return h_; }

    private:
        void* h_;
    };

    /// Runs fn() inside an SEH shell: a fault (malformed input, an unmapped read) classifies as
    /// `fallback` instead of crashing. No unwindable locals in THIS frame (MSVC C2712) -- fn must be a
    /// plain callable whose closure needs no destructor, which every guarded call site here already is.
    template <class Fn, class Ret>
    Ret SehGuarded(Fn fn, Ret fallback)
    {
        __try
        {
            return fn();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return fallback;
        }
    }

    // ---------------------------------------------------------------- per-tile model
    enum class Stage : int { Root = 0, Tex = 1, Obj = 2, Done = 3 };

    /// Per-chunk direct-fill index: every pointer aims INTO one of the three resident buffers, except
    /// mcrf which may aim into the tile's small MCRF concat pool.
    struct ChunkFill
    {
        uint8_t* rootMcnkHdr = nullptr; // raw MCNK (tag+size) in the root buffer
        // root-file sub-chunks
        uint8_t* mcvt = nullptr;
        uint8_t* mccv = nullptr;
        uint8_t* mcnr = nullptr;
        uint8_t* mclq = nullptr; uint32_t mclqSize = 0;
        uint8_t* mcse = nullptr; uint32_t mcseSize = 0;
        // _tex0 sub-chunks (header-less MCNK, correspondence by order)
        uint8_t* mcly = nullptr; uint32_t mclySize = 0;
        uint8_t* mcal = nullptr; uint32_t mcalSize = 0;
        uint8_t* mcsh = nullptr; uint32_t mcshSize = 0;
        // _obj0 sub-chunks
        uint8_t* mcrd = nullptr; uint32_t mcrdSize = 0;
        uint8_t* mcrw = nullptr; uint32_t mcrwSize = 0;
        // materialized fixup (MCRD data ‖ MCRW data) or a direct alias when one side is empty
        uint8_t* mcrf = nullptr;
        uint32_t nDoodadRefs = 0;
        uint32_t nMapObjRefs = 0;
        // parked (no 335 runtime home; retained for later features)
        uint8_t* mclv = nullptr;
        uint64_t hiResHoles = 0;
        bool     hadHiResHoles = false;
    };

    /// The three resident source buffers. Root is owned by the area itself (aliased at area+0x80/+0x84,
    /// freed by the stock destructor); tex/obj are owned here and released in the destructor detour.
    struct BufferSet
    {
        uint8_t* rootBuf = nullptr; uint32_t rootSize = 0;
        uint8_t* texBuf  = nullptr; uint32_t texSize  = 0; bool texOwned = false;
        uint8_t* objBuf  = nullptr; uint32_t objSize  = 0; bool objOwned = false;
    };

    /// Synthesized top-level chunks the split trio doesn't ship on its own: MCIN (the stock parser
    /// still needs it though split content dropped it from disk), the MCRD/MCRW concat pool the direct-filled
    /// chunks' mcrf aliases into, filler empties for absent tables, and the real MMDX/MMID/MWMO/MWID
    /// name-table chunks synthesized from FileDataID-only doodad/map-object refs.
    struct SynthBlobs
    {
        std::vector<uint8_t> mcin;       // synthesized MCIN chunk (8-byte header + 256 x 16 B)
        std::vector<uint8_t> mcrfPool;   // concatenated MCRD‖MCRW payloads (chunks alias into it)
        std::vector<uint8_t> synthEmpty; // synthesized empty top-level chunks for absent tables
        // Real MMDX/MMID name-table chunks synthesized from the doodads' MDDF FileDataID nameIds
        // (resolved via ModelFilePath). Each is a full {tag,size,data} chunk; MHDR points at them and
        // the (in-place rewritten) MDDF entries index MMID. Owned for the tile lifetime.
        std::vector<uint8_t> mmdxChunk;  // "MMDX" + NUL-terminated model paths
        std::vector<uint8_t> mmidChunk;  // "MMID" + u32 offsets into the MMDX payload
        // Same treatment for placed map objects: MODF nameIds carry a FileDataID under entry flag 0x8
        // and a modern _obj0 ships no MWMO/MWID at all. Unlike a doodad, a map object that fails to
        // resolve is FATAL (the client hashes the name straight away, and its file-open path has no
        // fallback equivalent), so an unresolved entry is instead struck from every MCRW ref list --
        // see modfDead below. Nothing is removed from MODF itself: the entry stays, simply unreferenced.
        std::vector<uint8_t> mwmoChunk;  // "MWMO" + NUL-terminated map-object paths
        std::vector<uint8_t> mwidChunk;  // "MWID" + u32 offsets into the MWMO payload
        std::vector<uint8_t> modfDead;   // one flag per MODF entry: 1 = unresolved, never reference it
        std::vector<uint8_t> mcrwFiltered; // rebuilt MCRW payloads when modfDead removed anything
    };

    /// Texture FileDataIDs read from _tex0 MDID (diffuse), indexed by MCLY.textureId, and the client
    /// paths they resolve to. Modern tiles carry no MTEX; this is how the terrain layers name their
    /// textures. The tex manager detour resolves each through TextureFilePath.db2 into texNameBlob
    /// (NUL-terminated client paths, in MDID order) and feeds THAT to the stock tile texture loader
    /// -- the slots then point into this persistent blob for the tile lifetime.
    struct TextureCache
    {
        std::vector<uint32_t> mdid;
        std::vector<char>     texNameBlob;
    };

    /// Everything the height-blend feature needs to build its per-texture slots. mhid mirrors mdid
    /// (index-aligned height-texture FileDataIDs); mtxp/mtxfData/mtexData park the _tex0 MTXP/MTXF/MTEX
    /// blobs (all pointing into the resident _tex0 buffer) for the MTXP-default flag fallback and the
    /// name-based "_h.blp" derivation on tiles without FileDataIDs.
    struct HeightState
    {
        std::vector<uint32_t> mhid;
        uint8_t* mtxfData = nullptr; uint32_t mtxfSize = 0;
        uint8_t* mtexData = nullptr; uint32_t mtexSizeParked = 0;
        uint8_t* mtxp     = nullptr; uint32_t mtxpSize = 0;

        /// Per-texture height slot, MTXP/MDID order. tex==null means "solid white" (default params,
        /// flag 0x1, or an unresolvable file). owned marks handles this record must release. exp is the
        /// UV-tiling exponent from the record's flags bits 4..7 (applies to the diffuse layer whether or
        /// not a height texture exists).
        struct HeightSlot
        {
            void* tex = nullptr; bool owned = false;
            float scale = 0.0f; float offset = 1.0f; uint8_t exp = 0;
        };
        std::vector<HeightSlot> heightSlots;
        bool heightBuilt = false;
    };

    /// Per-tile side record. Owns the _tex0/_obj0 buffers (root is owned by the area at +0x80) and every
    /// small fixup allocation the direct-filled chunks point at. Lifetime: created in the tile-area load
    /// detour, released in the tile-area destructor detour AFTER the original body -- i.e. after every
    /// chunk of the tile is already purged, so nothing can dangle.
    struct SplitTile
    {
        void*    area = nullptr;
        int      tileFirst = 0, tileSecond = 0;
        char     texName[300] = { 0 };
        char     objName[300] = { 0 };
        uint64_t loadStartMs = 0; // GetTickCount64() at arm time, for the load-duration log

        // root: sequential, solo -- its async is the one the tile-area load itself would carry, so it
        // alone still mirrors into area+0x70/+0x6C (the stock purge/cancel path stays untouched for
        // it). _tex0/_obj0 are independent files and load CONCURRENTLY once root completes; only
        // ONE pointer fits in area+0x70 at a time, so `stage` here tracks whichever of the two is
        // CURRENTLY mirrored there (updated by UpdateVisibleAsync as each finishes), not a fixed
        // sequential phase. The other -- the one not currently mirrored -- is invisible to the stock
        // purge/cancel path entirely and is retired solely by our own destructor detour.
        //
        // `complete` is what actually gates readiness now that the parse backing it can finish many
        // frames after area+0x70 goes back to null (see FinalizeTile in AdtSplit.cpp): the tile-area
        // update (hkAreaUpdate) checks it before letting the native body touch area+0x88 (via PrepareChunk) or
        // read a chunk pointer back out of its own grid, since area+0x70 itself cannot safely be held
        // non-null past a read's real completion -- see UpdateVisibleAsync's doc comment for the
        // AsyncFileReadWait() footgun that ruled that approach out.
        Stage    stage    = Stage::Root;
        bool     complete = false;
        void*    curAsync = nullptr; // root only
        void*    texAsync = nullptr;
        void*    objAsync = nullptr;
        bool     texDone  = false; // true once armed-and-completed, or never armed at all
        bool     objDone  = false;

        BufferSet    buffers;
        ChunkFill    chunks[256];
        uint32_t     chunkCount = 0;
        SynthBlobs   synth;
        TextureCache texCache;
        HeightState  height;

        // parked tile-level foreigners (no natural group of their own)
        uint8_t  mampValue = 0; // MHDR+0x30 / MAMP byte (zeroed for the client)
        uint32_t mclvChunks = 0, hiResHoleChunks = 0;

        // Cross-thread handoff for the background parse (FinalizeTile submits ParseAndPatchGuarded to
        // wxl.loadpool's stage1; see AdtSplit.cpp). `parsing` is set true right before submission and
        // cleared by the worker thread itself the instant ParseAndPatchGuarded returns -- NOT by
        // stage2, which runs on the main thread and would deadlock hkTileAreaDestroy's wait below if it
        // were the one clearing it. hkTileAreaDestroy waits on parseDone before touching or freeing
        // anything the worker could still be writing into; this is the ONLY thing protecting `chunks`/
        // `synth`/`texCache`/`height` from a purge racing an in-flight parse. g_mutex (below) is a
        // different lock protecting a different thing -- the g_tiles/g_splitMaps MAP structure, not one
        // tile's own contents.
        std::mutex              parseMutex;
        std::condition_variable parseDone;
        bool                    parsing = false;
    };

    // ---------------------------------------------------------------- shared state
    extern std::mutex g_mutex; // guards the two maps below (loads are main-thread; snapshots read them)
    extern std::unordered_map<void*, std::unique_ptr<SplitTile>> g_tiles;   // key: tile-area object
    extern std::unordered_map<std::string, bool> g_splitMaps;               // key: "<dir>\<name>" prefix

    extern std::atomic<uint32_t> g_statSplitMaps, g_statTilesLoaded, g_statTilesResident,
        g_statChunksFilled, g_statMcrfBytes, g_statMtxpTiles, g_statMclvChunks, g_statHoleChunks,
        g_statFailures, g_statWdlRead, g_statHeightTex, g_statDoodadModels, g_statMapObjects,
        g_statMapObjectsDropped, g_statLiquidLayers, g_statLiquidDegraded, g_statLiquidOob;

    SplitTile* FindTileLocked(void* area);   // caller holds g_mutex
    SplitTile* FindTileBrief(void* area);     // takes g_mutex only for the map access, then releases

    // ---------------------------------------------------------------- cross-unit responsibilities
    bool IsSplitTileName(const char* name, std::string& keyOut);  // AdtDetect
    void EnsureAlphaLayoutFlag();                                  // AdtDetect
    bool ParseAndPatchGuarded(SplitTile* t);                       // AdtParse
    bool InstallWdl();                                             // AdtWdl
    bool InstallTextures();                                        // AdtTexture
    bool InstallAdtSplit();                                        // AdtSplit, called from Module.cpp
}
