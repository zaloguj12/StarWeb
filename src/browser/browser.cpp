#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <future>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#include "../common/url_parser.hpp"
#include "../common/stwp_msg.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

// Structural definitions for DOM and CSS
struct CssStyle {
    ImVec4 color = ImVec4(1, 1, 1, 1);
    ImVec4 bg_color = ImVec4(0, 0, 0, 0);
    bool has_bg = false;
    bool has_color = false;
    float border_radius = 0.0f;
    ImVec4 gradient_start = ImVec4(0, 0, 0, 0);
    ImVec4 gradient_end = ImVec4(0, 0, 0, 0);
    bool has_gradient = false;
    std::string text_align = "left";
    ImVec2 padding = ImVec2(0, 0);
};

struct DomNode {
    std::string tag;
    std::string class_name;
    std::string onclick;
    std::string href;
    std::string text_content;
    std::vector<DomNode> children;
};

struct FetchResult {
    bool success = false;
    int status_code = 0;
    std::string status_text;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string error_message;
};

// Global State
std::string current_url = "moon://localhost/";
char url_input[512] = "moon://localhost/";
std::string status_text = "Idle";
bool is_fetching = false;

std::mutex fetch_mutex;
FetchResult active_page;
bool new_page_ready = false;
int active_socket_fd = -1;

DomNode page_dom;
std::unordered_map<std::string, CssStyle> css_classes;
std::string alert_text = "";
bool show_alert = false;

// Parser helper functions
inline std::string trim_spaces(std::string_view str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    return std::string(str.substr(start, end - start + 1));
}

ImVec4 parse_color(std::string_view str) {
    std::string s = trim_spaces(str);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    
    if (s == "white") return ImVec4(1, 1, 1, 1);
    if (s == "black") return ImVec4(0, 0, 0, 1);
    if (s == "red") return ImVec4(1, 0, 0, 1);
    if (s == "green") return ImVec4(0, 1, 0, 1);
    if (s == "blue") return ImVec4(0, 0, 1, 1);
    
    if (s.rfind("rgba", 0) == 0) {
        try {
            auto start = s.find('(');
            auto end = s.find(')');
            if (start != std::string::npos && end != std::string::npos) {
                std::string parts = s.substr(start + 1, end - start - 1);
                std::stringstream ss(parts);
                std::string r_s, g_s, b_s, a_s;
                std::getline(ss, r_s, ',');
                std::getline(ss, g_s, ',');
                std::getline(ss, b_s, ',');
                std::getline(ss, a_s, ',');
                return ImVec4(
                    std::stof(r_s) / 255.0f,
                    std::stof(g_s) / 255.0f,
                    std::stof(b_s) / 255.0f,
                    std::stof(a_s)
                );
            }
        } catch(...) {}
    }
    if (s.rfind("rgb", 0) == 0) {
        try {
            auto start = s.find('(');
            auto end = s.find(')');
            if (start != std::string::npos && end != std::string::npos) {
                std::string parts = s.substr(start + 1, end - start - 1);
                std::stringstream ss(parts);
                std::string r_s, g_s, b_s;
                std::getline(ss, r_s, ',');
                std::getline(ss, g_s, ',');
                std::getline(ss, b_s, ',');
                return ImVec4(
                    std::stof(r_s) / 255.0f,
                    std::stof(g_s) / 255.0f,
                    std::stof(b_s) / 255.0f,
                    1.0f
                );
            }
        } catch(...) {}
    }
    if (!s.empty() && s[0] == '#') {
        std::string hex = s.substr(1);
        if (hex.length() == 3) {
            std::string expanded = "";
            expanded += hex[0]; expanded += hex[0];
            expanded += hex[1]; expanded += hex[1];
            expanded += hex[2]; expanded += hex[2];
            hex = expanded;
        }
        if (hex.length() == 6) {
            try {
                unsigned int r = std::stoul(hex.substr(0, 2), nullptr, 16);
                unsigned int g = std::stoul(hex.substr(2, 2), nullptr, 16);
                unsigned int b = std::stoul(hex.substr(4, 2), nullptr, 16);
                return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
            } catch (...) {}
        }
    }
    return ImVec4(1, 1, 1, 1);
}

