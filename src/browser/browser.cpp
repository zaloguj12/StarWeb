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
    float border_radius = -1.0f;
    ImVec4 gradient_start = ImVec4(0, 0, 0, 0);
    ImVec4 gradient_end = ImVec4(0, 0, 0, 0);
    bool has_gradient = false;
    std::string text_align = "left";
    
    // Spacing and boundaries
    float padding_left = 0.0f;
    float padding_right = 0.0f;
    float padding_top = 0.0f;
    float padding_bottom = 0.0f;
    
    float margin_left = 0.0f;
    float margin_right = 0.0f;
    float margin_top = 0.0f;
    float margin_bottom = 0.0f;
    
    float width = -1.0f;
    float height = -1.0f;
    
    float border_width = 0.0f;
    ImVec4 border_color = ImVec4(0, 0, 0, 0);
    bool has_border_color = false;
    
    float font_size = 1.0f;
    std::string display = "";
};

struct DomNode {
    std::string tag;
    std::string class_name;
    std::string id;
    std::string onclick;
    std::string href;
    std::string text_content;
    std::string type;
    std::string value;
    std::string placeholder;
    std::string inline_style;
    bool has_inline_style = false;
    CssStyle parsed_inline_style;
    std::vector<DomNode> children;
};

struct FetchResult {
    bool success = false;
    int status_code = 0;
    std::string status_text;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string error_message;
    DomNode dom;
    std::unordered_map<std::string, CssStyle> css_classes;
};

// Global State
std::string current_url = "moon://localhost/index.html";
char url_input[512] = "moon://localhost/index.html";
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

inline std::string collapse_whitespace(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    bool last_was_space = false;
    
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    
    for (size_t i = start; i <= end; ++i) {
        char c = str[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!last_was_space) {
                result += ' ';
                last_was_space = true;
            }
        } else {
            result += c;
            last_was_space = false;
        }
    }
    return result;
}

