// Native split-ADT reader: the feature entry -- the async 3-read chain, the tile detours, the query surface.
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

// MECHANISM (direct-fill, no monolithic merge). This unit owns the tile lifetime: tile-area load arms an
// async read of root into a resident buffer, root alone mirrored at area+0x70/+0x6C so the stock
// purge/cancel path is untouched; once root completes, _tex0 and _obj0 -- independent files with no
// ordering constraint between them -- are armed CONCURRENTLY, with area+0x70/+0x6C kept mirroring
// whichever of the two is still outstanding (UpdateVisibleAsync; the stock per-frame streaming tick reads
// that field to decide purge-eligibility and "ready to build", so it must never read as done while either
// read genuinely still is). Once both complete, the tile parses (AdtParse) and hands the area to the
// UNCHANGED stock tile-area create routine. Per chunk, the sub-chunk walk direct-assigns the sub-chunk
// slots into the resident buffers. The destructor detour releases the tile's side record AFTER the
// original body, when every chunk is already purged, retiring whichever of the concurrent pair the stock
// purge path never saw (only one of the two ever occupies area+0x70 at a time).
// Detection lives in AdtDetect, the trio parse/fixups in AdtParse, the split WDL in AdtWdl, and the
// split-tile texture manager + height slots in AdtTexture.