void parse_background(std::string_view value, CssStyle& style) {
    std::string val_str = trim_spaces(value);
    if (val_str.find("linear-gradient") != std::string::npos) {
        style.has_gradient = true;
        size_t first_hash = val_str.find('#');
        if (first_hash != std::string::npos) {
            size_t second_hash = val_str.find('#', first_hash + 1);
            if (second_hash != std::string::npos) {
                style.gradient_start = parse_color(val_str.substr(first_hash, 7));
                style.gradient_end = parse_color(val_str.substr(second_hash, 7));
            }
        }
    } else {
        style.has_bg = true;
        style.bg_color = parse_color(val_str);
    }
}

void parse_css(const std::string& css_content, std::unordered_map<std::string, CssStyle>& styles) {
    size_t i = 0;
    size_t len = css_content.size();
    while (i < len) {
        size_t open_brace = css_content.find('{', i);
        if (open_brace == std::string::npos) break;
        
        std::string selector = trim_spaces(css_content.substr(i, open_brace - i));
        size_t close_brace = css_content.find('}', open_brace);
        if (close_brace == std::string::npos) break;
        
        std::string properties = css_content.substr(open_brace + 1, close_brace - open_brace - 1);
        i = close_brace + 1;
        
        CssStyle style;
        std::stringstream ss(properties);
        std::string prop;
        while (std::getline(ss, prop, ';')) {
            auto colon = prop.find(':');
            if (colon == std::string::npos) continue;
            std::string name = trim_spaces(prop.substr(0, colon));
            std::string val = trim_spaces(prop.substr(colon + 1));
            
            if (name == "background" || name == "background-color") {
                parse_background(val, style);
            } else if (name == "color") {
                style.has_color = true;
                style.color = parse_color(val);
            } else if (name == "border-radius") {
                try {
                    style.border_radius = std::stof(val);
                } catch(...) {}
            } else if (name == "text-align") {
                style.text_align = val;
            } else if (name == "padding") {
                try {
                    style.padding.x = std::stof(val);
                    style.padding.y = std::stof(val);
                } catch(...) {}
            }
        }
        styles[selector] = style;
    }
}

