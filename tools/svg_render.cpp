#include "svg_background_renderer.h"
#include "svg_theme.h"
#include <iostream>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) {
        std::cerr << "Usage: op1-svg-render INPUT.svg WIDTH HEIGHT OUTPUT.png [THEME|THEME.json]\n";
        return 1;
    }
    try {
        const auto css = argc == 6 ? SvgTheme::Load(SvgTheme::Resolve(argv[5])).Stylesheet() : "";
        SvgBackgroundRenderer renderer(argv[1], css);
        auto p = renderer.Render(std::stoul(argv[2]), std::stoul(argv[3]));
        // PNG stores straight alpha; the runtime GL upload uses p unchanged.
        for (size_t i = 0; i < p.rgba.size(); i += 4)
            if (p.rgba[i + 3])
                for (int c = 0; c < 3; ++c)
                    p.rgba[i + c] =
                        std::min(255u, (unsigned(p.rgba[i + c]) * 255 + p.rgba[i + 3] / 2) / p.rgba[i + 3]);
        if (!stbi_write_png(argv[4], p.width, p.height, 4, p.rgba.data(), p.width * 4))
            throw std::runtime_error("PNG write failed");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
