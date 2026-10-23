#include "DailyTips.hpp"
#include "DesignTokens.hpp"
#include "WorkspaceStyle.hpp"
#include <algorithm>
#include <utility>
#include <wx/string.h>
#include <string>
#include <functional>
#include <imgui.h>
#include "slic3r/GUI/GLTexture.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include <cstdint>
#include <cstddef>
#include <memory>
#include "slic3r/GUI/HintNotification.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/I18N.hpp"

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

namespace Slic3r { namespace GUI {
namespace {
ImVec4 tip_color(DesignSystem::ColorRole role, bool dark, float opacity = 1.0f)
{
    if (!dark && role == DesignSystem::ColorRole::Accent)
        role = DesignSystem::ColorRole::AccentInk;
    ImVec4 result = ImGuiWrapper::to_ImVec4(WorkspaceStyle::color(role, dark));
    result.w *= opacity;
    return result;
}

std::pair<std::string, std::string> tip_text(const std::string& text)
{
    const size_t split = text.find('\n');
    return split == std::string::npos ? std::make_pair(text, std::string()) :
        std::make_pair(text.substr(0, split), text.substr(split + 1));
}
}



struct DailyTipsData {
    std::string main_text;
    std::string wiki_url;
    std::string img_url;
    std::string additional_text;                  // currently not used
    std::string hyper_text;                       // currently not used
    std::function<void(void)> hypertext_callback; // currently not used
};

class DailyTipsDataRenderer {
public:
    DailyTipsDataRenderer(DailyTipsLayout layout);
    ~DailyTipsDataRenderer();
    void update_data(const DailyTipsData& data);
    void render(const ImVec2& pos, const ImVec2& size) const;
    bool has_image() const;
    float preferred_height(float width) const;
    void on_change_color_mode(bool is_dark);
    void set_fade_opacity(float opacity);

protected:
    void load_texture_from_img_url(const std::string url);
    void open_wiki() const;
    // relative to the window's upper-left position
    void render_img(const ImVec2& start_pos, const ImVec2& size) const;
    void render_text(const ImVec2& start_pos, const ImVec2& size) const;

private:
    DailyTipsData m_data;
    GLTexture* m_texture{ nullptr };
    bool m_is_dark{ false };
    DailyTipsLayout m_layout;
    float m_fade_opacity{ 1.0f };
};

DailyTipsDataRenderer::DailyTipsDataRenderer(DailyTipsLayout layout)
    : m_layout(layout)
{
}

DailyTipsDataRenderer::~DailyTipsDataRenderer() {
    if (m_texture)
        delete m_texture;
}

void DailyTipsDataRenderer::update_data(const DailyTipsData& data)
{
    m_data = data;
    load_texture_from_img_url(m_data.img_url);
}

void DailyTipsDataRenderer::load_texture_from_img_url(const std::string url)
{
    if (m_texture) {
        delete m_texture;
        m_texture = nullptr;
    }
    if (!url.empty()) {
        m_texture = new GLTexture();
        m_texture->load_from_file(Slic3r::resources_dir() + "/" + url, true, GLTexture::None, false);
    }
}

void DailyTipsDataRenderer::open_wiki() const
{
    if (!m_data.wiki_url.empty())
    {
        wxGetApp().CallAfter([url = m_data.wiki_url] {
            if (!wxGetApp().is_closing())
                wxGetApp().open_browser_with_warning_dialog(url);
        });
    }
}

void DailyTipsDataRenderer::render(const ImVec2& pos, const ImVec2& size) const
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    ImGuiWindow* parent_window = ImGui::GetCurrentWindow();
    int window_flags = parent_window->Flags;
    window_flags &= ~ImGuiWindowFlags_NoScrollbar;
    window_flags &= ~ImGuiWindowFlags_NoScrollWithMouse;
    std::string name = "##DailyTipsDataRenderer" + std::to_string(parent_window->ID);
    ImGui::SetNextWindowPos(pos);
    if (ImGui::BeginChild(name.c_str(), size, false, window_flags)) {
        if (m_layout == DailyTipsLayout::Vertical) {
            ImVec2 img_size(0, 0);
            float img_text_gap = 0.0f;
            if (has_image()) {
                img_size = ImVec2(size.x, 9.0f / 16.0f * size.x);
                render_img({0, 0}, img_size);
                img_text_gap = ImGui::CalcTextSize("A").y;
            }
            render_text({ 0, img_size.y + img_text_gap },
                        {ImGui::GetContentRegionAvail().x, size.y});
        }
        if (m_layout == DailyTipsLayout::Horizontal) {
            ImVec2 img_size(0, 0);
            float  img_text_gap = 0.0f;
            if (has_image()) {
                img_size = ImVec2(16.0f / 9.0f * size.y, size.y);
                render_img({0, 0}, img_size);
                img_text_gap = ImGui::CalcTextSize("A").y;
            }
            render_text({img_size.x + img_text_gap, 0}, {size.x - img_size.x - img_text_gap, size.y});
        }
    }
    ImGui::EndChild();
}

bool DailyTipsDataRenderer::has_image() const
{
    return m_texture && m_texture->get_id() != 0;
}

float DailyTipsDataRenderer::preferred_height(float width) const
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const float scale = imgui.get_font_size() / 15.0f;
    const auto text = tip_text(m_data.main_text);
    imgui.push_bold_font();
    float height = ImGui::CalcTextSize(text.first.c_str(), nullptr, false, width).y;
    imgui.pop_bold_font();
    if (!text.second.empty())
        height += 8.0f * scale + ImGui::CalcTextSize(text.second.c_str(), nullptr, false, width).y;
    if (!m_data.wiki_url.empty())
        height += 12.0f * scale + ImGui::GetFontSize() + 8.0f * scale;
    if (has_image())
        height += width * 9.0f / 16.0f + 16.0f * scale;
    return height + 16.0f * scale;
}