DomNode parse_html_to_dom(const std::string& html, std::string& css_content) {
    DomNode root;
    root.tag = "root";

    std::vector<DomNode*> node_stack;
    node_stack.push_back(&root);

    size_t i = 0;
    size_t len = html.size();

    while (i < len) {
        if (html[i] == '<') {
            size_t tag_end = html.find('>', i);
            if (tag_end == std::string::npos) {
                node_stack.back()->text_content += html[i];
                i++;
                continue;
            }

            std::string tag_inner = html.substr(i + 1, tag_end - i - 1);
            i = tag_end + 1;

            if (tag_inner.empty()) continue;

            // Check if closing tag
            if (tag_inner[0] == '/') {
                std::string tag_name = trim_spaces(tag_inner.substr(1));
                std::transform(tag_name.begin(), tag_name.end(), tag_name.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                
                if (tag_name == "style" && node_stack.size() > 1 && node_stack.back()->tag == "style") {
                    css_content = node_stack.back()->text_content;
                }

                if (node_stack.size() > 1 && node_stack.back()->tag == tag_name) {
                    node_stack.pop_back();
                }
                continue;
            }

            bool self_closing = false;
            if (tag_inner.back() == '/') {
                self_closing = true;
                tag_inner = tag_inner.substr(0, tag_inner.size() - 1);
            }

            std::stringstream ss(tag_inner);
            std::string tag_name;
            ss >> tag_name;
            std::transform(tag_name.begin(), tag_name.end(), tag_name.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            DomNode child;
            child.tag = tag_name;

            std::string attr;
            while (ss >> attr) {
                auto eq = attr.find('=');
                if (eq == std::string::npos) continue;
                std::string name = attr.substr(0, eq);
                std::string val = attr.substr(eq + 1);
                if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front()) {
                    val = val.substr(1, val.size() - 2);
                }
                if (name == "class") {
                    child.class_name = val;
                } else if (name == "onclick") {
                    child.onclick = val;
                } else if (name == "href") {
                    child.href = val;
                }
            }

            node_stack.back()->children.push_back(std::move(child));
            
            if (!self_closing && tag_name != "hr" && tag_name != "img" && tag_name != "br" && tag_name != "meta" && tag_name != "link") {
                node_stack.push_back(&(node_stack.back()->children.back()));
            }
        } else {
            node_stack.back()->text_content += html[i];
            i++;
        }
    }

    return root;
}

std::string extract_alert_message(const std::string& html) {
    size_t script_start = html.find("<script>");
    if (script_start != std::string::npos) {
        size_t script_end = html.find("</script>", script_start);
        if (script_end != std::string::npos) {
            std::string script_content = html.substr(script_start + 8, script_end - script_start - 8);
            size_t alert_pos = script_content.find("alert(");
            if (alert_pos != std::string::npos) {
                size_t val_start = script_content.find_first_of("\"'", alert_pos);
                if (val_start != std::string::npos) {
                    size_t val_end = script_content.find_first_of("\"'", val_start + 1);
                    if (val_end != std::string::npos) {
                        return script_content.substr(val_start + 1, val_end - val_start - 1);
                    }
                }
            }
        }
    }
    return "Action performed.";
}

void apply_style(CssStyle& dest, const CssStyle& src) {
    if (src.has_color) {
        dest.color = src.color;
        dest.has_color = true;
    }
    if (src.has_bg) {
        dest.bg_color = src.bg_color;
        dest.has_bg = true;
        dest.has_gradient = false;
    }
    if (src.has_gradient) {
        dest.gradient_start = src.gradient_start;
        dest.gradient_end = src.gradient_end;
        dest.has_gradient = true;
        dest.has_bg = false;
    }
    if (src.border_radius > 0.0f) {
        dest.border_radius = src.border_radius;
    }
    if (src.text_align != "left") {
        dest.text_align = src.text_align;
    }
    if (src.padding.x > 0.0f || src.padding.y > 0.0f) {
        dest.padding = src.padding;
    }
}

// Fetch Logic
FetchResult perform_fetch(const std::string& url_str) {
    FetchResult result;
    auto opt_parsed = parse_url(url_str);
    if (!opt_parsed) {
        result.error_message = "Invalid URL format.";
        return result;
    }

    auto parsed = *opt_parsed;
    if (parsed.scheme != "moon") {
        result.error_message = "Only 'moon://' scheme is supported.";
        return result;
    }

    struct addrinfo hints{}, *res_info;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(parsed.port);
    int status = getaddrinfo(parsed.host.c_str(), port_str.c_str(), &hints, &res_info);
    if (status != 0) {
        result.error_message = "Host resolution failed: " + std::string(gai_strerror(status));
        return result;
    }

    int socket_fd = -1;
    struct addrinfo* rp;
    for (rp = res_info; rp != nullptr; rp = rp->ai_next) {
        socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (socket_fd == -1) continue;

        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            // If the URL has changed in the meantime, abort early
            if (url_str != current_url) {
                close(socket_fd);
                freeaddrinfo(res_info);
                result.error_message = "Cancelled";
                return result;
            }
            active_socket_fd = socket_fd;
        }

        struct timeval tv;
        tv.tv_sec = 4;
        tv.tv_usec = 0;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        if (connect(socket_fd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            if (active_socket_fd == socket_fd) {
                active_socket_fd = -1;
            }
        }
        close(socket_fd);
    }

    freeaddrinfo(res_info);

    if (rp == nullptr) {
        result.error_message = "Connection failed to " + parsed.host + ":" + port_str;
        return result;
    }

    StwpRequest req;
    req.method = "GET";
    req.path = parsed.path;
    req.headers["Host"] = parsed.host + (parsed.port == 8090 ? "" : ":" + port_str);
    req.headers["User-Agent"] = "StarBrowser/1.0";
    req.headers["Connection"] = "close";

    std::string serialized_req = req.serialize();
    if (send(socket_fd, serialized_req.data(), serialized_req.size(), 0) < 0) {
        result.error_message = "Failed to send request.";
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            if (active_socket_fd == socket_fd) active_socket_fd = -1;
        }
        close(socket_fd);
        return result;
    }

    std::string raw_response;
    char recv_buf[4096];
    while (true) {
        ssize_t bytes_received = recv(socket_fd, recv_buf, sizeof(recv_buf), 0);
        if (bytes_received < 0) {
            result.error_message = "Socket read failure.";
            break;
        }
        if (bytes_received == 0) {
            break;
        }
        raw_response.append(recv_buf, bytes_received);
    }

    {
        std::lock_guard<std::mutex> lock(fetch_mutex);
        if (active_socket_fd == socket_fd) {
            active_socket_fd = -1;
        }
    }
    close(socket_fd);

    if (result.error_message == "Socket read failure.") {
        return result;
    }

    StwpResponse res_msg;
    size_t bytes_consumed = 0;
    if (!parse_response(raw_response, bytes_consumed, res_msg)) {
        result.error_message = "Failed to parse STWP response.";
        return result;
    }

    result.success = true;
    result.status_code = res_msg.status_code;
    result.status_text = res_msg.status_text;
    result.headers = res_msg.headers;
    result.body = res_msg.body;
    return result;
}

