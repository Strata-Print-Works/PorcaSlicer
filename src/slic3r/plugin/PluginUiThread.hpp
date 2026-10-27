#pragma once

#include "pybind11/gil.h"
#include <exception>
#include <slic3r/GUI/GUI_App.hpp>

#include <pybind11/pybind11.h>

#include <wx/app.h>

#include <future>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <wx/thread.h>

namespace Slic3r {

// Run a (pure C++/wx) callable on the main/UI thread, blocking the caller until it
// completes, with the GIL released across the wait. If already on the main thread,
// run inline (also with the GIL released so other Python threads run).
//
// Everything that touches presets, tabs or windows has to go through here: plugin
// code runs on its own thread, and wx is not thread-safe. Do NOT call this from a
// slicing pipeline hook — that hook runs on the slicing worker, which the UI thread
// can be blocked waiting on, so the marshaled call would deadlock.
// wxTheApp is rechecked because it goes null while a worker may still be waiting on the UI thread.
inline bool app_closing() { return wxTheApp == nullptr || GUI::wxGetApp().is_closing(); }

// State of a call queued by run_on_ui_blocking(). It is at namespace scope because MSVC does not
// find a function-local enumerator from the default member initializer of a local class in a template.
enum : int { UiCallPending, UiCallClaimed, UiCallAbandoned };

// --------------------------------------------------------------------------
// Run a (pure C++/wx) callable on the main/UI thread, blocking the caller until
// it completes, with the GIL released across the wait. If already on the main
// thread, run inline (also with the GIL released so other Python threads run).
//
// A worker thread gives up with a RuntimeError once the app is closing and the
// UI thread has not picked the call up yet: the UI thread may be waiting for
// that worker (a printer agent joining it in disconnect_printer()), or its event
// loop may never run again.
// --------------------------------------------------------------------------
template<typename Fn>
auto run_on_ui_blocking(Fn&& fn) -> std::invoke_result_t<Fn&>
{
    using R = std::invoke_result_t<Fn&>;
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    if (wxIsMainThread()) {
        pybind11::gil_scoped_release nogil;
        return fn();
    }

    if (app_closing())
        throw std::runtime_error("OrcaSlicer is closing");

    // Shared with the queued task, which can outlive an abandoned wait. The task only touches fn
    // after claiming the call, and a claimed call is always waited for, so fn is still alive then.
    struct Call
    {
        std::promise<R>  prom;
        std::atomic<int> state{UiCallPending};
    };
    auto           call = std::make_shared<Call>();
    std::future<R> fut  = call->prom.get_future();

    pybind11::gil_scoped_release nogil;
    GUI::wxGetApp().CallAfter([call, &fn]() {
        int expected = UiCallPending;
        if (!call->state.compare_exchange_strong(expected, UiCallClaimed))
            return;
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
                call->prom.set_value();
            } else {
                call->prom.set_value(fn());
            }
        } catch (...) {
            call->prom.set_exception(std::current_exception());
        }
    });
    while (fut.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
        int expected = UiCallPending;
        if (app_closing() && call->state.compare_exchange_strong(expected, UiCallAbandoned))
            throw std::runtime_error("OrcaSlicer is closing");
    }
    return fut.get();
}


} // namespace Slic3r