void DailyTipsDataRenderer::on_change_color_mode(bool is_dark)
{
    m_is_dark = is_dark;
}

void DailyTipsDataRenderer::set_fade_opacity(float opacity)
{
    m_fade_opacity = opacity;
}

void DailyTipsDataRenderer::render_img(const ImVec2& start_pos, const ImVec2& size) const
{
    if (has_image())
        ImGui::Image((ImTextureID)(intptr_t)m_texture->get_id(), size, ImVec2(0, 0), ImVec2(1, 1), m_is_dark ? ImVec4(0.8, 0.8, 0.8, m_fade_opacity) : ImVec4(1, 1, 1, m_fade_opacity));

}

void DailyTipsDataRenderer::render_text(const ImVec2& start_pos, const ImVec2& size) const
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const float scale = imgui.get_font_size() / 15.0f;
    const auto text = tip_text(m_data.main_text);
    ImGui::SetCursorPos(start_pos);
    ImGui::PushStyleColor(ImGuiCol_Text, tip_color(DesignSystem::ColorRole::Ink, m_is_dark, m_fade_opacity));
    imgui.push_bold_font();
    imgui.text_wrapped(text.first, size.x);
    imgui.pop_bold_font();
    if (!text.second.empty()) {
        ImGui::Dummy(ImVec2(0, 8.0f * scale));
        ImGui::SetCursorPosX(start_pos.x);
        imgui.text_wrapped(text.second, size.x);
    }
    ImGui::PopStyleColor();

    if (!m_data.wiki_url.empty()) {
        ImGui::Dummy(ImVec2(0, 12.0f * scale));
        ImGui::SetCursorPosX(start_pos.x);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, tip_color(DesignSystem::ColorRole::AccentInk, m_is_dark, m_fade_opacity));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tip_color(DesignSystem::ColorRole::AccentSoft, m_is_dark, m_fade_opacity));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, tip_color(DesignSystem::ColorRole::AccentSoft, m_is_dark, m_fade_opacity));
        if (imgui.button(_L("Read more")))
            open_wiki();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
    }
}

int DailyTipsPanel::uid = 0;

DailyTipsPanel::DailyTipsPanel(bool can_expand, DailyTipsLayout layout)
    : m_pos(ImVec2(0, 0)),
    m_width(0),
    m_height(0),
    m_can_expand(can_expand),
    m_uid(DailyTipsPanel::uid++),
    m_dailytips_renderer(std::make_unique<DailyTipsDataRenderer>(layout))
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float scale = imgui.get_font_size() / 15.0f;
    m_footer_height = 30.0f * scale;
    m_is_expanded = wxGetApp().app_config->get("show_hints") == "true";
}