void start_async_fetch(const std::string& url_str) {
    // Automatically prepend moon:// if missing
    std::string final_url = url_str;
    if (final_url.find("://") == std::string::npos) {
        final_url = "moon://" + final_url;
    }

    // Cancel active socket to abort current download immediately
    {
        std::lock_guard<std::mutex> lock(fetch_mutex);
        if (active_socket_fd != -1) {
            close(active_socket_fd);
            active_socket_fd = -1;
        }
        new_page_ready = false;
    }

    is_fetching = true;
    status_text = "Fetching " + final_url + "...";
    current_url = final_url;

    std::strncpy(url_input, final_url.c_str(), sizeof(url_input) - 1);
    url_input[sizeof(url_input) - 1] = '\0';

    std::thread([final_url]() {
        FetchResult res = perform_fetch(final_url);
        
        std::lock_guard<std::mutex> lock(fetch_mutex);
        // Only accept if this thread's URL is still the active URL
        if (final_url == current_url) {
            active_page = std::move(res);
            new_page_ready = true;
        }
    }).detach();
}

// Hierarchical rendering logic
void render_node(const DomNode& node, const CssStyle& parent_style) {
    // Filter tags that are headers or layout structural nodes
    if (node.tag == "script" || node.tag == "style" || node.tag == "head" || node.tag == "title" || node.tag == "meta") {
        return;
    }

    CssStyle merged = parent_style;
    auto tag_it = css_classes.find(node.tag);
    if (tag_it != css_classes.end()) {
        apply_style(merged, tag_it->second);
    }
    if (!node.class_name.empty()) {
        auto class_it = css_classes.find("." + node.class_name);
        if (class_it != css_classes.end()) {
            apply_style(merged, class_it->second);
        }
    }

    if (node.tag == "div") {
        if (node.class_name == "card") {
            float card_width = 460.0f;
            float card_height = 250.0f;
            
            ImVec2 win_size = ImGui::GetWindowSize();
            ImVec2 cursor_pos = ImVec2((win_size.x - card_width) * 0.5f, (win_size.y - card_height) * 0.5f - 40.0f);
            if (cursor_pos.x < 10) cursor_pos.x = 10;
            if (cursor_pos.y < 10) cursor_pos.y = 10;
            
            ImGui::SetCursorPos(cursor_pos);
            
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = ImVec2(p_min.x + card_width, p_min.y + card_height);
            
            float rounding = merged.border_radius > 0.0f ? merged.border_radius : 20.0f;
            ImVec4 bg_color = merged.has_bg ? merged.bg_color : ImVec4(1, 1, 1, 0.12f);
            
            // Draw drop shadow
            draw_list->AddRectFilled(ImVec2(p_min.x + 5, p_min.y + 5), ImVec2(p_max.x + 5, p_max.y + 5), IM_COL32(0, 0, 0, 50), rounding);
            // Draw glass card body
            draw_list->AddRectFilled(p_min, p_max, ImGui::ColorConvertFloat4ToU32(bg_color), rounding);
            // Draw white edge line
            draw_list->AddRect(p_min, p_max, IM_COL32(255, 255, 255, 35), rounding, 0, 1.2f);
            
            ImGui::SetCursorPos(ImVec2(cursor_pos.x + 30.0f, cursor_pos.y + 35.0f));
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card_width - 60.0f);
            
            for (const auto& child : node.children) {
                render_node(child, merged);
            }
            
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            return;
        } else {
            for (const auto& child : node.children) {
                render_node(child, merged);
            }
        }
    } else if (node.tag == "h1" || node.tag == "h2" || node.tag == "h3") {
        float font_scale = 1.0f;
        if (node.tag == "h1") font_scale = 1.8f;
        else if (node.tag == "h2") font_scale = 1.4f;
        else if (node.tag == "h3") font_scale = 1.2f;
        
        ImGui::SetWindowFontScale(font_scale);
        std::string cleaned_text = trim_spaces(node.text_content);
        
        if (merged.text_align == "center") {
            float text_width = ImGui::CalcTextSize(cleaned_text.c_str()).x;
            float group_width = 400.0f;
            float offset = (group_width - text_width) * 0.5f;
            if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        }
        
        ImGui::TextColored(merged.has_color ? merged.color : ImVec4(1, 1, 1, 1), "%s", cleaned_text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
    } else if (node.tag == "p") {
        std::string cleaned_text = trim_spaces(node.text_content);
        if (merged.text_align == "center") {
            float text_width = ImGui::CalcTextSize(cleaned_text.c_str()).x;
            float group_width = 400.0f;
            float offset = (group_width - text_width) * 0.5f;
            if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        }
        ImGui::TextWrapped("%s", cleaned_text.c_str());
        ImGui::Spacing();
    } else if (node.tag == "button") {
        std::string cleaned_text = trim_spaces(node.text_content);
        float btn_width = ImGui::CalcTextSize(cleaned_text.c_str()).x + 36.0f;
        if (merged.text_align == "center") {
            float group_width = 400.0f;
            float offset = (group_width - btn_width) * 0.5f;
            if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        }
        
        ImVec4 btn_bg = merged.has_bg ? merged.bg_color : ImVec4(1, 1, 1, 1);
        ImVec4 btn_text = merged.has_color ? merged.color : ImVec4(0.31f, 0.27f, 0.90f, 1.0f);
        
        ImGui::PushStyleColor(ImGuiCol_Button, btn_bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(btn_bg.x * 0.9f, btn_bg.y * 0.9f, btn_bg.z * 0.9f, btn_bg.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(btn_bg.x * 0.8f, btn_bg.y * 0.8f, btn_bg.z * 0.8f, btn_bg.w));
        ImGui::PushStyleColor(ImGuiCol_Text, btn_text);
        
        float rounding = merged.border_radius > 0.0f ? merged.border_radius : 15.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        
        std::string btn_id = cleaned_text + "##" + node.onclick;
        if (ImGui::Button(btn_id.c_str(), ImVec2(btn_width, 0))) {
            show_alert = true;
        }
        
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::Spacing();
    } else if (node.tag == "a") {
        std::string cleaned_text = trim_spaces(node.text_content);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.65f, 1.0f, 1.0f));
        ImGui::Text("%s", cleaned_text.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImVec2 min_pos = ImGui::GetItemRectMin();
            ImVec2 max_pos = ImGui::GetItemRectMax();
            min_pos.y = max_pos.y;
            ImGui::GetWindowDrawList()->AddLine(min_pos, max_pos, IM_COL32(100, 165, 255, 255));
            
            if (ImGui::IsItemClicked()) {
                std::string new_url = node.href;
                if (new_url.find("://") == std::string::npos) {
                    auto opt_curr = parse_url(current_url);
                    if (opt_curr) {
                        if (!new_url.empty() && new_url[0] != '/') {
                            new_url = "/" + new_url;
                        }
                        new_url = opt_curr->scheme + "://" + opt_curr->host + ":" + std::to_string(opt_curr->port) + new_url;
                    }
                }
                start_async_fetch(new_url);
            }
        }
        ImGui::PopStyleColor();
        ImGui::Spacing();
    } else if (node.tag == "hr") {
        ImGui::Separator();
        ImGui::Spacing();
    } else if (node.tag == "li") {
        std::string cleaned_text = trim_spaces(node.text_content);
        ImGui::BulletText("%s", cleaned_text.c_str());
    } else {
        // Render child nodes recursively for unhandled structural tags
        for (const auto& child : node.children) {
            render_node(child, merged);
        }
    }
}