ImVec4 parse_color(std::string_view str) {
    std::string s = trim_spaces(str);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    
    if (s == "white") return ImVec4(1, 1, 1, 1);
    if (s == "black") return ImVec4(0, 0, 0, 1);
    if (s == "red") return ImVec4(1, 0, 0, 1);
    if (s == "green") return ImVec4(0, 1, 0, 1);
    if (s == "blue") return ImVec4(0, 0, 1, 1);
    if (s == "gray" || s == "grey") return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    if (s == "lightgray" || s == "lightgrey") return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    if (s == "darkgray" || s == "darkgrey") return ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    if (s == "yellow") return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    
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

void parse_css_properties(const std::string& properties, CssStyle& style) {
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
                float p = std::stof(val);
                style.padding_left = style.padding_right = style.padding_top = style.padding_bottom = p;
            } catch(...) {}
        } else if (name == "padding-left") {
            try { style.padding_left = std::stof(val); } catch(...) {}
        } else if (name == "padding-right") {
            try { style.padding_right = std::stof(val); } catch(...) {}
        } else if (name == "padding-top") {
            try { style.padding_top = std::stof(val); } catch(...) {}
        } else if (name == "padding-bottom") {
            try { style.padding_bottom = std::stof(val); } catch(...) {}
        } else if (name == "margin") {
            try {
                float m = std::stof(val);
                style.margin_left = style.margin_right = style.margin_top = style.margin_bottom = m;
            } catch(...) {}
        } else if (name == "margin-left") {
            try { style.margin_left = std::stof(val); } catch(...) {}
        } else if (name == "margin-right") {
            try { style.margin_right = std::stof(val); } catch(...) {}
        } else if (name == "margin-top") {
            try { style.margin_top = std::stof(val); } catch(...) {}
        } else if (name == "margin-bottom") {
            try { style.margin_bottom = std::stof(val); } catch(...) {}
        } else if (name == "width") {
            try { style.width = std::stof(val); } catch(...) {}
        } else if (name == "height") {
            try { style.height = std::stof(val); } catch(...) {}
        } else if (name == "border-width") {
            try { style.border_width = std::stof(val); } catch(...) {}
        } else if (name == "border-color") {
            style.border_color = parse_color(val);
            style.has_border_color = true;
        } else if (name == "font-size") {
            try {
                style.font_size = std::stof(val);
            } catch(...) {}
        } else if (name == "display") {
            style.display = val;
        }
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
        parse_css_properties(properties, style);
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

            // Extract tag name
            size_t tag_name_end = 0;
            while (tag_name_end < tag_inner.size() && !std::isspace(tag_inner[tag_name_end])) {
                tag_name_end++;
            }
            std::string tag_name = tag_inner.substr(0, tag_name_end);
            std::transform(tag_name.begin(), tag_name.end(), tag_name.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            DomNode child;
            child.tag = tag_name;

            // Parse attributes
            size_t attr_pos = tag_name_end;
            while (attr_pos < tag_inner.size()) {
                while (attr_pos < tag_inner.size() && std::isspace(tag_inner[attr_pos])) {
                    attr_pos++;
                }
                if (attr_pos >= tag_inner.size()) break;
                
                size_t name_start = attr_pos;
                while (attr_pos < tag_inner.size() && tag_inner[attr_pos] != '=' && !std::isspace(tag_inner[attr_pos])) {
                    attr_pos++;
                }
                std::string attr_name = tag_inner.substr(name_start, attr_pos - name_start);
                
                while (attr_pos < tag_inner.size() && std::isspace(tag_inner[attr_pos])) {
                    attr_pos++;
                }
                
                std::string attr_val = "";
                if (attr_pos < tag_inner.size() && tag_inner[attr_pos] == '=') {
                    attr_pos++;
                    while (attr_pos < tag_inner.size() && std::isspace(tag_inner[attr_pos])) {
                        attr_pos++;
                    }
                    if (attr_pos < tag_inner.size()) {
                        if (tag_inner[attr_pos] == '"' || tag_inner[attr_pos] == '\'') {
                            char quote = tag_inner[attr_pos];
                            attr_pos++;
                            size_t val_start = attr_pos;
                            while (attr_pos < tag_inner.size() && tag_inner[attr_pos] != quote) {
                                attr_pos++;
                            }
                            attr_val = tag_inner.substr(val_start, attr_pos - val_start);
                            if (attr_pos < tag_inner.size()) attr_pos++;
                        } else {
                            size_t val_start = attr_pos;
                            while (attr_pos < tag_inner.size() && !std::isspace(tag_inner[attr_pos])) {
                                attr_pos++;
                            }
                            attr_val = tag_inner.substr(val_start, attr_pos - val_start);
                        }
                    }
                }
                
                if (attr_name == "class") {
                    child.class_name = attr_val;
                } else if (attr_name == "onclick") {
                    child.onclick = attr_val;
                } else if (attr_name == "href") {
                    child.href = attr_val;
                } else if (attr_name == "id") {
                    child.id = attr_val;
                } else if (attr_name == "style") {
                    child.inline_style = attr_val;
                    child.has_inline_style = true;
                    parse_css_properties(attr_val, child.parsed_inline_style);
                } else if (attr_name == "type") {
                    child.type = attr_val;
                } else if (attr_name == "value") {
                    child.value = attr_val;
                } else if (attr_name == "placeholder") {
                    child.placeholder = attr_val;
                }
            }

            node_stack.back()->children.push_back(std::move(child));
            
            if (!self_closing && tag_name != "hr" && tag_name != "img" && tag_name != "br" && tag_name != "meta" && tag_name != "link" && tag_name != "input") {
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
    if (src.border_radius >= 0.0f) {
        dest.border_radius = src.border_radius;
    }
    if (src.text_align != "left") {
        dest.text_align = src.text_align;
    }
    if (src.padding_left > 0.0f) dest.padding_left = src.padding_left;
    if (src.padding_right > 0.0f) dest.padding_right = src.padding_right;
    if (src.padding_top > 0.0f) dest.padding_top = src.padding_top;
    if (src.padding_bottom > 0.0f) dest.padding_bottom = src.padding_bottom;
    
    if (src.margin_left > 0.0f) dest.margin_left = src.margin_left;
    if (src.margin_right > 0.0f) dest.margin_right = src.margin_right;
    if (src.margin_top > 0.0f) dest.margin_top = src.margin_top;
    if (src.margin_bottom > 0.0f) dest.margin_bottom = src.margin_bottom;
    
    if (src.width > -1.0f) dest.width = src.width;
    if (src.height > -1.0f) dest.height = src.height;
    if (src.border_width > 0.0f) dest.border_width = src.border_width;
    if (src.has_border_color) {
        dest.border_color = src.border_color;
        dest.has_border_color = true;
    }
    if (src.font_size != 1.0f) dest.font_size = src.font_size;
    if (!src.display.empty()) dest.display = src.display;
}

// Helpers for URL resolution, stylesheet scanning and title lookup
std::string resolve_url(const std::string& base_url, const std::string& relative_url) {
    if (relative_url.empty()) return base_url;
    if (relative_url.find("://") != std::string::npos) {
        return relative_url;
    }
    
    auto base_opt = parse_url(base_url);
    if (!base_opt) return relative_url;
    
    std::string scheme = base_opt->scheme;
    std::string host = base_opt->host;
    int port = base_opt->port;
    
    std::string path;
    if (relative_url[0] == '/') {
        path = relative_url;
    } else {
        std::string base_path = base_opt->path;
        auto last_slash = base_path.find_last_of('/');
        if (last_slash != std::string::npos) {
            path = base_path.substr(0, last_slash + 1) + relative_url;
        } else {
            path = "/" + relative_url;
        }
    }
    
    return scheme + "://" + host + ":" + std::to_string(port) + path;
}

std::string find_title_in_dom(const DomNode& node) {
    if (node.tag == "title") {
        return node.text_content;
    }
    for (const auto& child : node.children) {
        std::string t = find_title_in_dom(child);
        if (!t.empty()) return t;
    }
    return "";
}

void find_stylesheets_in_dom(const DomNode& node, std::vector<std::string>& hrefs) {
    if (node.tag == "link") {
        if (!node.href.empty()) {
            hrefs.push_back(node.href);
        }
    }
    for (const auto& child : node.children) {
        find_stylesheets_in_dom(child, hrefs);
    }
}

bool is_inline_element(const DomNode& node, const CssStyle& merged) {
    if (merged.display == "inline" || merged.display == "inline-block") return true;
    if (merged.display == "block") return false;
    
    if (node.tag == "span" || node.tag == "a" || node.tag == "button" || 
        node.tag == "input" || node.tag == "textarea" || node.tag == "select" || node.tag == "option") {
        return true;
    }
    return false;
}

// Fetch Logic
FetchResult perform_fetch(const std::string& url_str, bool is_main_resource = true) {
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
            if (is_main_resource && url_str != current_url) {
                close(socket_fd);
                freeaddrinfo(res_info);
                result.error_message = "Cancelled";
                return result;
            }
            if (is_main_resource) {
                active_socket_fd = socket_fd;
            }
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
            if (is_main_resource && active_socket_fd == socket_fd) {
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
            if (is_main_resource && active_socket_fd == socket_fd) active_socket_fd = -1;
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
        if (is_main_resource && active_socket_fd == socket_fd) {
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
        FetchResult res = perform_fetch(final_url, true);
        
        if (res.success) {
            std::string css_content = "";
            res.dom = parse_html_to_dom(res.body, css_content);
            
            // Scan for stylesheet links
            std::vector<std::string> stylesheet_hrefs;
            find_stylesheets_in_dom(res.dom, stylesheet_hrefs);
            
            for (const auto& href : stylesheet_hrefs) {
                std::string sheet_url = resolve_url(final_url, href);
                FetchResult sheet_res = perform_fetch(sheet_url, false);
                if (sheet_res.success) {
                    css_content += "\n" + sheet_res.body;
                }
            }
            
            parse_css(css_content, res.css_classes);
        }
        
        std::lock_guard<std::mutex> lock(fetch_mutex);
        // Only accept if this thread's URL is still the active URL
        if (final_url == current_url) {
            active_page = std::move(res);
            new_page_ready = true;
        }
    }).detach();
}

// Hierarchical rendering logic
// Hierarchical rendering logic
void render_node(DomNode& node, const CssStyle& parent_style, bool& is_inline_flow, int li_index = -1) {
    // Filter tags that are headers or layout structural nodes
    if (node.tag == "script" || node.tag == "style" || node.tag == "head" || node.tag == "title" || node.tag == "meta" || node.tag == "option") {
        return;
    }

    CssStyle merged;
    // Copy only inherited CSS properties
    if (parent_style.has_color) {
        merged.color = parent_style.color;
        merged.has_color = true;
    }
    merged.font_size = parent_style.font_size;
    merged.text_align = parent_style.text_align;
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
    if (node.has_inline_style) {
        apply_style(merged, node.parsed_inline_style);
    }

    // Determine inline-ness
    bool is_inline = is_inline_element(node, merged);
    if (is_inline) {
        if (is_inline_flow) {
            ImGui::SameLine(0, 8.0f + merged.margin_left);
        }
        is_inline_flow = true;
    } else {
        is_inline_flow = false;
    }

    // Setup backgrounds, gradients, borders using ImGui channel splitting
    bool draw_bg = (merged.has_bg || merged.has_gradient || (merged.border_width > 0.0f)) &&
                   (node.tag != "input" && node.tag != "textarea" && node.tag != "select" && node.tag != "button" && node.tag != "a");
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImDrawListSplitter splitter;
    ImVec2 start_pos = ImGui::GetCursorScreenPos();
    ImVec2 content_start = start_pos;

    // Font size scaling
    float base_font_scale = merged.font_size;
    if (node.tag == "h1") base_font_scale *= 1.8f;
    else if (node.tag == "h2") base_font_scale *= 1.4f;
    else if (node.tag == "h3") base_font_scale *= 1.2f;
    else if (node.tag == "h4") base_font_scale *= 1.1f;
    else if (node.tag == "h5") base_font_scale *= 1.0f;
    else if (node.tag == "h6") base_font_scale *= 0.9f;

    if (base_font_scale != 1.0f) {
        ImGui::SetWindowFontScale(base_font_scale);
    }

    if (draw_bg) {
        // Adjust for margins
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.margin_top);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.margin_left);
        
        content_start = ImGui::GetCursorScreenPos();
        
        // Adjust Y for padding top, X for padding left
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.padding_top);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.padding_left);
        
        splitter.Split(draw_list, 2);
        splitter.SetCurrentChannel(draw_list, 1); // Content is drawn in channel 1
    } else {
        // Apply margins and padding even if no background
        if (merged.margin_top > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.margin_top);
        if (merged.margin_left > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.margin_left);
        if (merged.padding_top > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.padding_top);
        if (merged.padding_left > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + merged.padding_left);
    }

    ImGui::BeginGroup();

    // Render tag specific behaviors
    if (node.tag == "div") {
        bool child_inline_flow = false;
        for (auto& child : node.children) {
            render_node(child, merged, child_inline_flow);
        }
    } else if (node.tag == "ol") {
        int index = 1;
        bool child_inline_flow = false;
        for (auto& child : node.children) {
            if (child.tag == "li") {
                render_node(child, merged, child_inline_flow, index++);
            } else {
                render_node(child, merged, child_inline_flow);
            }
        }
    } else if (node.tag == "ul") {
        bool child_inline_flow = false;
        for (auto& child : node.children) {
            render_node(child, merged, child_inline_flow);
        }
    } else if (node.tag == "li") {
        std::string cleaned_text = collapse_whitespace(node.text_content);
        if (li_index >= 0) {
            ImGui::TextColored(merged.color, "%d. %s", li_index, cleaned_text.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, merged.color);
            ImGui::BulletText("%s", cleaned_text.c_str());
            ImGui::PopStyleColor();
        }
    } else if (node.tag == "h1" || node.tag == "h2" || node.tag == "h3" || node.tag == "h4" || node.tag == "h5" || node.tag == "h6" || node.tag == "p" || node.tag == "span") {
        std::string cleaned_text = collapse_whitespace(node.text_content);
        if (!cleaned_text.empty()) {
            if (merged.text_align == "center") {
                float text_width = ImGui::CalcTextSize(cleaned_text.c_str()).x;
                float avail_width = merged.width > 0.0f ? merged.width : ImGui::GetContentRegionAvail().x;
                float offset = (avail_width - text_width) * 0.5f;
                if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
            } else if (merged.text_align == "right") {
                float text_width = ImGui::CalcTextSize(cleaned_text.c_str()).x;
                float avail_width = merged.width > 0.0f ? merged.width : ImGui::GetContentRegionAvail().x;
                float offset = avail_width - text_width;
                if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
            }
            
            float wrap_width = merged.width > 0.0f ? merged.width : ImGui::GetContentRegionAvail().x;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
            
            if (node.tag == "span") {
                ImGui::TextColored(merged.color, "%s", cleaned_text.c_str());
            } else {
                ImGui::TextColored(merged.color, "%s", cleaned_text.c_str());
                ImGui::Spacing();
            }
            
            ImGui::PopTextWrapPos();
        }
        
        // Also render inline children of headings/p if any
        bool child_inline_flow = true;
        for (auto& child : node.children) {
            render_node(child, merged, child_inline_flow);
        }
    } else if (node.tag == "button") {
        std::string cleaned_text = collapse_whitespace(node.text_content);
        float btn_width = merged.width > 0.0f ? merged.width : (ImGui::CalcTextSize(cleaned_text.c_str()).x + 36.0f);
        float btn_height = merged.height > 0.0f ? merged.height : 0.0f;
        
        ImVec4 btn_bg = merged.has_bg ? merged.bg_color : ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
        ImVec4 btn_text = merged.has_color ? merged.color : ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
        
        ImGui::PushStyleColor(ImGuiCol_Button, btn_bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(btn_bg.x * 0.95f, btn_bg.y * 0.95f, btn_bg.z * 0.95f, btn_bg.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(btn_bg.x * 0.9f, btn_bg.y * 0.9f, btn_bg.z * 0.9f, btn_bg.w));
        ImGui::PushStyleColor(ImGuiCol_Text, btn_text);
        
        float rounding = merged.border_radius >= 0.0f ? merged.border_radius : 0.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, merged.border_width >= 0.0f ? merged.border_width : 0.0f);
        
        ImGui::PushStyleColor(ImGuiCol_Border, merged.has_border_color ? merged.border_color : ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        
        std::string btn_id = cleaned_text + "##" + (node.id.empty() ? std::to_string((uintptr_t)&node) : node.id);
        if (ImGui::Button(btn_id.c_str(), ImVec2(btn_width, btn_height))) {
            if (!node.onclick.empty()) {
                alert_text = extract_alert_message(node.onclick);
                show_alert = true;
            } else {
                alert_text = "Button clicked.";
                show_alert = true;
            }
        }
        
        ImGui::PopStyleColor(); // Pop border color
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
    } else if (node.tag == "a") {
        std::string cleaned_text = collapse_whitespace(node.text_content);
        ImVec4 link_color = merged.has_color ? merged.color : ImVec4(0.1f, 0.3f, 0.85f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, link_color);
        ImGui::Text("%s", cleaned_text.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImVec2 min_pos = ImGui::GetItemRectMin();
            ImVec2 max_pos = ImGui::GetItemRectMax();
            min_pos.y = max_pos.y;
            ImGui::GetWindowDrawList()->AddLine(min_pos, max_pos, ImGui::ColorConvertFloat4ToU32(link_color));
            
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
    } else if (node.tag == "hr") {
        ImGui::Separator();
        ImGui::Spacing();
    } else if (node.tag == "input") {
        std::string type = node.type;
        if (type.empty() || type == "text" || type == "password") {
            char buf[1024] = {0};
            std::strncpy(buf, node.value.c_str(), sizeof(buf) - 1);
            
            float width = merged.width > 0.0f ? merged.width : 200.0f;
            ImGui::PushItemWidth(width);
            
            ImGuiInputTextFlags flags = 0;
            if (node.type == "password") {
                flags |= ImGuiInputTextFlags_Password;
            }
            
            std::string input_label = "##" + (node.id.empty() ? std::to_string((uintptr_t)&node) : node.id);
            
            float rounding = merged.border_radius >= 0.0f ? merged.border_radius : 0.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, merged.border_width >= 0.0f ? merged.border_width : 1.0f);
            
            ImGui::PushStyleColor(ImGuiCol_FrameBg, merged.has_bg ? merged.bg_color : ImVec4(1,1,1,1));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, merged.has_bg ? ImVec4(merged.bg_color.x*0.95f, merged.bg_color.y*0.95f, merged.bg_color.z*0.95f, merged.bg_color.w) : ImVec4(0.95f,0.95f,0.95f,1));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, merged.has_bg ? ImVec4(merged.bg_color.x*0.9f, merged.bg_color.y*0.9f, merged.bg_color.z*0.9f, merged.bg_color.w) : ImVec4(0.9f,0.9f,0.9f,1));
            ImGui::PushStyleColor(ImGuiCol_Text, merged.has_color ? merged.color : ImVec4(0,0,0,1));
            ImGui::PushStyleColor(ImGuiCol_Border, merged.has_border_color ? merged.border_color : ImVec4(0.7f,0.7f,0.7f,1));
            ImGui::PushStyleColor(ImGuiCol_InputTextCursor, merged.has_color ? merged.color : ImVec4(0,0,0,1));
            
            if (ImGui::InputTextWithHint(input_label.c_str(), node.placeholder.c_str(), buf, sizeof(buf), flags)) {
                node.value = buf;
            }
            
            ImGui::PopStyleColor(6);
            ImGui::PopStyleVar(2);
            ImGui::PopItemWidth();
        }
    } else if (node.tag == "textarea") {
        char buf[4096] = {0};
        std::strncpy(buf, node.value.c_str(), sizeof(buf) - 1);
        
        float width = merged.width > 0.0f ? merged.width : 300.0f;
        float height = merged.height > 0.0f ? merged.height : 100.0f;
        
        std::string label = "##" + (node.id.empty() ? std::to_string((uintptr_t)&node) : node.id);
        
        float rounding = merged.border_radius >= 0.0f ? merged.border_radius : 0.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, merged.border_width >= 0.0f ? merged.border_width : 1.0f);
        
        ImGui::PushStyleColor(ImGuiCol_FrameBg, merged.has_bg ? merged.bg_color : ImVec4(1,1,1,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, merged.has_bg ? ImVec4(merged.bg_color.x*0.95f, merged.bg_color.y*0.95f, merged.bg_color.z*0.95f, merged.bg_color.w) : ImVec4(0.95f,0.95f,0.95f,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, merged.has_bg ? ImVec4(merged.bg_color.x*0.9f, merged.bg_color.y*0.9f, merged.bg_color.z*0.9f, merged.bg_color.w) : ImVec4(0.9f,0.9f,0.9f,1));
        ImGui::PushStyleColor(ImGuiCol_Text, merged.has_color ? merged.color : ImVec4(0,0,0,1));
        ImGui::PushStyleColor(ImGuiCol_Border, merged.has_border_color ? merged.border_color : ImVec4(0.7f,0.7f,0.7f,1));
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, merged.has_color ? merged.color : ImVec4(0,0,0,1));
        
        if (ImGui::InputTextMultiline(label.c_str(), buf, sizeof(buf), ImVec2(width, height))) {
            node.value = buf;
        }
        
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(2);
    } else if (node.tag == "select") {
        std::vector<std::string> options;
        std::vector<std::string> option_vals;
        int current_item = -1;
        
        for (size_t idx = 0; idx < node.children.size(); idx++) {
            if (node.children[idx].tag == "option") {
                std::string opt_text = trim_spaces(node.children[idx].text_content);
                std::string opt_val = node.children[idx].value.empty() ? opt_text : node.children[idx].value;
                options.push_back(opt_text);
                option_vals.push_back(opt_val);
                
                if (node.value == opt_val) {
                    current_item = (int)idx;
                }
            }
        }
        
        if (current_item == -1 && !option_vals.empty()) {
            current_item = 0;
            node.value = option_vals[0];
        }
        
        std::string combo_label = "##" + (node.id.empty() ? std::to_string((uintptr_t)&node) : node.id);
        
        std::vector<const char*> items;
        for (const auto& opt : options) {
            items.push_back(opt.c_str());
        }
        
        float width = merged.width > 0.0f ? merged.width : 150.0f;
        ImGui::PushItemWidth(width);
        
        float rounding = merged.border_radius >= 0.0f ? merged.border_radius : 0.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, merged.border_width >= 0.0f ? merged.border_width : 1.0f);
        
        ImGui::PushStyleColor(ImGuiCol_FrameBg, merged.has_bg ? merged.bg_color : ImVec4(1,1,1,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, merged.has_bg ? ImVec4(merged.bg_color.x*0.95f, merged.bg_color.y*0.95f, merged.bg_color.z*0.95f, merged.bg_color.w) : ImVec4(0.95f,0.95f,0.95f,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, merged.has_bg ? ImVec4(merged.bg_color.x*0.9f, merged.bg_color.y*0.9f, merged.bg_color.z*0.9f, merged.bg_color.w) : ImVec4(0.9f,0.9f,0.9f,1));
        ImGui::PushStyleColor(ImGuiCol_Text, merged.has_color ? merged.color : ImVec4(0,0,0,1));
        ImGui::PushStyleColor(ImGuiCol_Border, merged.has_border_color ? merged.border_color : ImVec4(0.7f,0.7f,0.7f,1));
        
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        
        if (!items.empty()) {
            if (ImGui::Combo(combo_label.c_str(), &current_item, items.data(), items.size())) {
                if (current_item >= 0 && current_item < (int)option_vals.size()) {
                    node.value = option_vals[current_item];
                }
            }
        }
        
        ImGui::PopStyleColor(8);
        ImGui::PopStyleVar(2);
        ImGui::PopItemWidth();
    } else {
        // Fallback recursive render
        bool child_inline_flow = false;
        for (auto& child : node.children) {
            render_node(child, merged, child_inline_flow);
        }
    }

    ImGui::EndGroup();

    // End layout wrapping and draw background in bottom layer channel
    if (draw_bg) {
        ImVec2 min_p = content_start;
        ImVec2 max_p = ImGui::GetItemRectMax();
        
        // Add padding bottom and right
        max_p.x += merged.padding_right;
        max_p.y += merged.padding_bottom;
        
        // Sizing override
        if (merged.width > 0.0f) max_p.x = min_p.x + merged.width;
        if (merged.height > 0.0f) max_p.y = min_p.y + merged.height;
        
        splitter.SetCurrentChannel(draw_list, 0); // Select background channel
        
        float rounding = merged.border_radius;
        if (merged.has_gradient) {
            ImU32 col_start = ImGui::ColorConvertFloat4ToU32(merged.gradient_start);
            ImU32 col_end = ImGui::ColorConvertFloat4ToU32(merged.gradient_end);
            draw_list->AddRectFilledMultiColor(min_p, max_p, col_start, col_start, col_end, col_end);
        } else if (merged.has_bg) {
            draw_list->AddRectFilled(min_p, max_p, ImGui::ColorConvertFloat4ToU32(merged.bg_color), rounding);
        }
        
        if (merged.border_width > 0.0f && merged.has_border_color) {
            draw_list->AddRect(min_p, max_p, ImGui::ColorConvertFloat4ToU32(merged.border_color), rounding, 0, merged.border_width);
        }
        
        splitter.Merge(draw_list);
        
        // Position cursor after this block, including margins
        ImGui::SetCursorScreenPos(ImVec2(start_pos.x, max_p.y + merged.margin_bottom));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    } else {
        // Adjust for margins and paddings on exit
        if (merged.padding_bottom > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.padding_bottom);
        if (merged.margin_bottom > 0.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + merged.margin_bottom);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    if (base_font_scale != 1.0f) {
        ImGui::SetWindowFontScale(1.0f);
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
    style.Colors[ImGuiCol_InputTextCursor] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

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
                    page_dom = std::move(active_page.dom);
                    css_classes = std::move(active_page.css_classes);
                    
                    // Update window title
                    std::string title = find_title_in_dom(page_dom);
                    if (!title.empty()) {
                        glfwSetWindowTitle(window, ("StarBrowser - " + trim_spaces(title)).c_str());
                    } else {
                        glfwSetWindowTitle(window, "StarBrowser");
                    }
                    
                    alert_text = extract_alert_message(active_page.body);
                } else {
                    status_text = "Error: " + active_page.error_message;
                    std::string error_html = "<h1>Error loading page</h1><p>" + active_page.error_message + "</p>";
                    std::string temp_css = "";
                    page_dom = parse_html_to_dom(error_html, temp_css);
                    css_classes.clear();
                    glfwSetWindowTitle(window, "StarBrowser - Error");
                    alert_text = "";
                }
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
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0, 0, 0, 1));
        ImGui::BeginChild("RenderViewport", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 5), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        
        // Draw the document body background using ImGui DrawList
        auto body_it = css_classes.find("body");
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 min_p = ImGui::GetWindowPos();
        ImVec2 max_p = ImVec2(min_p.x + ImGui::GetWindowWidth(), min_p.y + ImGui::GetWindowHeight());
        
        if (body_it != css_classes.end()) {
            const auto& body_style = body_it->second;
            if (body_style.has_gradient) {
                ImU32 col_start = ImGui::ColorConvertFloat4ToU32(body_style.gradient_start);
                ImU32 col_end = ImGui::ColorConvertFloat4ToU32(body_style.gradient_end);
                // Draw vertical gradient (using corners)
                draw_list->AddRectFilledMultiColor(min_p, max_p, col_start, col_start, col_end, col_end);
            } else if (body_style.has_bg) {
                draw_list->AddRectFilled(min_p, max_p, ImGui::ColorConvertFloat4ToU32(body_style.bg_color));
            } else {
                draw_list->AddRectFilled(min_p, max_p, IM_COL32(255, 255, 255, 255));
            }
        } else {
            // Default to white background
            draw_list->AddRectFilled(min_p, max_p, IM_COL32(255, 255, 255, 255));
        }

        // Render DOM tree
        CssStyle default_style;
        default_style.color = ImVec4(0, 0, 0, 1); // Default text color is black
        default_style.has_color = true;
        bool default_inline_flow = false;
        render_node(page_dom, default_style, default_inline_flow);

        ImGui::EndChild();
        ImGui::PopStyleColor(2);

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