#include "engine/events/Event.hpp"
#include "game/Loading.hpp"
#include "AdtSplit.hpp"
#include "AdtSplitInternal.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace
{
    using namespace wxl::runtime::adtsplit::detail;
    namespace ev = wxl::events;

    adt::TileAreaLoadFn           g_origTileAreaLoad    = nullptr;
    adt::ChunkProcessIffChunksFn  g_origProcessIff      = nullptr;
    adt::TileAreaDestroyFn        g_origTileAreaDestroy = nullptr;
    adt::Map_AreaUpdateFn         g_origAreaUpdate      = nullptr;

    // ---------------------------------------------------------------- the async chain
    void __cdecl SplitTexComplete(void* areaCtx); // fwd
    void __cdecl SplitObjComplete(void* areaCtx); // fwd

    /**
     * @brief Keeps area+0x70/+0x6C mirroring whichever of the concurrent _tex0/_obj0 reads is still
     *        genuinely outstanding (root uses its own, simpler, always-mirrored path -- see
     *        hkTileAreaLoad/SplitRootComplete). Once both are done the field is cleared to null.
     *
     * DO NOT "fix" the tile-area prepare-chunk null-deref (area+0x88 not populated yet when the
     * background parse hasn't finished) by parking some already-completed async object here instead
     * of null -- that was tried and reverted. area+0x70 is not a passive marker to the native engine:
     * the map's pre-update-areas pass also calls the native async-wait helper on it for close-range
     * tiles, and that helper keeps a single global reentrancy guard that it only decrements on its
     * normal in-flight path -- handed an object whose completion callback already ran, it takes the
     * "already handled" early-return instead, which never decrements the guard, so the NEXT tile's
     * async-wait call anywhere hits that guard's fatal assert. Clearing to null the
     * instant both reads finish is what the native engine expects; the tile-not-ready-yet window this
     * opens (TileAreaCreate now runs later, once the background parse drains) is instead closed by
     * hkAreaUpdate gating on SplitTile::complete before touching area+0x88 at all.
     */
    void UpdateVisibleAsync(SplitTile& t)
    {
        void* visible;
        if (t.texAsync)      { visible = t.texAsync; t.stage = Stage::Tex; }
        else if (t.objAsync) { visible = t.objAsync; t.stage = Stage::Obj; }
        else                 { visible = nullptr;     t.stage = Stage::Done; }

        wxl::game::world::SetTileAsyncRead(t.area, visible);
        wxl::game::world::SetTileFileHandle(t.area, visible ? At<void*>(visible, wld::kOffAsyncFile) : nullptr);
    }

    /**
     * @brief Arms one async whole-file read for _tex0/_obj0: open -> size -> AllocRawAreaData ->
     *        async-read record{file,buf,size,ctx=area,cb} -> enqueue.
     *
     * Unlike root, does NOT itself touch area+0x70/+0x6C -- the two stages run concurrently once root
     * completes, and UpdateVisibleAsync (called by the caller once both arm attempts are known) is what
     * decides which single one of them, if any, gets mirrored there. Returns false (nothing armed, tile
     * fields untouched) when the stage's file is absent or empty.
     */
    bool StartStage(SplitTile& t, Stage stage)
    {
        const char* name = (stage == Stage::Tex) ? t.texName : t.objName;
        void* file = OpenFile(name);
        if (!file) return false;
        const uint32_t size = SizeOfFile(file);
        if (size == 0 || size == 0xFFFFFFFFu) { CloseFile(file); return false; }
        uint8_t* buf = AllocRaw(size);
        if (!buf) { CloseFile(file); return false; }
        void* async = AsyncAlloc();
        if (!async) { FreeRaw(buf, size); CloseFile(file); return false; }

        if (stage == Stage::Tex) { t.buffers.texBuf = buf; t.buffers.texSize = size; t.buffers.texOwned = true; }
        else                     { t.buffers.objBuf = buf; t.buffers.objSize = size; t.buffers.objOwned = true; }

        At<void*>(async, wld::kOffAsyncFile)     = file;
        At<void*>(async, wld::kOffAsyncBuffer)   = buf;
        At<uint32_t>(async, wld::kOffAsyncSize)  = size;
        At<void*>(async, wld::kOffAsyncCtx)      = t.area;
        At<void*>(async, wld::kOffAsyncCallback) = reinterpret_cast<void*>(
            stage == Stage::Tex ? &SplitTexComplete : &SplitObjComplete);

        if (stage == Stage::Tex) t.texAsync = async; else t.objAsync = async;
        AsyncEnqueue(async);
        return true;
    }

    /**
     * @brief Everything FinalizeTile does once the parse result is known: publish it (t.complete),
     *        hand the area to the UNCHANGED stock tile-area create routine, retire root's async, log/emit.
     *
     * Runs on the main thread ONLY -- TileAreaCreate walks/builds the chunk grid the render loop reads,
     * so it must never run anywhere else. Called either straight from FinalizeTile (pool absent) or
     * from Stage2Finalize once the background parse (if any) has completed.
     */
    void FinalizeTileTail(SplitTile& t, bool parsed)
    {
        void* area = t.area;
        if (!parsed)
        {
            g_statFailures.fetch_add(1, std::memory_order_relaxed);
            WLOG_ERROR("adt-split: tile %d_%d parse FAILED; running the stock parser over the raw root "
                       "(native corrupt-data behaviour)", t.tileFirst, t.tileSecond);
        }
        t.complete = parsed; // gate the ProcessIffChunks direct-fill on a good index only
        wxl::game::Native<adt::TileAreaCreateFn>(adt::kTileAreaCreate)(area, nullptr);

        if (t.curAsync) { AsyncRetire(t.curAsync); t.curAsync = nullptr; } // root, if still tracked
        wxl::game::world::SetTileAsyncRead(area, nullptr);
        wxl::game::world::SetTileFileHandle(area, nullptr);
        t.stage = Stage::Done;

        if (parsed)
        {
            g_statTilesLoaded.fetch_add(1, std::memory_order_relaxed);
            if (t.height.mtxp) g_statMtxpTiles.fetch_add(1, std::memory_order_relaxed);
            g_statMclvChunks.fetch_add(t.mclvChunks, std::memory_order_relaxed);
            g_statHoleChunks.fetch_add(t.hiResHoleChunks, std::memory_order_relaxed);
            const uint32_t ms = static_cast<uint32_t>(GetTickCount64() - t.loadStartMs);
            WLOG_INFO("adt-split: tile %d_%d loaded (root %u B, tex %u B, obj %u B, %u chunks, mcrf %zu B, %u ms)",
                      t.tileFirst, t.tileSecond, t.buffers.rootSize, t.buffers.texSize, t.buffers.objSize, t.chunkCount,
                      t.synth.mcrfPool.size(), ms);
            ev::AdtSplitTileLoadArgs a{ t.tileFirst, t.tileSecond, t.buffers.rootSize, t.buffers.texSize, t.buffers.objSize,
                                        t.chunkCount };
            wxl_modern_adt::g_api->Emit(uint32_t(ev::Event::OnAdtSplitTileLoad), &a);
        }
    }

    /**
     * @brief Background job context for one tile's parse. `tile` is only safe to use from Stage1Parse:
     *        hkTileAreaDestroy's wait on SplitTile::parseDone guarantees the tile cannot be freed while
     *        stage1 still holds it, but gives NO such guarantee for stage2 -- a purge can land between
     *        stage1 finishing and stage2's drain turn, so Stage2Finalize re-resolves the tile from
     *        `area` instead of trusting this pointer (see SplitTile::parsing in AdtSplitInternal.hpp).
     */
    struct FinalizeCtx
    {
        SplitTile* tile;
        void*      area;
        bool       parsed = false;
    };

    /// Runs on a wxl.loadpool worker thread (or inline, see FinalizeTile). The only background access
    /// to `t`'s contents; clears `parsing` itself, on this thread, the instant it is done -- see the
    /// field's own doc comment for why stage2 must not be the one clearing it.
    void __cdecl Stage1Parse(void* argRaw)
    {
        auto* ctx = static_cast<FinalizeCtx*>(argRaw);
        SplitTile& t = *ctx->tile;
        ctx->parsed = ParseAndPatchGuarded(&t);
        {
            std::lock_guard<std::mutex> lock(t.parseMutex);
            t.parsing = false;
        }
        t.parseDone.notify_all();
    }

    /// Runs on the main thread (the wxl.loadpool per-frame drain, or inline -- see FinalizeTile). Never
    /// dereferences ctx->tile: a purge may have already freed it by the time this runs, so the tile is
    /// re-resolved from ctx->area under g_mutex, exactly like every other lookup in this file. A miss
    /// means hkTileAreaDestroy already ran and did its own cleanup; there is nothing left to finalize.
    void __cdecl Stage2Finalize(void* argRaw)
    {
        std::unique_ptr<FinalizeCtx> ctx(static_cast<FinalizeCtx*>(argRaw));
        if (SplitTile* t = FindTileBrief(ctx->area))
            FinalizeTileTail(*t, ctx->parsed);
    }

    /**
     * @brief Queues the tile's parse on wxl.loadpool, publishing the result on the main thread once
     *        both the parse and its finalize tail complete. Falls back to running both inline,
     *        synchronously, on the calling thread when the extension is not loaded -- WXL_LoadPoolApi's
     *        own Submit already does this when the pool itself is merely disabled, so this is only for
     *        "the interface was never published at all".
     */
    void FinalizeTile(SplitTile& t)
    {
        {
            std::lock_guard<std::mutex> lock(t.parseMutex);
            t.parsing = true;
        }
        auto* ctx = new FinalizeCtx{ &t, t.area, false };
        if (const WXL_LoadPoolApi* pool = wxl_modern_adt::LoadPool())
            pool->Submit(&Stage1Parse, &Stage2Finalize, ctx);
        else
        {
            Stage1Parse(ctx);
            Stage2Finalize(ctx);
        }
    }

    /**
     * @brief Main-thread completion for root's read (installed as async+0x10, invoked by the stock
     *        drain as callback(ctx=area)).
     *
     * Retires root's own async (mirroring the native single-read epilogue exactly, since root alone
     * still occupies area+0x70/+0x6C), then arms _tex0 and _obj0 CONCURRENTLY -- they're independent
     * files with no ordering constraint between them, unlike root which must resolve first (its
     * filename is the tile's own; tex/obj's names are only known once split-ness is confirmed). A tile
     * with neither sibling still finalizes the same way, through FinalizeTile (see its own doc comment
     * for why the actual TileAreaCreate call is no longer guaranteed to happen in this same frame).
     */
    void __cdecl SplitRootComplete(void* areaCtx)
    {
        SplitTile* t = FindTileBrief(areaCtx);
        if (!t) return;

        if (t->curAsync) { AsyncRetire(t->curAsync); t->curAsync = nullptr; } // closes root file
        wxl::game::world::SetTileAsyncRead(t->area, nullptr);
        wxl::game::world::SetTileFileHandle(t->area, nullptr);

        const bool texArmed = StartStage(*t, Stage::Tex);
        const bool objArmed = StartStage(*t, Stage::Obj);
        t->texDone = !texArmed;
        t->objDone = !objArmed;
        UpdateVisibleAsync(*t);
        if (t->texDone && t->objDone) FinalizeTile(*t);
    }

    /// Completion for the concurrent _tex0 read. See UpdateVisibleAsync for why area+0x70/+0x6C are
    /// touched here (not just retire-and-clear): the sibling _obj0 read may still be outstanding.
    void __cdecl SplitTexComplete(void* areaCtx)
    {
        SplitTile* t = FindTileBrief(areaCtx);
        if (!t) return;
        void* finished = t->texAsync;
        t->texAsync = nullptr;
        t->texDone  = true;
        UpdateVisibleAsync(*t); // repoints area+0x70 at objAsync if it's still pending, else clears it
        if (finished) AsyncRetire(finished);
        if (t->texDone && t->objDone) FinalizeTile(*t);
    }

    /// Completion for the concurrent _obj0 read; symmetric to SplitTexComplete.
    void __cdecl SplitObjComplete(void* areaCtx)
    {
        SplitTile* t = FindTileBrief(areaCtx);
        if (!t) return;
        void* finished = t->objAsync;
        t->objAsync = nullptr;
        t->objDone  = true;
        UpdateVisibleAsync(*t);
        if (finished) AsyncRetire(finished);
        if (t->texDone && t->objDone) FinalizeTile(*t);
    }

    // ---------------------------------------------------------------- tile detours
    /**
     * @brief Detours tile-area load (the per-tile open + async-read arm).
     *
     * Split tile: arms the 3-read chain (root first, stock-shaped) and returns without the original.
     * Non-split tile, unknown name, or any arming failure: the untouched native body runs -- the stock
     * path stays byte-identical for classic maps.
     */
    void __fastcall hkTileAreaLoad(void* area, void* edx, const char* filename)
    {
        if (filename && area)
        {
            std::string key;
            if (IsSplitTileName(filename, key))
            {
                // The alpha-layout flag lives in map-header state that is rebuilt whenever a world
                // loads, so every tile re-arms it (idempotent) instead of it being set once when the
                // map is first recognised. This runs before the tile's coverage is ever sized.
                EnsureAlphaLayoutFlag();

                // stage 1 (root), shaped exactly like the native body
                void* file = OpenFile(filename);
                if (file)
                {
                    const uint32_t size = SizeOfFile(file);
                    uint8_t* buf   = (size && size != 0xFFFFFFFFu) ? AllocRaw(size) : nullptr;
                    void*    async = buf ? AsyncAlloc() : nullptr;
                    if (async)
                    {
                        auto tile = std::make_unique<SplitTile>();
                        tile->area        = area;
                        tile->tileFirst   = static_cast<const adt::TileArea*>(area)->tileFirst;
                        tile->tileSecond  = static_cast<const adt::TileArea*>(area)->tileSecond;
                        tile->loadStartMs = GetTickCount64();
                        const size_t base = std::strlen(filename) - 4;
                        std::snprintf(tile->texName, sizeof tile->texName, "%.*s_tex0.adt",
                                      static_cast<int>(base), filename);
                        std::snprintf(tile->objName, sizeof tile->objName, "%.*s_obj0.adt",
                                      static_cast<int>(base), filename);
                        tile->buffers.rootBuf  = buf;
                        tile->buffers.rootSize = size;
                        tile->stage    = Stage::Root;
                        tile->curAsync = async;

                        At<void*>(async, wld::kOffAsyncFile)     = file;
                        At<void*>(async, wld::kOffAsyncBuffer)   = buf;
                        At<uint32_t>(async, wld::kOffAsyncSize)  = size;
                        At<void*>(async, wld::kOffAsyncCtx)      = area;
                        At<void*>(async, wld::kOffAsyncCallback) = reinterpret_cast<void*>(&SplitRootComplete);

                        wxl::game::world::SetTileFileHandle(area, file);
                        wxl::game::world::SetTileFileSize(area, size);
                        wxl::game::world::SetTileFileBuffer(area, buf);
                        wxl::game::world::SetTileAsyncRead(area, async);
                        {
                            std::lock_guard<std::mutex> lock(g_mutex);
                            // Records are keyed by the loading tile object, and that address is
                            // recycled across world loads. A leftover here means a teardown bypassed
                            // the teardown seam, so the record is dropped rather than inherited by the
                            // new tile -- its raw buffers are left alone, since ownership of them can
                            // no longer be established.
                            if (g_tiles.erase(area) != 0)
                            {
                                g_statTilesResident.fetch_sub(1, std::memory_order_relaxed);
                                WLOG_WARN("adt-split: stale tile record found on a recycled tile slot, dropped");
                            }
                            g_tiles[area] = std::move(tile);
                        }
                        g_statTilesResident.fetch_add(1, std::memory_order_relaxed);
                        AsyncEnqueue(async);
                        return;
                    }
                    if (buf) FreeRaw(buf, size);
                    CloseFile(file);
                }
                g_statFailures.fetch_add(1, std::memory_order_relaxed);
                WLOG_WARN("adt-split: split arming failed for '%s', falling back to the native load", filename);
            }
        }
        g_origTileAreaLoad(area, edx, filename);
    }

    /**
     * @brief Detours the chunk sub-chunk walk (the sequential walk over MCNK data).
     *
     * For a chunk of a completed split tile the walk is REPLACED by direct pointer assignment: the
     * chunk+0x11C..+0x13C sub-chunk slots aim straight into the three resident split buffers (and the
     * tiny MCRF pool), zero payload copies. Non-split chunks run the untouched original.
     */
    void __fastcall hkProcessIffChunks(void* chunk, void* edx, int firstBuild)
    {
        // Fast path: with no split tile resident (every classic map, always) the stock walk runs
        // with only a single relaxed atomic load added -- no owner-link deref, no lock, no hash lookup.
        if (g_statTilesResident.load(std::memory_order_relaxed) == 0)
        {
            g_origProcessIff(chunk, edx, firstBuild);
            return;
        }

        auto* mc = static_cast<adt::MapChunk*>(chunk);

        // chunk -> owning area, the engine's own link arithmetic: *((link & ~1) + 8)
        void* area = nullptr;
        const uint32_t link = mc->texOwnerSrc;
        if (link != 0 && (link & 1u) == 0)
            area = *reinterpret_cast<void**>(static_cast<uintptr_t>(link) + 8);

        const ChunkFill* f = nullptr;
        if (area)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            SplitTile* t = FindTileLocked(area);
            if (t && t->complete)
            {
                const int lx = mc->indexX;
                const int ly = mc->indexY;
                const uint32_t idx = static_cast<uint32_t>(ly) * 16u + static_cast<uint32_t>(lx);
                auto* raw = reinterpret_cast<uint8_t*>(mc->rawMcnk);
                if (idx < 256 && t->chunks[idx].rootMcnkHdr == raw)
                    f = &t->chunks[idx];
                else
                    WLOG_WARN("adt-split: chunk (%d,%d) raw MCNK mismatch, using stock walk", lx, ly);
            }
        }
        if (!f)
        {
            g_origProcessIff(chunk, edx, firstBuild);
            return;
        }

        (void)firstBuild;
        mc->mcnkHeader = mc->rawMcnk + 8;                                     // 0x80-byte SMChunk header
        mc->mcvt = reinterpret_cast<uint32_t>(f->mcvt);
        mc->mccv = reinterpret_cast<uint32_t>(f->mccv);
        mc->mcnr = reinterpret_cast<uint32_t>(f->mcnr);
        mc->mcsh = reinterpret_cast<uint32_t>(f->mcsh);
        mc->mcly = reinterpret_cast<uint32_t>(f->mcly);
        mc->mcal = reinterpret_cast<uint32_t>(f->mcal);
        mc->mcrf = reinterpret_cast<uint32_t>(f->mcrf);
        mc->mclq = reinterpret_cast<uint32_t>((f->mclq && f->mclqSize) ? f->mclq : nullptr);
        mc->mcse = reinterpret_cast<uint32_t>((f->mcse && f->mcseSize >= 0x1C) ? f->mcse : nullptr);
        g_statChunksFilled.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Detours the tile-area destructor (tile teardown, frees area+0x80).
     *
     * Releases the tile's side record AFTER the original body -- by then every chunk of the tile is
     * already purged, so no chunk-object pointer into the _tex0/_obj0 buffers can outlive them. Handles
     * the purge-cancel ownership transfer: a DEFERRED cancel (read in service) hands the in-flight
     * stage's buffer to the engine's deferred cleanup and zeroes area+0x80 -- in that case the
     * transferred buffer is NOT freed here, and the orphaned root buffer IS. `t->stage` names whichever
     * of tex/obj was CURRENTLY mirrored at area+0x70 at the moment of teardown (see UpdateVisibleAsync)
     * -- exactly the one the stock purge could have deferred-cancelled, so it alone is checked here.
     *
     * Confirmed by testing: the map's purge-area routine always reaches this destructor (via its purge
     * path then the scalar-deleting-destructor thunk) before recycling the tile area, on every teardown
     * path, including an out-of-rect purge mid-load -- so this detour is the right and only place a
     * concurrent tex/obj pair's un-mirrored member (see below) can be released.
     */
    void __fastcall hkTileAreaDestroy(void* area)
    {
        std::unique_ptr<SplitTile> t;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_tiles.find(area);
            if (it != g_tiles.end())
            {
                t = std::move(it->second);
                g_tiles.erase(it);
            }
        }

        if (t)
        {
            // A background parse (FinalizeTile's pool submission) may still be running against this
            // tile's buffers -- the ONLY thing that can still touch `t` from another thread. Wait for
            // it here, before touching or freeing anything below. Cleared by Stage1Parse itself, on the
            // worker thread, the instant ParseAndPatchGuarded returns -- never by stage2 (which runs on
            // this same main thread and would deadlock this wait if it were the one clearing it).
            std::unique_lock<std::mutex> lock(t->parseMutex);
            t->parseDone.wait(lock, [&] { return !t->parsing; });
        }

        bool freeRootAfter = false;
        if (t)
        {
            if (!t->complete)
            {
                const bool deferredCancel =
                    wxl::game::world::TileFileBuffer(area) == nullptr && t->buffers.rootBuf != nullptr;
                if (deferredCancel)
                {
                    if (t->stage == Stage::Tex)      t->buffers.texOwned = false; // deferred cleanup frees it
                    else if (t->stage == Stage::Obj) t->buffers.objOwned = false;
                    if (t->stage != Stage::Root)     freeRootAfter = true; // +0x80 zeroed, root orphaned
                }
            }
            // Whichever of {root, tex, obj} is CURRENTLY mirrored at area+0x70 may already have been
            // cancelled by the stock purge path (pending reads null then) -- retire it ourselves only
            // if the stock path did not already take it, to avoid a double AsyncRetire on the same
            // object.
            void* pending = wxl::game::world::TileAsyncRead(area);
            if (pending)
            {
                if      (pending == t->curAsync) t->curAsync = nullptr;
                else if (pending == t->texAsync) t->texAsync = nullptr;
                else if (pending == t->objAsync) t->objAsync = nullptr;
                AsyncRetire(pending);
            }
            // A concurrent tex/obj pair's OTHER member -- never mirrored at area+0x70, so never visible
            // to the stock purge -- and root's own async on a teardown that bypassed the purge entirely:
            // nothing else can ever release these but us.
            if (t->curAsync) { AsyncRetire(t->curAsync); t->curAsync = nullptr; }
            if (t->texAsync) { AsyncRetire(t->texAsync); t->texAsync = nullptr; }
            if (t->objAsync) { AsyncRetire(t->objAsync); t->objAsync = nullptr; }
            wxl::game::world::SetTileAsyncRead(area, nullptr);
            wxl::game::world::SetTileFileHandle(area, nullptr);
        }

        g_origTileAreaDestroy(area);

        if (t)
        {
            // Height-blend "_h" handles are module-owned (the stock destructor only frees the tile's own
            // area+0x60 diffuse slots); release each once, after every chunk of the tile purged.
            for (const HeightState::HeightSlot& hs : t->height.heightSlots)
                if (hs.owned && hs.tex)
                    wxl::game::Native<adt::TextureReleaseFn>(adt::kTextureRelease)(hs.tex);
            if (t->buffers.texOwned && t->buffers.texBuf) FreeRaw(t->buffers.texBuf, t->buffers.texSize);
            if (t->buffers.objOwned && t->buffers.objBuf) FreeRaw(t->buffers.objBuf, t->buffers.objSize);
            if (freeRootAfter)            FreeRaw(t->buffers.rootBuf, t->buffers.rootSize);
            g_statTilesResident.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Detours the tile-area update -- the true readiness gate for a split tile's chunk grid.
     *
     * area+0x70 == null is the only signal the map's pre-update-areas pass uses before calling into
     * tile-area update, and it goes null the moment both _tex0/_obj0 reads finish, not once the
     * (now-backgrounded) parse and its TileAreaCreate call have actually run -- see
     * UpdateVisibleAsync's doc comment for why area+0x70 itself can't be the thing held back instead.
     * Update's own body has at least two separate places that assume a grid slot, once built, is a
     * valid chunk pointer -- the prepare-chunk step's first read is area+0x88 (the MCIN pointer only
     * TileAreaCreate ever writes), and a second, unguarded step further down reads the SAME pointer
     * back out of the grid for a per-chunk visibility check with no null check of its own (confirmed
     * off two real crash dumps -- gating the prepare-chunk step alone left this second one open).
     * Update is its only caller, so skipping the whole call for a managed tile that isn't `complete`
     * yet is the one guard that actually covers every internal consumer: the area's own +0xBC grid
     * simply stays at all-zero for one more frame, exactly as if nothing in view needed building yet,
     * and the map's pre-update-areas pass retries it next frame. Non-split areas and already-complete
     * tiles run the untouched original body.
     */
    void __fastcall hkAreaUpdate(void* area, void* edx, int buildFlag, uint32_t* bounds)
    {
        // Fast path: same reasoning as hkProcessIffChunks -- with no split tile resident (every
        // classic map, always) this adds only one relaxed atomic load to the per-frame walk.
        if (g_statTilesResident.load(std::memory_order_relaxed) != 0)
        {
            if (SplitTile* t = FindTileBrief(area); t && !t->complete) return;
        }
        g_origAreaUpdate(area, edx, buildFlag, bounds);
    }
}

