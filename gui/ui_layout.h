#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct UIPoint {
    double x = 0, y = 0;
};
struct UIRegion {
    std::vector<UIPoint> polygon;
    bool Contains(UIPoint point) const;
};
using UIGpio = std::pair<int, int>;
struct UIButton {
    std::string name;
    UIRegion region;
    UIGpio gpio;
};
struct UIEncoder {
    std::string name;
    UIRegion clockwise, counterClockwise;
    UIGpio a, b;
    unsigned phase = 0;
};
struct UIKey {
    std::string name;
    UIGpio gpio;
};
enum class UIScreenFilter { Nearest, Linear };
struct UILayout {
    std::string background;
    std::string themeBackground;
    double scale = 1;
    bool svg = false;
    std::array<UIPoint, 4> screen; // TL, TR, BR, BL, in intrinsic viewport pixels
    UIScreenFilter screenFilter = UIScreenFilter::Nearest;
    std::vector<UIButton> buttons;
    std::vector<UIEncoder> encoders;
    std::vector<UIKey> keys;
    UIRegion volumeUp, volumeDown;
    static UILayout Load(const std::string &path);
};
struct UIFit {
    double scale, x, y;
    static UIFit Into(double artWidth, double artHeight, double width, double height);
    UIPoint ToArt(UIPoint logical) const;
};
// Homogeneous unit-square -> screen mapping; also covers skew/perspective panels.
std::array<double, 16> UIScreenMatrix(const std::array<UIPoint, 4> &quad);

class UIPanelInput {
  public:
    explicit UIPanelInput(UILayout &layout) : layout_(layout) {}
    std::function<void(int, int, bool)> event;
    void Mouse(bool pressed, UIPoint point);
    void Key(int source, UIGpio gpio, bool pressed);
    void ReleaseAll();
    uint8_t Volume() const { return volume_; }

  private:
    void SetSource(int source, const std::vector<UIGpio> &pins);
    UILayout &layout_;
    std::map<int, std::vector<UIGpio>> held_;
    uint8_t volume_ = 128;
};
