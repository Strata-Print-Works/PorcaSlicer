// Input conversion only: no wx application, GL context, fonts or sockets.
#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include <imgui/imgui.h>
#include <wx/event.h>

using Slic3r::GUI::ImGuiWrapper;
using Catch::Matchers::WithinAbs;

TEST_CASE("Motion between a press and release does not release an overlay button", "[ImGuiInput][Regression]")
{
    const int button = GENERATE(0, 1, 2);
    const bool double_click = GENERATE(false, true);
    const wxEventType downs[] = {wxEVT_LEFT_DOWN, wxEVT_RIGHT_DOWN, wxEVT_MIDDLE_DOWN};
    const wxEventType doubles[] = {wxEVT_LEFT_DCLICK, wxEVT_RIGHT_DCLICK, wxEVT_MIDDLE_DCLICK};
    const wxEventType ups[] = {wxEVT_LEFT_UP, wxEVT_RIGHT_UP, wxEVT_MIDDLE_UP};
    ImGuiWrapper imgui;
    imgui.set_display_size(800.f, 600.f);
    // Keep frame/font creation out of this input-conversion test.
    ImGui::GetIO().WantCaptureMouse = false;
    wxMouseEvent press(double_click ? doubles[button] : downs[button]);
    imgui.update_mouse_data(press);
    REQUIRE(ImGui::GetIO().MouseDown[button]);
    for (wxEventType type : {wxEVT_MOTION, wxEVT_ENTER_WINDOW, wxEVT_LEAVE_WINDOW, wxEVT_MOUSEWHEEL}) {
        wxMouseEvent intermediate(type);
        imgui.update_mouse_data(intermediate);
        CHECK(ImGui::GetIO().MouseDown[button]);
    }
    wxMouseEvent release(ups[button]);
    imgui.update_mouse_data(release);
    CHECK_FALSE(ImGui::GetIO().MouseDown[button]);
}

TEST_CASE("Losing capture cancels held overlay buttons", "[ImGuiInput][Regression]")
{
    ImGuiWrapper imgui;
    imgui.set_display_size(800.f, 600.f);
    ImGui::GetIO().MouseDown[0] = true;
    ImGui::GetIO().MouseDown[1] = true;
    ImGui::GetIO().MouseDown[2] = true;
    ImGuiWrapper::cancel_mouse_drag();
    CHECK_FALSE(ImGui::GetIO().MouseDown[0]);
    CHECK_FALSE(ImGui::GetIO().MouseDown[1]);
    CHECK_FALSE(ImGui::GetIO().MouseDown[2]);
}

TEST_CASE("Moving or clicking over an overlay does not scroll it", "[ImGuiInput][Regression]")
{
    const auto type = GENERATE(wxEVT_MOTION, wxEVT_LEFT_DOWN, wxEVT_LEFT_UP, wxEVT_ENTER_WINDOW, wxEVT_LEAVE_WINDOW);
    ImGuiWrapper imgui;
    imgui.set_display_size(800.f, 600.f);
    wxMouseEvent event(type);
    imgui.update_mouse_data(event);
    CHECK_THAT(ImGui::GetIO().MouseWheel, WithinAbs(0., 1e-6));
    CHECK_THAT(ImGui::GetIO().MouseWheelH, WithinAbs(0., 1e-6));
}

TEST_CASE("Overlay scrolling preserves small movements and accumulates a burst", "[ImGuiInput][Regression]")
{
    const int delta = GENERATE(10, 120);
    ImGuiWrapper imgui;
    imgui.set_display_size(800.f, 600.f);
    ImGui::GetIO().WantCaptureMouse = true;
    const int frame = ImGui::GetFrameCount();
    wxMouseEvent event(wxEVT_MOUSEWHEEL);
    event.m_wheelDelta = delta;
    event.m_wheelRotation = 1;
    CHECK(imgui.update_mouse_data(event));
    CHECK(ImGui::GetFrameCount() == frame);
    CHECK_THAT(ImGui::GetIO().MouseWheel, WithinAbs(1. / delta, 1e-6));
    // Native trackpad bursts can interleave pointer motion with wheel events.
    wxMouseEvent motion(wxEVT_MOTION);
    CHECK(imgui.update_mouse_data(motion));
    CHECK(ImGui::GetFrameCount() == frame);
    event.m_wheelRotation = 2;
    imgui.update_mouse_data(event);
    CHECK_THAT(ImGui::GetIO().MouseWheel, WithinAbs(3. / delta, 1e-6));
    event.m_wheelRotation = 0;
    imgui.update_mouse_data(event);
    CHECK_THAT(ImGui::GetIO().MouseWheel, WithinAbs(3. / delta, 1e-6));
    event.m_wheelRotation = -delta;
    imgui.update_mouse_data(event);
    CHECK_THAT(ImGui::GetIO().MouseWheel, WithinAbs(3. / delta - 1., 1e-6));
    CHECK(ImGui::GetFrameCount() == frame);
}

TEST_CASE("Sideways trackpad movements do not move a vertical overlay list", "[ImGuiInput][Regression]")
{
    ImGuiWrapper imgui;
    imgui.set_display_size(800.f, 600.f);
    wxMouseEvent event(wxEVT_MOUSEWHEEL);
    event.m_wheelDelta = 10;
    event.m_wheelRotation = 3;
    event.m_wheelAxis = wxMOUSE_WHEEL_HORIZONTAL;
    imgui.update_mouse_data(event);
    CHECK_THAT(ImGui::GetIO().MouseWheel, WithinAbs(0., 1e-6));
    CHECK_THAT(ImGui::GetIO().MouseWheelH, WithinAbs(-0.3, 1e-6));
}