namespace wxl::runtime::adtsplit::detail
{
    /**
     * @brief Installs the split-ADT detours: the load seam, per-chunk fill seam, teardown seam, and
     *        (delegated) the split WDL guard and the split-tile texture manager.
     */
    bool InstallAdtSplit()
    {
        wxl_modern_adt::HookAttachByName("Adt.TileAreaLoad", &hkTileAreaLoad, &g_origTileAreaLoad);
        wxl_modern_adt::HookAttachByName("Adt.ProcessIffChunks", &hkProcessIffChunks, &g_origProcessIff);
        wxl_modern_adt::HookAttachByName("Adt.TileAreaDestroy", &hkTileAreaDestroy, &g_origTileAreaDestroy);
        wxl_modern_adt::HookAttachByName("Adt.AreaUpdate", &hkAreaUpdate, &g_origAreaUpdate);
        InstallWdl();
        InstallTextures();
        return true;
    }
}

// ---------------------------------------------------------------- public query surface
namespace wxl::runtime::adtsplit
{
    Stats GetStats()
    {
        Stats s{};
        s.splitMapsDetected = detail::g_statSplitMaps.load(std::memory_order_relaxed);
        s.tilesLoaded       = detail::g_statTilesLoaded.load(std::memory_order_relaxed);
        s.tilesResident     = detail::g_statTilesResident.load(std::memory_order_relaxed);
        s.chunksFilled      = detail::g_statChunksFilled.load(std::memory_order_relaxed);
        s.mcrfBytes         = detail::g_statMcrfBytes.load(std::memory_order_relaxed);
        s.parkedMtxpTiles   = detail::g_statMtxpTiles.load(std::memory_order_relaxed);
        s.parkedMclvChunks  = detail::g_statMclvChunks.load(std::memory_order_relaxed);
        s.parkedHoleChunks  = detail::g_statHoleChunks.load(std::memory_order_relaxed);
        s.loadFailures      = detail::g_statFailures.load(std::memory_order_relaxed);
        s.wdlRead           = detail::g_statWdlRead.load(std::memory_order_relaxed);
        s.heightTexLoaded   = detail::g_statHeightTex.load(std::memory_order_relaxed);
        s.doodadModels      = detail::g_statDoodadModels.load(std::memory_order_relaxed);
        s.mapObjects        = detail::g_statMapObjects.load(std::memory_order_relaxed);
        s.mapObjectsDropped = detail::g_statMapObjectsDropped.load(std::memory_order_relaxed);
        s.liquidLayers      = detail::g_statLiquidLayers.load(std::memory_order_relaxed);
        s.liquidDegraded    = detail::g_statLiquidDegraded.load(std::memory_order_relaxed);
        s.liquidOob         = detail::g_statLiquidOob.load(std::memory_order_relaxed);
        return s;
    }