int main() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

#if defined(__APPLE__)
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow* window = glfwCreateWindow(1024, 768, "StarBrowser", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load custom TrueType Font (Arial) for smooth modern typography
    ImFont* font = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Supplemental/Arial.ttf", 16.0f);
    if (font == nullptr) {
        io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();

    auto& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.13f, 0.95f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.10f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.28f, 1.00f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.60f, 0.45f, 0.94f, 0.60f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.60f, 0.45f, 0.94f, 0.80f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.45f, 0.94f, 1.00f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.14f, 0.28f, 0.80f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Initial page load
    start_async_fetch(current_url);

    std::string page_raw_html = "";

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Check for complete downloads
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            if (new_page_ready) {
                is_fetching = false;
                new_page_ready = false;
                if (active_page.success) {
                    status_text = "Success (" + std::to_string(active_page.status_code) + " " + active_page.status_text + ")";
                    page_raw_html = active_page.body;
                } else {
                    status_text = "Error: " + active_page.error_message;
                    page_raw_html = "<h1>Error loading page</h1><p>" + active_page.error_message + "</p>";
                }
                
                // Parse DOM and CSS Styles
                std::string css_content = "";
                page_dom = parse_html_to_dom(page_raw_html, css_content);
                css_classes.clear();
                parse_css(css_content, css_classes);
                alert_text = extract_alert_message(page_raw_html);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y));
        ImGui::Begin("StarBrowserWorkspace", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        
        // Navigation Bar
        ImGui::PushItemWidth(-110.0f);
        if (ImGui::InputText("##url", url_input, IM_ARRAYSIZE(url_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
            start_async_fetch(url_input);
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Go", ImVec2(100, 0))) {
            start_async_fetch(url_input);
        }
        ImGui::Spacing();

        // Content Viewport
        ImGui::BeginChild("RenderViewport", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 5), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        
        // Draw the document body background using ImGui DrawList
        auto body_it = css_classes.find("body");
        if (body_it != css_classes.end()) {
            const auto& body_style = body_it->second;
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 min_p = ImGui::GetWindowPos();
            ImVec2 max_p = ImVec2(min_p.x + ImGui::GetWindowWidth(), min_p.y + ImGui::GetWindowHeight());
            
            if (body_style.has_gradient) {
                ImU32 col_start = ImGui::ColorConvertFloat4ToU32(body_style.gradient_start);
                ImU32 col_end = ImGui::ColorConvertFloat4ToU32(body_style.gradient_end);
                // Draw vertical gradient (using corners)
                draw_list->AddRectFilledMultiColor(min_p, max_p, col_start, col_start, col_end, col_end);
            } else if (body_style.has_bg) {
                draw_list->AddRectFilled(min_p, max_p, ImGui::ColorConvertFloat4ToU32(body_style.bg_color));
            }
        }

        // Render DOM tree
        CssStyle default_style;
        render_node(page_dom, default_style);

        ImGui::EndChild();

        // Bottom Status Bar
        ImGui::Separator();
        if (is_fetching) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Loading...");
        } else if (status_text.find("Success") != std::string::npos) {
            ImGui::TextDisabled("Ready");
        } else if (status_text.find("Error") != std::string::npos) {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%s", status_text.c_str());
        } else {
            ImGui::TextDisabled("Ready");
        }

        // Alert Popup simulation
        if (show_alert) {
            ImGui::OpenPopup("Alert");
        }
        if (ImGui::BeginPopupModal("Alert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", alert_text.c_str());
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                show_alert = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();

        // OpenGL rendering
        ImGui::Render();
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.07f, 0.09f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
