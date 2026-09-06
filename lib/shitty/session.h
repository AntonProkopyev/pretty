/*
 * Copyright (C) 2026 Shitty team
 * MIT licensed
 * See the file LICENSE.MIT for the full license.
 */

#pragma once

#include <lib/vterm/pty.h>
#include <std/str/view.h>

#include <signal.h>
#include <stddef.h>

struct Composer;
struct Vterm;

// The terminals behind one window, and which of them the window shows.
struct SessionSet {
    // The active session's terminal - the one authoritative answer to
    // "which terminal is the window's". Session creation, selection and
    // death are driven by the tab actions registered at create().
    virtual Vterm* activeTerminal() const = 0;
    // The tab model a window chrome projects: the live sessions in
    // visual order. Every model mutation and every title change commits
    // its state first and then notifies
    // composer.sessionsChangedListeners.
    virtual size_t count() const = 0;
    virtual size_t activeIndex() const = 0;
    virtual u64 identity(size_t index) const = 0;
    // The session's last published title; empty until its shell set one.
    virtual stl::StringView title(size_t index) const = 0;
    virtual bool pinned(size_t index) const = 0;
    virtual void pin(size_t index, bool value) = 0;
    virtual void rename(size_t index, stl::StringView title) = 0;
    virtual void move(size_t from, size_t to) = 0;
    virtual stl::StringView directory(size_t index) const = 0;
    virtual bool newSessionNear(size_t index) = 0;
    virtual void activate(size_t index) = 0;
    virtual void newSession() = 0;
    // False when the closed session was the last one: the caller owns
    // the decision to close the window.
    virtual bool close(size_t index) = 0;
    virtual PtyExitResult lastExit() const {
        return {};
    }
    // The number of live sessions, readable from a signal handler.
    static volatile sig_atomic_t liveSessions;

    static SessionSet* create(Composer& composer);
};