    uint32_t ResidentTilesRelaxed()
    {
        return detail::g_statTilesResident.load(std::memory_order_relaxed);
    }

    int IsSplitMap()
    {
        // Compose the same "<dir>\<name>" prefix the tile-name keying uses from the live loader globals
        // (phasing swaps them per load, but the base map's key is what a user asks about).
        const char* dir  = reinterpret_cast<const char*>(wxl::offsets::game::world::kMapDirStr);
        const char* name = reinterpret_cast<const char*>(wxl::offsets::game::world::kMapNameStr);
        if (!dir[0] || !name[0]) return -1;
        std::string key(dir);
        key += '\\';
        key += name;
        std::lock_guard<std::mutex> lock(detail::g_mutex);
        auto it = detail::g_splitMaps.find(key);
        if (it == detail::g_splitMaps.end()) return -1;
        return it->second ? 1 : 0;
    }

    namespace
    {
        void FillStatus(const detail::SplitTile& t, TileStatus& out)
        {
            out.tileFirst       = t.tileFirst;
            out.tileSecond      = t.tileSecond;
            out.rootSize        = t.buffers.rootSize;
            out.texSize         = t.buffers.texSize;
            out.objSize         = t.buffers.objSize;
            out.chunkCount      = t.chunkCount;
            out.complete        = t.complete;
            out.hasMtxp         = t.height.mtxp != nullptr;
            out.mclvChunks      = t.mclvChunks;
            out.hiResHoleChunks = t.hiResHoleChunks;
        }
    }

    uint32_t ResidentTileCount()
    {
        std::lock_guard<std::mutex> lock(detail::g_mutex);
        return static_cast<uint32_t>(detail::g_tiles.size());
    }

    bool GetResidentTile(uint32_t index, TileStatus& out)
    {
        std::lock_guard<std::mutex> lock(detail::g_mutex);
        uint32_t i = 0;
        for (const auto& kv : detail::g_tiles)
        {
            if (i++ == index)
            {
                FillStatus(*kv.second, out);
                return true;
            }
        }
        return false;
    }

    bool FindTile(int tileFirst, int tileSecond, TileStatus& out)
    {
        std::lock_guard<std::mutex> lock(detail::g_mutex);
        for (const auto& kv : detail::g_tiles)
        {
            if (kv.second->tileFirst == tileFirst && kv.second->tileSecond == tileSecond)
            {
                FillStatus(*kv.second, out);
                return true;
            }
        }
        return false;
    }
}
