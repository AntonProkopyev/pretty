/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "pty.h"
#include "session.h"
#include "startup.h"
#include "composer.h"

#include <lib/vterm/vterm.h>
#include <lib/vterm/listener.h>

#include <std/tst/ut.h>
#include <std/ios/out.h>
#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#include <plt/fiber.h>
#include <plt/platform.h>
#include <plt/platform_headless.h>

using namespace stl;

namespace {
    // A trivially owned chunk for the stub: header and payload in one
    // small-obj allocation, released on send.
    struct StubChunk final: public PtyHandle::Chunk, public stl::Newable {
        void* data() override {
            return this + 1;
        }

        size_t length() override {
            return used;
        }

        Chunk* next() override {
            return nullptr;
        }

        SmallObjAllocator* owner = nullptr;
        u32 allocated = 0;
        u32 used = 0;
    };

    struct StubHandle final: public PtyHandle {
        StubHandle(Composer& composer_, size_t* destroyed_, bool* writeEntered, bool* writeResumed)
            : composer(composer_)
            , destroyed(destroyed_)
            , entered(writeEntered)
            , resumed(writeResumed)
        {
        }

        ~StubHandle() noexcept {
            ++*destroyed;
        }

        void resize(const PtySize& requested) override {
            size = requested;
            ++resizes;
        }

        void engage() override {
        }

        Chunk* allocate(size_t len) override {
            constexpr size_t cap = smallObjMaxSize - sizeof(StubChunk);
            const size_t granted = len < cap ? len : cap;
            auto* const chunk = new (composer.smallObjects->allocate(sizeof(StubChunk) + granted)) StubChunk;
            chunk->owner = composer.smallObjects;
            chunk->allocated = (u32)(sizeof(StubChunk) + granted);
            chunk->used = (u32)(granted);
            return chunk;
        }

        void send(Chunk* chunk, size_t len) override {
            auto* const block = static_cast<StubChunk*>(chunk);
            if (log != nullptr) {
                log->append(block->data(), len);
            }
            if (entered != nullptr) {
                *entered = true;
                composer.scheduler->current()->park();
                *resumed = true;
            }
            block->owner->deallocate(block, block->allocated);
        }

        Chunk* acquire() override {
            for (;;) {
                composer.scheduler->current()->park();
            }
        }

        void release(Chunk*) override {
        }

        pid_t foregroundProcessGroup() override {
            return 0;
        }

        Composer& composer;
        size_t* destroyed;
        bool* entered;
        bool* resumed;
        Buffer* log = nullptr;
        PtySize size{};
        size_t resizes = 0;
    };

    struct StubPty final: public Pty {
        StubPty(Composer& composer_, size_t& destroyed_)
            : composer(composer_)
            , destroyed(destroyed_)
        {
        }

        PtyHandle* spawn(ObjPool& owner, const LaunchCommand& command) override {
            directory.reset();
            directory.append(command.directory.data(), command.directory.used());
            StubHandle* const handle = owner.make<StubHandle>(composer, &destroyed, blockNextWrite ? &writeEntered : nullptr, blockNextWrite ? &writeResumed : nullptr);
            blockNextWrite = false;
            handles.pushBack(handle);
            return handle;
        }

        Composer& composer;
        Vector<StubHandle*> handles;
        size_t& destroyed;
        bool blockNextWrite = false;
        bool writeEntered = false;
        bool writeResumed = false;
        Buffer directory;
    };

    void publish(IntrusiveList& listeners) {
        for (IntrusiveNode* node = listeners.mutFront(); node != listeners.mutEnd();) {
            Listener* const listener = static_cast<Listener*>(node);
            node = node->next;
            listener->onListen();
        }
    }

    struct Harness {
        explicit Harness(size_t* destroyed = nullptr)
            : composer(*pool->make<Composer>(pool.mutPtr()))
            , pty(composer, destroyed == nullptr ? ownedDestroyed : *destroyed)
        {
            composer.platform = plt::createHeadlessPlatform(*composer.pool);
            composer.window = composer.platform->createWindow(*composer.pool, {.width = 80, .height = 24});
            composer.installVtHost();
            composer.geometry.setCellPixelSize(1, 1);
            composer.resizeWindow(80, 24);
            composer.pty = &pty;
            composer.launch = &command;
            sessions = SessionSet::create(composer);
        }

        void newTab() {
            publish(composer.newTabListeners);
        }

        void closeTab() {
            publish(composer.closeTabListeners);
        }

        void nextTab() {
            publish(composer.nextTabListeners);
        }

        void previousTab() {
            publish(composer.prevTabListeners);
        }

        size_t ownedDestroyed = 0;
        ObjPool::Ref pool = ObjPool::fromMemory();
        Composer& composer;
        LaunchCommand command;
        StubPty pty;
        SessionSet* sessions = nullptr;
    };
}

namespace {
    struct ModelProbe final: public Listener {
        explicit ModelProbe(SessionSet* sessions_);

        void onListen(void*) override;

