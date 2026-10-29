#include "ui_layout.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
using nlohmann::json;
namespace {
double Number(const json &v) {
    double n = v.get<double>();
    if (!std::isfinite(n) || std::abs(n) > 1000000)
        throw std::runtime_error("Invalid UI coordinate");
    return n;
}
UIPoint Point(const json &v) {
    if (!v.is_array() || v.size() != 2)
        throw std::runtime_error("Expected UI point [x,y]");
    return {Number(v[0]), Number(v[1])};
}
double Cross(UIPoint a, UIPoint b, UIPoint c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
UIRegion Region(const json &v) {
    UIRegion r;
    if (v.size() == 4 && v[0].is_number()) {
        double x = Number(v[0]), y = Number(v[1]), w = Number(v[2]), h = Number(v[3]);
        if (w <= 0 || h <= 0)
            throw std::runtime_error("Empty UI rectangle");
        r.polygon = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    } else {
        if (!v.is_array() || v.size() < 3 || v.size() > 32)
            throw std::runtime_error("Invalid UI polygon");
        for (const auto &p : v)
            r.polygon.push_back(Point(p));
    }
    double sign = 0;
    for (size_t i = 0; i < r.polygon.size(); ++i) {
        double c =
            Cross(r.polygon[i], r.polygon[(i + 1) % r.polygon.size()], r.polygon[(i + 2) % r.polygon.size()]);
        if (std::abs(c) < 1e-8 || (sign && c * sign < 0))
            throw std::runtime_error("UI polygons must be convex and ordered");
        sign = c;
    }
    return r;
}
UIGpio Gpio(const json &v) {
    if (!v.is_array() || v.size() != 2 || !v[0].is_number_integer() || !v[1].is_number_integer())
        throw std::runtime_error("Invalid UI GPIO");
    UIGpio p{v[0].get<int>(), v[1].get<int>()};
    if (p.first < 0 || p.first > 6 || p.second < 0 || p.second > 15)
        throw std::runtime_error("UI GPIO out of range");
    return p;
}
} // namespace
bool UIRegion::Contains(UIPoint p) const {
    if (polygon.empty())
        return false;
    double sign = 0;
    for (size_t i = 0; i < polygon.size(); ++i) {
        double c = Cross(polygon[i], polygon[(i + 1) % polygon.size()], p);
        if (std::abs(c) < 1e-8)
            continue;
        if (sign && c * sign < 0)
            return false;
        sign = c;
    }
    return true;
}
UILayout UILayout::Load(const std::string &path) {
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Cannot open UI config: " + path);
    json j;
    f >> j;
    UILayout l;
    l.background =
        (std::filesystem::path(path).parent_path() / j.at("background").get<std::string>()).string();
    l.svg = std::filesystem::path(l.background).extension() == ".svg";
    if (j.contains("theme_background"))
        l.themeBackground = (std::filesystem::path(path).parent_path() /
                             j.at("theme_background").get<std::string>()).string();
    l.scale = Number(j.at("scale"));
    if (l.scale <= 0 || l.scale > 8)
        throw std::runtime_error("UI scale must be in (0,8]");
    auto &d = j.at("display");
    const auto filter = d.value("filter", std::string("nearest"));
    if (filter == "linear")
        l.screenFilter = UIScreenFilter::Linear;
    else if (filter != "nearest")
        throw std::runtime_error("display.filter must be nearest or linear");
    if (d.contains("quad")) {
        auto r = Region(d.at("quad"));
        if (r.polygon.size() != 4)
            throw std::runtime_error("Screen requires four corners");
        std::copy(r.polygon.begin(), r.polygon.end(), l.screen.begin());
    } else {
        auto r = Region(json::array({d.at("left"), d.at("top"), d.at("width"), d.at("height")}));
        std::copy(r.polygon.begin(), r.polygon.end(), l.screen.begin());
    }
    UIScreenMatrix(l.screen); // reject singular quads before creating a window
    for (auto &[name, b] : j.at("buttons").items())
        l.buttons.push_back({name, Region(b.at("position")), Gpio(b.at("gpio"))});
    if (j.contains("encoders"))
        for (auto &[name, e] : j.at("encoders").items())
            l.encoders.push_back({name, Region(e.at("clockwise")), Region(e.at("counter_clockwise")),
                                  Gpio(e.at("clockwise_gpio")), Gpio(e.at("counter_clockwise_gpio"))});
    if (j.contains("keycaps"))
        for (auto &[name, g] : j.at("keycaps").items())
            l.keys.push_back({name, Gpio(g)});
    if (j.contains("volume_knob")) {
        l.volumeUp = Region(j["volume_knob"].at("clockwise"));
        l.volumeDown = Region(j["volume_knob"].at("counter_clockwise"));
    }
    return l;
}
UIFit UIFit::Into(double aw, double ah, double w, double h) {
    if (!(aw > 0 && ah > 0 && w > 0 && h > 0))
        throw std::runtime_error("Cannot fit an empty UI viewport");
    double s = std::min(w / aw, h / ah);
    return {s, (w - aw * s) / 2, (h - ah * s) / 2};
}
UIPoint UIFit::ToArt(UIPoint p) const { return {(p.x - x) / scale, (p.y - y) / scale}; }
std::array<double, 16> UIScreenMatrix(const std::array<UIPoint, 4> &p) {
    const double dx1 = p[1].x - p[2].x, dx2 = p[3].x - p[2].x, dx3 = p[0].x - p[1].x + p[2].x - p[3].x;
    const double dy1 = p[1].y - p[2].y, dy2 = p[3].y - p[2].y, dy3 = p[0].y - p[1].y + p[2].y - p[3].y;
    const double det = dx1 * dy2 - dx2 * dy1;
    if (std::abs(det) < 1e-8)
        throw std::runtime_error("Singular display quad");
    const double g = (dx3 * dy2 - dx2 * dy3) / det, h = (dx1 * dy3 - dx3 * dy1) / det;
    return {p[1].x - p[0].x + g * p[1].x,
            p[1].y - p[0].y + g * p[1].y,
            0,
            g,
            p[3].x - p[0].x + h * p[3].x,
            p[3].y - p[0].y + h * p[3].y,
            0,
            h,
            0,
            0,
            1,
            0,
            p[0].x,
            p[0].y,
            0,
            1};
}
void UIPanelInput::SetSource(int source, const std::vector<UIGpio> &pins) {
    std::set<UIGpio> before, after;
    for (const auto &[s, v] : held_)
        before.insert(v.begin(), v.end());
    if (pins.empty())
        held_.erase(source);
    else
        held_[source] = pins;
    for (const auto &[s, v] : held_)
        after.insert(v.begin(), v.end());
    if (event) {
        for (auto p : before)
            if (!after.count(p))
                event(p.first, p.second, false);
        for (auto p : after)
            if (!before.count(p))
                event(p.first, p.second, true);
    }
}
void UIPanelInput::Mouse(bool pressed, UIPoint point) {
    if (!pressed) {
        SetSource(-1, {});
        return;
    } // release where the press began
    if (layout_.volumeUp.Contains(point)) {
        volume_ = std::min(255, int(volume_) + 5);
        return;
    }
    if (layout_.volumeDown.Contains(point)) {
        volume_ = std::max(0, int(volume_) - 5);
        return;
    }
    std::vector<UIGpio> pins;
    for (const auto &b : layout_.buttons)
        if (b.region.Contains(point))
            pins.push_back(b.gpio);
    SetSource(-1, pins);
    constexpr unsigned sequence[] = {0, 2, 3, 1};
    for (auto &e : layout_.encoders) {
        int direction = e.clockwise.Contains(point) ? 1 : (e.counterClockwise.Contains(point) ? -1 : 0);
        if (!direction)
            continue;
        e.phase = (e.phase + 4 + direction) % 4;
        if (event) {
            event(e.a.first, e.a.second, sequence[e.phase] & 2);
            event(e.b.first, e.b.second, sequence[e.phase] & 1);
        }
    }
}
void UIPanelInput::Key(int source, UIGpio gpio, bool pressed) {
    SetSource(source, pressed ? std::vector<UIGpio>{gpio} : std::vector<UIGpio>{});
}
void UIPanelInput::ReleaseAll() {
    while (!held_.empty())
        SetSource(held_.begin()->first, {});
    for (auto &e : layout_.encoders) {
        if (event && e.phase) {
            event(e.a.first, e.a.second, false);
            event(e.b.first, e.b.second, false);
        }
        e.phase = 0;
    }
}
