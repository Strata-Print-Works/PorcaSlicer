#pragma once

#include <slic3r/GUI/GUI_App.hpp>

#include <pybind11/pybind11.h>

#include <wx/app.h>

#include <future>
#include <stdexcept>
#include <type_traits>

namespace Slic3r {

// Run a (pure C++/wx) callable on the main/UI thread, blocking the caller until it
// completes, with the GIL released across the wait. If already on the main thread,
// run inline (also with the GIL released so other Python threads run).
//
// Everything that touches presets, tabs or windows has to go through here: plugin
// code runs on its own thread, and wx is not thread-safe. Do NOT call this from a
// slicing pipeline hook — that hook runs on the slicing worker, which the UI thread
// can be blocked waiting on, so the marshaled call would deadlock.
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

    std::promise<R> prom;
    std::future<R>  fut = prom.get_future();

    pybind11::gil_scoped_release nogil;
    GUI::wxGetApp().CallAfter([&prom, &fn]() {
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
                prom.set_value();
            } else {
                prom.set_value(fn());
            }
        } catch (...) {
            prom.set_exception(std::current_exception());
        }
    });
    return fut.get();
}

} // namespace Slic3r