        SessionSet* sessions;
        size_t count = 0;
        size_t active = 0;
        unsigned notified = 0;
    };
}

ModelProbe::ModelProbe(SessionSet* sessions_)
    : sessions(sessions_)
{
}

void ModelProbe::onListen(void*) {
    count = sessions->count();
    active = sessions->activeIndex();
    ++notified;
}

STD_TEST_SUITE(SessionSet) {
    STD_TEST(TabMetadataFollowsTheSessionAcrossMovesAndTitles) {
        Harness harness;
        harness.newTab();
        harness.newTab();
        Vterm* const visible = harness.sessions->activeTerminal();
        const u64 identity = harness.sessions->identity(2);
        harness.sessions->rename(2, StringView(u8"Build logs"));
        harness.sessions->move(2, 0);
        STD_INSIST(harness.sessions->activeTerminal() == visible);
        STD_INSIST(harness.sessions->activeIndex() == 0);
        STD_INSIST(harness.sessions->identity(0) == identity);
        const StringView title(u8"\x1b]2;automatic title\x07");
        visible->feedPty(title);
        STD_INSIST(harness.sessions->title(0) == StringView(u8"Build logs"));
        harness.sessions->rename(0, {});
        STD_INSIST(harness.sessions->title(0) == StringView(u8"automatic title"));
        harness.sessions->pin(0, true);
        harness.sessions->move(0, 2);
        STD_INSIST(harness.sessions->pinned(0));
        STD_INSIST(harness.sessions->identity(0) == identity);
        harness.sessions->move(2, 0);
        STD_INSIST(harness.sessions->identity(0) == identity);
        harness.sessions->pin(2, true);
        harness.sessions->pin(0, false);
        STD_INSIST(harness.sessions->pinned(0));
        STD_INSIST(!harness.sessions->pinned(1));
        STD_INSIST(harness.sessions->identity(1) == identity);
        STD_INSIST(harness.sessions->activeTerminal() == visible);
    }

    STD_TEST(NewNeighbourUsesReportedDirectoryWithoutChangingOtherSessions) {
        Harness harness;
        STD_INSIST(!harness.sessions->newSessionNear(0));
        harness.newTab();
        harness.sessions->activate(0);
        const u64 identity = harness.sessions->identity(0);
        const StringView cwd(u8"\x1b]7;file://localhost/tmp/build%20space\x07");
        harness.sessions->activeTerminal()->feedPty(cwd);
        STD_INSIST(harness.sessions->newSessionNear(0));
        STD_INSIST(harness.sessions->count() == 3);
        STD_INSIST(harness.sessions->activeIndex() == 1);
        STD_INSIST(harness.sessions->identity(0) == identity);
        STD_INSIST(StringView(harness.pty.directory) == StringView(u8"/tmp/build space"));
        STD_INSIST(!harness.sessions->newSessionNear(99));
        harness.sessions->move(99, 0);
        harness.sessions->rename(99, StringView(u8"ignored"));
        STD_INSIST(harness.sessions->count() == 3);
    }

    STD_TEST(TabModelCommitsBeforeItNotifies) {
        Harness harness;
        ModelProbe probe{harness.sessions};
        harness.composer.sessionsChangedListeners.pushBack(&probe);

        harness.newTab();
        STD_INSIST(probe.notified != 0);
        STD_INSIST(probe.count == 2);
        STD_INSIST(probe.active == 1);
        STD_INSIST(harness.sessions->title(0).length() == 0);

        harness.sessions->activate(0);
        STD_INSIST(probe.active == 0);

        harness.closeTab();
        STD_INSIST(probe.count == 1);
        STD_INSIST(probe.active == 0);
    }

    STD_TEST(DirectSelectionChordsPickTheirTab) {
        Harness harness;
        harness.newTab();
        harness.newTab();
        STD_INSIST(harness.sessions->activeIndex() == 2);

        publish(harness.composer.selectTabListeners[0]);
        STD_INSIST(harness.sessions->activeIndex() == 0);

        // The ninth chord means "the last tab" however many there are.
        publish(harness.composer.selectTabListeners[8]);
        STD_INSIST(harness.sessions->activeIndex() == 2);

        // Out-of-range and already-active chords change nothing.
        publish(harness.composer.selectTabListeners[6]);
        STD_INSIST(harness.sessions->activeIndex() == 2);
        publish(harness.composer.selectTabListeners[2]);
        STD_INSIST(harness.sessions->activeIndex() == 2);
    }

    STD_TEST(ClosingABackgroundTabKeepsTheViewPut) {
        Harness harness;
        ModelProbe probe{harness.sessions};
        harness.composer.sessionsChangedListeners.pushBack(&probe);

        harness.newTab();
        harness.newTab();
        STD_INSIST(harness.sessions->activeIndex() == 2);
        Vterm* const watched = harness.sessions->activeTerminal();

        STD_INSIST(harness.sessions->close(0));
        STD_INSIST(harness.sessions->activeTerminal() == watched);
        STD_INSIST(probe.count == 2);
        STD_INSIST(probe.active == 1);

        STD_INSIST(harness.sessions->close(0));
        STD_INSIST(harness.sessions->activeTerminal() == watched);
        STD_INSIST(probe.count == 1);
        STD_INSIST(probe.active == 0);
    }

    STD_TEST(CreateOpensAndActivatesTheFirstSession) {
        Harness harness;

        STD_INSIST(SessionSet::liveSessions == 1);
        STD_INSIST(harness.pty.handles.length() == 1);
        STD_INSIST(harness.sessions->activeTerminal() != nullptr);
        STD_INSIST(harness.pty.handles[0]->resizes == 1);
    }

    STD_TEST(NewTabUsesTheSameProductionSpawnPath) {
        Harness harness;
        Vterm* const first = harness.sessions->activeTerminal();

        harness.newTab();

        STD_INSIST(SessionSet::liveSessions == 2);
        STD_INSIST(harness.pty.handles.length() == 2);
        STD_INSIST(harness.sessions->activeTerminal() != first);
    }

    STD_TEST(NextAndPreviousWrapAround) {
        Harness harness;
        Vterm* const first = harness.sessions->activeTerminal();
        harness.newTab();
        Vterm* const second = harness.sessions->activeTerminal();
        harness.newTab();
        Vterm* const third = harness.sessions->activeTerminal();

        harness.nextTab();
        STD_INSIST(harness.sessions->activeTerminal() == first);
        harness.nextTab();
        STD_INSIST(harness.sessions->activeTerminal() == second);
        harness.previousTab();
        STD_INSIST(harness.sessions->activeTerminal() == first);
        harness.previousTab();
        STD_INSIST(harness.sessions->activeTerminal() == third);
    }

    STD_TEST(SwitchingOneSessionStaysPut) {
        Harness harness;
        Vterm* const only = harness.sessions->activeTerminal();

        harness.nextTab();
        harness.previousTab();

        STD_INSIST(harness.sessions->activeTerminal() == only);
    }

    STD_TEST(CloseTabDestroysItsWholeArena) {
        Harness harness;
        Vterm* const first = harness.sessions->activeTerminal();
        harness.newTab();

        harness.closeTab();

        STD_INSIST(SessionSet::liveSessions == 1);
        STD_INSIST(harness.pty.destroyed == 1);
        STD_INSIST(harness.sessions->activeTerminal() == first);
    }

    STD_TEST(ClosingTheLastSessionReportsNoLiveSessions) {
        Harness harness;

        harness.closeTab();

        STD_INSIST(SessionSet::liveSessions == 0);
    }

    STD_TEST(TeardownReleasesEverySessionArena) {
        size_t destroyed = 0;
        {
            Harness harness(&destroyed);
            harness.newTab();
        }

        STD_INSIST(destroyed == 2);
        STD_INSIST(SessionSet::liveSessions == 0);
    }

    STD_TEST(ResizeReachesEverySessionHandle) {
        Harness harness;
        harness.newTab();
        const size_t firstResizes = harness.pty.handles[0]->resizes;
        const size_t secondResizes = harness.pty.handles[1]->resizes;

        harness.composer.resizeWindow(100, 40);

        STD_INSIST(harness.pty.handles[0]->resizes == firstResizes + 1);
        STD_INSIST(harness.pty.handles[1]->resizes == secondResizes + 1);
    }

    STD_TEST(ClosingReleasesAParkedClientWriteFiber) {
        Harness harness;
        harness.pty.blockNextWrite = true;
        harness.newTab();

        harness.sessions->activeTerminal()->sendBytes(StringView(u8"x"), true);
        STD_INSIST(harness.pty.writeEntered);
        STD_INSIST(!harness.pty.writeResumed);

        harness.closeTab();

        STD_INSIST(harness.pty.destroyed == 1);
        STD_INSIST(!harness.pty.writeResumed);
    }

    // The natural-editing chords translate to their readline escapes on
    // the active session's PTY, in publish order.
    STD_TEST(ReadlineChordsReachThePty) {
        Harness harness;
        Buffer sent;
        harness.pty.handles[0]->log = &sent;

        publish(harness.composer.wordLeftListeners);
        publish(harness.composer.wordRightListeners);
        publish(harness.composer.lineStartListeners);
        publish(harness.composer.lineEndListeners);
        publish(harness.composer.killLineListeners);
        publish(harness.composer.eraseWordListeners);

        STD_INSIST(StringView(sent) == StringView(u8"\033b\033f\x01\x05\x15\x1b\x7f"));
    }

    STD_TEST(SessionCountDoesNotLengthenTheInputChain) {
        Harness harness;
        harness.newTab();
        harness.newTab();
        size_t handlers = 0;
        for (IntrusiveNode* node = harness.composer.inputHandlers.mutFront(); node != harness.composer.inputHandlers.mutEnd(); node = node->next) {
            ++handlers;
        }

        // InputBindings and the one SessionSet handler.
        STD_INSIST(handlers == 2);
    }
}