void DailyTipsPanel::set_position(const ImVec2& pos)
{
    m_pos = pos;
}

void DailyTipsPanel::set_size(const ImVec2& size)
{
    m_width = size.x;
    if (!m_can_expand) {
        const float scale = wxGetApp().imgui()->get_font_size() / 15.0f;
        m_footer_height = 40.0f * scale;
        m_height = std::min(size.y, std::max(140.0f * scale,
            m_dailytips_renderer->preferred_height(size.x) + m_footer_height));
        m_content_height = std::max(1.0f, m_height - m_footer_height);
        return;
    }
    //Orca: Keep the requested content height so expanding a collapsed panel restores its full size.
    m_content_height = size.y - m_footer_height;
    m_height = m_can_expand && !m_is_expanded ? m_footer_height : size.y;
}

void DailyTipsPanel::set_can_expand(bool can_expand)
{
    m_can_expand = can_expand;
}

ImVec2 DailyTipsPanel::get_size()
{
    return ImVec2(m_width, m_height);
}

void DailyTipsPanel::render()
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float scale = imgui.get_font_size() / 15.0f;

    if (!m_first_enter) {
        retrieve_data_from_hint_database(HintDataNavigation::Curr);
        m_first_enter = true;
    }

    push_styles();
    if (m_can_expand) {
        if (m_is_expanded) {
            m_height = m_content_height + m_footer_height;
        }
        else {
            m_height = m_footer_height;
        }
    }
    ImGui::SetNextWindowPos(m_pos);
    ImGui::SetNextWindowSizeConstraints(ImVec2(m_width, m_height), ImVec2(m_width, m_height));
    int window_flags = ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild((std::string("##DailyTipsPanel") + std::to_string(m_uid)).c_str(), ImVec2(m_width, m_height), false, window_flags)) {
        if (m_can_expand) {
            if (m_is_expanded) {
                m_dailytips_renderer->render({ m_pos.x, m_pos.y }, { m_width, m_content_height });
                render_controller_buttons({ m_pos.x, m_pos.y + m_height - m_footer_height }, { m_width, m_footer_height });
            }
            else {
                render_controller_buttons({ m_pos.x, m_pos.y + m_height - m_footer_height }, { m_width, m_footer_height });
            }
        }
        else {
            m_dailytips_renderer->render({ m_pos.x, m_pos.y }, { m_width, m_content_height });
            render_controller_buttons({ m_pos.x, m_pos.y + m_height - m_footer_height }, { m_width, m_footer_height });
        }

        //{// for debug
        //    ImVec2 vMin = ImGui::GetWindowContentRegionMin();
        //    ImVec2 vMax = ImGui::GetWindowContentRegionMax();
        //    vMin += ImGui::GetWindowPos();
        //    vMax += ImGui::GetWindowPos();
        //    ImGui::GetForegroundDrawList()->AddRect(vMin, vMax, IM_COL32(180, 180, 255, 255));
        //}
    }
    ImGui::EndChild();
    pop_styles();
}

void DailyTipsPanel::retrieve_data_from_hint_database(HintDataNavigation nav)
{
    HintData* hint_data = HintDatabase::get_instance().get_hint(nav);
    if (hint_data != nullptr)
    {
        DailyTipsData data{ hint_data->text,
                            hint_data->documentation_link,
                            hint_data->image_url,
                            hint_data->follow_text,
                            hint_data->hypertext,
                            hint_data->callback
        };
        m_dailytips_renderer->update_data(data);
        wxGetApp().imgui()->set_requires_extra_frame();
    }
}

void DailyTipsPanel::expand(bool expand)
{
    if (!m_can_expand)
        return;
    m_is_expanded = expand;
    wxGetApp().app_config->set_bool("show_hints", expand);
}

void DailyTipsPanel::collapse()
{
    if (!m_can_expand)
        return;
    m_is_expanded = false;
    wxGetApp().app_config->set_bool("show_hints", false);
}

bool DailyTipsPanel::is_expanded()
{
    return m_is_expanded;
}

void DailyTipsPanel::on_change_color_mode(bool is_dark)
{
    m_is_dark = is_dark;
    m_dailytips_renderer->on_change_color_mode(is_dark);
}

void DailyTipsPanel::set_fade_opacity(float opacity)
{
    m_fade_opacity = opacity;
    m_dailytips_renderer->set_fade_opacity(opacity);
}

