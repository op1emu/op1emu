#include "svg_background_renderer.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#ifdef OP1_HAVE_RESVG
#include <resvg.h>
struct SvgBackgroundRenderer::Impl {
    resvg_render_tree *tree = nullptr;
    resvg_size size{};
    ~Impl() {
        if (tree)
            resvg_tree_destroy(tree);
    }
};
#else
struct SvgBackgroundRenderer::Impl {};
#endif

bool SvgBackgroundRenderer::Available() {
#ifdef OP1_HAVE_RESVG
    return true;
#else
    return false;
#endif
}
size_t SvgBackgroundRenderer::BufferSize(uint32_t w, uint32_t h) {
    // Bound the destination before multiplying or allocating.
    constexpr uint64_t maxPixels = 16 * 1024 * 1024;
    const uint64_t pixels = uint64_t(w) * h;
    if (!w || !h || w > 8192 || h > 8192 || pixels > maxPixels ||
        pixels > std::numeric_limits<size_t>::max() / 4)
        throw std::runtime_error("SVG raster size exceeds 8192px / 16 megapixel limit");
    return static_cast<size_t>(pixels * 4);
}
SvgBackgroundRenderer::SvgBackgroundRenderer(const std::string &path, const std::string &stylesheet)
    : impl_(std::make_unique<Impl>()) {
#ifdef OP1_HAVE_RESVG
    static std::once_flag logging;
    std::call_once(logging, resvg_init_log);
    std::unique_ptr<resvg_options, decltype(&resvg_options_destroy)> options(resvg_options_create(),
                                                                             resvg_options_destroy);
    if (!options)
        throw std::runtime_error("Cannot allocate resvg options");
    const auto absolute = std::filesystem::absolute(path);
    const auto resources = absolute.parent_path().string();
    resvg_options_set_resources_dir(options.get(), resources.c_str());
    if (!stylesheet.empty())
        resvg_options_set_stylesheet(options.get(), stylesheet.c_str());
    resvg_options_load_system_fonts(options.get());
    const auto error = resvg_parse_tree_from_file(absolute.string().c_str(), options.get(), &impl_->tree);
    if (error != RESVG_OK || !impl_->tree)
        throw std::runtime_error("Cannot load SVG " + path + " (resvg error " + std::to_string(error) + ")");
    impl_->size = resvg_get_image_size(impl_->tree);
    if (!std::isfinite(Width()) || !std::isfinite(Height()) || Width() <= 0 || Height() <= 0 ||
        Width() > 8192 || Height() > 8192)
        throw std::runtime_error("Invalid or oversized SVG intrinsic viewport: " + path);
#else
    (void)path;
    (void)stylesheet;
    throw std::runtime_error("SVG support was disabled at build time (OP1_ENABLE_SVG=OFF)");
#endif
}
SvgBackgroundRenderer::~SvgBackgroundRenderer() = default;
float SvgBackgroundRenderer::Width() const {
#ifdef OP1_HAVE_RESVG
    return impl_->size.width;
#else
    return 0;
#endif
}
float SvgBackgroundRenderer::Height() const {
#ifdef OP1_HAVE_RESVG
    return impl_->size.height;
#else
    return 0;
#endif
}
SvgPixels SvgBackgroundRenderer::Render(uint32_t width, uint32_t height) const {
    SvgPixels result{width, height, std::vector<uint8_t>(BufferSize(width, height), 0)};
#ifdef OP1_HAVE_RESVG
    const float scale = std::min(width / Width(), height / Height());
    auto transform = resvg_transform_identity();
    transform.a = transform.d = scale;
    transform.e = (width - Width() * scale) / 2;
    transform.f = (height - Height() * scale) / 2;
    resvg_render(impl_->tree, transform, width, height, reinterpret_cast<char *>(result.rgba.data()));
#endif
    return result;
}
