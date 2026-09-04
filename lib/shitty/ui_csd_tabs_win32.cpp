/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#include "ui_csd_tabs.h"

#include "brand.h"
#include "composer.h"
#include "options.h"
#include "session.h"

#include <lib/vterm/listener.h>

#include <plt/window.h>

#include <std/mem/obj_pool.h>

using namespace stl;

namespace {
    struct Win32TabsUi;

    struct CallSessionsChangedWin32 final: Listener {
        explicit CallSessionsChangedWin32(Win32TabsUi& owner_)
            : owner(owner_)
        {
        }

        void onListen(void*) override;

        Win32TabsUi& owner;
    };

    struct Win32TabsUi final: plt::WindowTabs {
        explicit Win32TabsUi(Composer& composer_)
            : composer(composer_)
            , changed(*this)
        {
            composer.sessionsChangedListeners.pushBack(&changed);
            composer.window->requestTabs(this);
        }

        size_t count() const override {
            return composer.sessions == nullptr ? 0 : composer.sessions->count();
        }

        size_t active() const override {
            return composer.sessions == nullptr ? 0 : composer.sessions->activeIndex();
        }

        StringView title(size_t index) const override {
            if (composer.sessions == nullptr) {
                return {};
            }
            const StringView value = composer.sessions->title(index);
            return value.empty() ? composer.brand->displayName() : value;
        }

        plt::WindowColor background() const override {
            const Color color = composer.opts->vt.bg;
            return {color.red, color.green, color.blue};
        }

        plt::WindowColor foreground() const override {
            const Color color = composer.opts->vt.fg;
            return {color.red, color.green, color.blue};
        }

        void select(size_t index) override {
            if (composer.sessions != nullptr && index < composer.sessions->count()) {
                composer.sessions->activate(index);
                composer.window->requestFrame();
            }
        }

        void close(size_t index) override {
            if (composer.sessions == nullptr || index >= composer.sessions->count()) {
                return;
            }
            if (composer.sessions->close(index)) {
                composer.window->requestFrame();
            } else {
                composer.window->requestClose();
            }
        }

        void open() override {
            if (composer.sessions != nullptr) {
                composer.sessions->newSession();
                composer.window->requestFrame();
            }
        }

        Composer& composer;
        CallSessionsChangedWin32 changed;
    };

    void CallSessionsChangedWin32::onListen(void*) {
        owner.composer.window->requestTabsRedraw();
    }
}

void createCsdTabsWin32Ui(ObjPool& owner, Composer& composer) {
    owner.make<Win32TabsUi>(composer);
}