//void DailyTipsPanel::render_header(const ImVec2& pos, const ImVec2& size)
//{
//    ImGuiWrapper& imgui = *wxGetApp().imgui();
//    ImGuiWindow* parent_window = ImGui::GetCurrentWindow();
//    int window_flags = parent_window->Flags;
//    std::string name = "##DailyTipsPanelHeader" + std::to_string(parent_window->ID);
//    ImGui::SetNextWindowPos(pos);
//    if (ImGui::BeginChild(name.c_str(), size, false, window_flags)) {
//        ImVec2 text_pos = pos + ImVec2(0, (size.y - ImGui::CalcTextSize("A").y) / 2);
//        ImGui::SetCursorScreenPos(text_pos);
//        imgui.push_bold_font();
//        imgui.text(_u8L("Daily Tips"));
//        imgui.pop_bold_font();
//    }
//    ImGui::EndChild();
//}

void DailyTipsPanel::render_controller_buttons(const ImVec2& pos, const ImVec2& size)
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float scale = imgui.get_font_size() / 15.0f;
    ImGuiWindow* parent_window = ImGui::GetCurrentWindow();
    int window_flags = parent_window->Flags;
    std::string name = "##DailyTipsPanelControllers" + std::to_string(parent_window->ID);
    ImGui::SetNextWindowPos(pos);
    if (ImGui::BeginChild(name.c_str(), size, false, window_flags)) {
        if (!m_can_expand) {
            ImGui::GetWindowDrawList()->AddLine(pos, pos + ImVec2(size.x, 0),
                ImColor(tip_color(DesignSystem::ColorRole::Line, m_is_dark)));
            m_pages_count = HintDatabase::get_instance().get_count();
            m_page_index = m_pages_count > 0 ? HintDatabase::get_instance().get_index() + 1 : 0;
            const std::string count = std::to_string(m_page_index) + " / " + std::to_string(m_pages_count);
            ImGui::SetCursorScreenPos(pos + ImVec2(0, (size.y - ImGui::GetFontSize()) / 2));
            ImGui::PushStyleColor(ImGuiCol_Text, tip_color(DesignSystem::ColorRole::InkMuted, m_is_dark));
            imgui.text(count);
            ImGui::PopStyleColor();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 8) * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f * scale);
            ImGui::PushStyleColor(ImGuiCol_Button, tip_color(DesignSystem::ColorRole::Field, m_is_dark));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tip_color(DesignSystem::ColorRole::SurfaceHover, m_is_dark));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, tip_color(DesignSystem::ColorRole::AccentSoft, m_is_dark));
            ImGui::PushStyleColor(ImGuiCol_Border, tip_color(DesignSystem::ColorRole::LineControl, m_is_dark));
            const float button = ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(pos + ImVec2(size.x - 2 * button - 8 * scale, (size.y - button) / 2));
            if (imgui.button(wxString(ImGui::PrevArrowBtnIcon), ImVec2(button, button), m_pages_count > 1))
                retrieve_data_from_hint_database(HintDataNavigation::Prev);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Previous tip").c_str());
            ImGui::SameLine(0, 8 * scale);
            if (imgui.button(wxString(ImGui::NextArrowBtnIcon), ImVec2(button, button), m_pages_count > 1))
                retrieve_data_from_hint_database(HintDataNavigation::Next);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Next tip").c_str());
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(3);
            ImGui::EndChild();
            return;
        }
        ImVec2 button_size = ImVec2(38.0f, 38.0f) * scale;
        float button_margin_x = 8.0f * scale;
        std::wstring button_text;

        // collapse / expand
        ImVec2 btn_pos = pos + ImVec2(0, (size.y - ImGui::CalcTextSize("A").y) / 2);
        ImGui::SetCursorScreenPos(btn_pos);
        if (m_can_expand) {
            if (m_is_expanded) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_Text, tip_color(DesignSystem::ColorRole::InkMuted, m_is_dark, m_fade_opacity));

                button_text = ImGui::CollapseArrowIcon;
                imgui.button((_L("Collapse") + button_text));
                ImVec2 collapse_btn_size = ImGui::CalcTextSize((_u8L("Collapse")).c_str());
                collapse_btn_size.x += button_size.x / 2.0f;
                if (ImGui::IsMouseHoveringRect(btn_pos, btn_pos + collapse_btn_size, true))
                {
                    //underline
                    ImVec2 lineEnd = ImGui::GetItemRectMax();
                    lineEnd.x -= ImGui::CalcTextSize("A").x / 2;
                    lineEnd.y -= 2;
                    ImVec2 lineStart = lineEnd;
                    lineStart.x = ImGui::GetItemRectMin().x;
                    ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, ImColor(tip_color(DesignSystem::ColorRole::InkMuted, m_is_dark, m_fade_opacity)));

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        collapse();
                }

                ImGui::PopStyleColor(4);
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));
                ImGui::PushStyleColor(ImGuiCol_Text, tip_color(DesignSystem::ColorRole::Ink, m_is_dark, m_fade_opacity));

                // for bold font text, split text and icon-font button
                imgui.push_bold_font();
                imgui.button((_L("Daily Tips")));
                imgui.pop_bold_font();
                ImVec2 expand_btn_size = ImGui::CalcTextSize((_u8L("Daily Tips")).c_str());
                ImGui::SetCursorScreenPos(ImVec2(btn_pos.x + expand_btn_size.x + ImGui::CalcTextSize(" ").x, btn_pos.y));
                button_text = ImGui::ExpandArrowIcon;
                imgui.button(button_text.c_str());
                expand_btn_size.x += 19.0f * scale;
                if (ImGui::IsMouseHoveringRect(btn_pos, btn_pos + expand_btn_size, true))
                {
                    //underline
                    ImVec2 lineEnd = ImGui::GetItemRectMax();
                    lineEnd.x -= ImGui::CalcTextSize("A").x / 2;
                    lineEnd.y -= 2;
                    ImVec2 lineStart = lineEnd;
                    lineStart.x = ImGui::GetItemRectMin().x - expand_btn_size.x;
                    ImGui::GetWindowDrawList()->AddLine(lineStart, lineEnd, ImColor(tip_color(DesignSystem::ColorRole::Ink, m_is_dark, m_fade_opacity)));

                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        expand();
                }
                
                ImGui::PopStyleColor(4);

                ImGui::EndChild();
                return;
            }
        }

        // page index
        m_page_index = HintDatabase::get_instance().get_index() + 1;
        m_pages_count = HintDatabase::get_instance().get_count();
        std::string text_str = std::to_string(m_page_index) + "/" + std::to_string(m_pages_count);
        float text_item_width = ImGui::CalcTextSize(text_str.c_str()).x;
        ImGui::PushItemWidth(text_item_width);
        float text_pos_x = (pos + size).x - button_margin_x * 2 - button_size.x * 2 - text_item_width;
        float text_pos_y = pos.y + (size.y - ImGui::CalcTextSize("A").y) / 2;
        ImGui::SetCursorScreenPos(ImVec2(text_pos_x, text_pos_y));
        ImGui::PushStyleColor(ImGuiCol_Text, tip_color(DesignSystem::ColorRole::Ink, m_is_dark, m_fade_opacity));
        imgui.text(text_str);
        ImGui::PopStyleColor();
        ImGui::PopItemWidth();
        
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.0f, .0f, .0f, .0f));

        // prev button
        ImColor button_text_color = ImColor(tip_color(DesignSystem::ColorRole::InkMuted, m_is_dark, m_fade_opacity));
        ImVec2 prev_button_pos = pos + size + ImVec2(-button_margin_x - button_size.x * 2, -size.y + (size.y - button_size.y) / 2);
        ImGui::SetCursorScreenPos(prev_button_pos);
        button_text = ImGui::PrevArrowBtnIcon;
        if (ImGui::IsMouseHoveringRect(prev_button_pos, prev_button_pos + button_size, true))
        {
            button_text_color = ImColor(tip_color(DesignSystem::ColorRole::Accent, m_is_dark, m_fade_opacity));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                retrieve_data_from_hint_database(HintDataNavigation::Prev);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, button_text_color.Value);// for icon-font button
        imgui.button(button_text.c_str());
        ImGui::PopStyleColor();

        // next button
        button_text_color = ImColor(tip_color(DesignSystem::ColorRole::InkMuted, m_is_dark, m_fade_opacity));
        ImVec2 next_button_pos = pos + size + ImVec2(-button_size.x, -size.y + (size.y - button_size.y) / 2);
        ImGui::SetCursorScreenPos(next_button_pos);
        button_text = ImGui::NextArrowBtnIcon;
        if (ImGui::IsMouseHoveringRect(next_button_pos, next_button_pos + button_size, true))
        {
            button_text_color = ImColor(tip_color(DesignSystem::ColorRole::Accent, m_is_dark, m_fade_opacity));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                retrieve_data_from_hint_database(HintDataNavigation::Next);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, button_text_color.Value);// for icon-font button
        imgui.button(button_text.c_str());
        ImGui::PopStyleColor();

        ImGui::PopStyleColor(4);
    }
    ImGui::EndChild();
}

