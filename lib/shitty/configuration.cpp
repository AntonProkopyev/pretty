/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "configuration.h"

#include "brand.h"
#include "options.h"
#include "composer.h"

#include <lib/vterm/listener.h>

#include <std/ios/sys.h>
#include <std/str/view.h>
#include <std/sys/throw.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <plt/poller.h>
#include <plt/platform.h>
#if !defined(_WIN32)
    #include <std/sys/event_fd.h>
    #include <signal.h>
#endif

using namespace stl;

namespace {
    struct ConfigImpl;

#if !defined(_WIN32)
    EventFD* reloadEvent = nullptr;

    void reloadSignalHandler(int) {
        if (reloadEvent != nullptr) {
            reloadEvent->signal();
        }
    }
#endif

    struct ConfigImpl final: public Config
#if !defined(_WIN32)
        , public plt::PollCallback
#endif
    {
        explicit ConfigImpl(Composer& composer);
        ~ConfigImpl() noexcept;

        void initialize(int* argc, char* argv[]) override;
        void start() override;
        void stop() override;
        void reload() override;
#if !defined(_WIN32)
        void ready(PollFD) override;
#endif

        Options* load(ObjPool& owner, int* argc, char* argv[], OptionsLoad mode);
        void publish(ObjPool* nextPool, Options* next);
        void warn(StringView message);

        Composer& composer;
        ObjPool* optionsPool = nullptr;
        Vector<StringView> arguments;
#if !defined(_WIN32)
        EventFD event;
        plt::PollWaiter waiter;
        struct sigaction previousAction{};
#endif
        bool started = false;
    };
}

ConfigImpl::ConfigImpl(Composer& composer_)
    : composer(composer_)
{
}

ConfigImpl::~ConfigImpl() noexcept {
    stop();
    delete optionsPool;
}

Options* ConfigImpl::load(ObjPool& owner, int* argc, char* argv[], OptionsLoad mode) {
    Options* const options = Options::create(owner, *composer.brand, argv, *argc, mode);
    *argc = 0;
    while (argv[*argc] != nullptr) {
        ++*argc;
    }
    if (*argc > 2 && StringView(argv[1]) == StringView(u8"-e") && options->titleSource != OptionSource::CmdLine && options->titleSource != OptionSource::Config) {
        options->vt.title = owner.intern(StringView(argv[2]));
    }
    return options;
}

void ConfigImpl::initialize(int* argc, char* argv[]) {
    for (int index = 0; index < *argc; ++index) {
        arguments.pushBack(composer.pool->intern(StringView(argv[index])));
    }

    ObjPool* const nextPool = ObjPool::fromMemoryRaw();
    try {
        Options* const next = load(*nextPool, argc, argv, OptionsLoad::Startup);
        optionsPool = nextPool;
        composer.setOptions(next);
    } catch (...) {
        delete nextPool;
        throw;
    }
}

void ConfigImpl::start() {
    if (started) {
        return;
    }
#if !defined(_WIN32)
    waiter.fd = {
        .fd = event.fd(),
        .flags = PollFlag::In,
    };
    waiter.callback = this;
    composer.platform->poller()->arm(waiter);

    reloadEvent = &event;
    struct sigaction action{};
    action.sa_handler = reloadSignalHandler;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, &previousAction) < 0) {
        reloadEvent = nullptr;
        composer.platform->poller()->cancel(waiter);
        Errno().raise(StringView(u8"can't install SIGUSR1 handler: sigaction()"));
    }
#endif
    started = true;
}

void ConfigImpl::stop() {
    if (!started) {
        return;
    }
#if !defined(_WIN32)
    sigset_t blocked;
    sigset_t previousMask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blocked, &previousMask);
    sigaction(SIGUSR1, &previousAction, nullptr);
    reloadEvent = nullptr;
    composer.platform->poller()->cancel(waiter);
    sigprocmask(SIG_SETMASK, &previousMask, nullptr);
#endif
    started = false;
}

void ConfigImpl::warn(StringView message) {
    sysE << composer.brand->identifier() << StringView(u8": config reload failed: ") << message << endL;
}

void ConfigImpl::publish(ObjPool* nextPool, Options* next) {
    ObjPool* const previousPool = optionsPool;
    optionsPool = nextPool;
    composer.setOptions(next);
    try {
        for (IntrusiveNode* node = composer.configChangedListeners.mutFront(); node != composer.configChangedListeners.mutEnd();) {
            Listener* const listener = static_cast<Listener*>(node);
            node = node->next;
            listener->onListen();
        }
    } catch (...) {
        delete previousPool;
        throw;
    }
    delete previousPool;
}

void ConfigImpl::reload() {
    ObjPool* const nextPool = ObjPool::fromMemoryRaw();
    Options* next;
    try {
        Vector<char*> argv;
        for (const StringView argument : arguments) {
            argv.pushBack((char*)(argument.data()));
        }
        argv.pushBack(nullptr);
        int argc = (int)(arguments.length());
        next = load(*nextPool, &argc, argv.mutData(), OptionsLoad::Reload);
    } catch (Exception& error) {
        warn(error.description());
        delete nextPool;
        return;
    } catch (...) {
        delete nextPool;
        throw;
    }
    try {
        publish(nextPool, next);
    } catch (Exception& error) {
        // The new snapshot is already canonical and owned by this object;
        // a component failed to rematerialize its dependent state.
        warn(error.description());
    }
}

#if !defined(_WIN32)
void ConfigImpl::ready(PollFD) {
    event.drain();
    composer.platform->poller()->arm(waiter);
    reload();
}
#endif

Config* Config::create(Composer& composer) {
    return composer.pool->make<ConfigImpl>(composer);
}
