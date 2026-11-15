// Checks the shipped runtime using the same resource discovery as AdVisual.
#include <gtkmm.h>
#include <common.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        const std::filesystem::path prefix = std::filesystem::u8path(get_advisual_dir());
        if (!std::filesystem::is_regular_file(prefix / "share/advisual/advisual.ini"))
            throw std::runtime_error("Missing default AdVisual settings");
        if (!std::filesystem::is_regular_file(prefix / "share/advisual/themes/DefaultTheme.css"))
            throw std::runtime_error("Missing AdVisual theme");
        auto application = Gtk::Application::create("org.othercat.AdVisual.RelocationCheck", Gio::Application::Flags::NON_UNIQUE);
        if (!application->register_application()) throw std::runtime_error("Cannot initialize bundled GTK");
        int images = 0;
        for (const auto& entry : std::filesystem::directory_iterator(prefix / "share/advisual/icons")) {
            if (entry.path().extension() != ".png") continue;
            auto texture = Gdk::Texture::create_from_filename(entry.path().string());
            if (!texture || texture->get_width() < 1) throw std::runtime_error("Cannot load a bundled PNG");
            ++images;
        }
        if (images < 20) throw std::runtime_error("Missing generated application icons");
        // Exercise the dynamic SVG module as well as production PNG paths.
        auto svg = Gdk::Texture::create_from_filename((prefix / "share/advisual/icons/PlayButton.svg").string());
        if (!svg) throw std::runtime_error("Cannot load a bundled SVG");
        GSettingsSchemaSource* source = g_settings_schema_source_get_default();
        GSettingsSchema* schema = source ? g_settings_schema_source_lookup(source, "org.gtk.gtk4.Settings.FileChooser", true) : nullptr;
        if (!schema) throw std::runtime_error("Missing GTK FileChooser schema");
        g_settings_schema_unref(schema);
        Gtk::Label label("AdVisual PAL RIX · 音乐");
        auto* description = pango_font_description_from_string("Sans 12");
        auto* font = pango_context_load_font(label.get_pango_context()->gobj(), description);
        pango_font_description_free(description);
        if (!font) throw std::runtime_error("Cannot resolve a system font");
        g_object_unref(font);
        std::cout << "PASS relocated AdVisual runtime: " << images << " PNGs, SVG loader, GTK schema, system font\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL AdVisual runtime: " << error.what() << '\n';
        return 1;
    }
}