void DailyTipsPanel::push_styles()
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float scale = imgui.get_font_size() / 15.0f;
    imgui.push_common_window_style(scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // framePadding cannot be zero. Otherwise, there is a problem with icon font button display
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * scale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tip_color(DesignSystem::ColorRole::SurfaceRaised, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_Text, tip_color(DesignSystem::ColorRole::Ink, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_Border, tip_color(DesignSystem::ColorRole::LineControl, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, tip_color(DesignSystem::ColorRole::SurfaceRaised, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, tip_color(DesignSystem::ColorRole::InkDisabled, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, tip_color(DesignSystem::ColorRole::InkMuted, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, tip_color(DesignSystem::ColorRole::InkMuted, m_is_dark));
}

void DailyTipsPanel::pop_styles()
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    imgui.pop_common_window_style();
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(7);
}


DailyTipsWindow::DailyTipsWindow()
{
    m_panel = new DailyTipsPanel(false, DailyTipsLayout::Vertical);
    on_change_color_mode(wxGetApp().dark_mode());
}

void DailyTipsWindow::open()
{
    m_show = true;
    m_panel->retrieve_data_from_hint_database(HintDataNavigation::Curr);
}

void DailyTipsWindow::close()
{
    m_show = false;
}

void DailyTipsWindow::render()
{
    if (!m_show)
        return;
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const float scale = imgui.get_font_size() / 15.0f;
    const Size& canvas = wxGetApp().plater()->get_current_canvas3D()->get_canvas_size();
    const ImVec2 padding = ImVec2(24, 24) * scale;
    const float margin = 16.0f * scale;
    const ImVec2 content(std::max(1.0f, std::min(420.0f * scale, canvas.get_width() - 2 * (margin + padding.x))),
                         std::max(1.0f, canvas.get_height() - 2 * (margin + padding.y) - 32.0f * scale));
    m_panel->set_size(content);
    const ImVec2 panel_size = m_panel->get_size();

    imgui.push_menu_style(scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6) * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8) * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2(0, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, tip_color(DesignSystem::ColorRole::SurfaceRaised, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, tip_color(DesignSystem::ColorRole::SurfaceRaised, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, tip_color(DesignSystem::ColorRole::SurfaceRaised, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tip_color(DesignSystem::ColorRole::SurfaceRaised, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tip_color(DesignSystem::ColorRole::SurfaceRaised, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_Text, tip_color(DesignSystem::ColorRole::Ink, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_Border, tip_color(DesignSystem::ColorRole::LineControl, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_Button, tip_color(DesignSystem::ColorRole::Field, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tip_color(DesignSystem::ColorRole::SurfaceHover, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, tip_color(DesignSystem::ColorRole::AccentSoft, m_is_dark));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, tip_color(DesignSystem::ColorRole::Focus, m_is_dark));
    imgui.push_bold_font();
    const float title_height = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(canvas.get_width(), canvas.get_height()) * 0.5f,
                           ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(panel_size + padding * 2 + ImVec2(0, title_height));
    const int flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const bool visible = ImGui::Begin(_u8L("Daily Tips").c_str(), &m_show, flags);
    imgui.pop_bold_font();
    if (visible) {
        m_panel->set_position(ImGui::GetWindowPos() + ImGui::GetWindowContentRegionMin());
        m_panel->render();
        if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)))
            m_show = false;
    }
    ImGui::End();
    ImGui::PopStyleColor(11);
    ImGui::PopStyleVar(7);
    imgui.pop_menu_style();
}

void DailyTipsWindow::on_change_color_mode(bool is_dark)
{
    m_is_dark = is_dark;
    m_panel->on_change_color_mode(is_dark);
}

}}
