#include <common.h>
#include <Util/Settings.h>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

shared_ptr<Gtk::Application> app;
shared_ptr<Gtk::Label> status;
shared_ptr<Gio::SimpleActionGroup> common_action_group;

namespace {
namespace fs = std::filesystem;
string utf8_path(const fs::path& path) {
    const auto value = path.generic_u8string();
    return string(value.begin(), value.end());
}
fs::path executable_directory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (size && size < buffer.size()) return fs::path(std::wstring(buffer.data(), size)).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) return fs::weakly_canonical(buffer.data()).parent_path();
#else
    std::vector<char> buffer(32768);
    const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (size > 0) return fs::path(string(buffer.data(), size)).parent_path();
#endif
    return fs::current_path();
}
const fs::path& resource_prefix() {
    static const fs::path prefix = [] {
        const auto binary = executable_directory();
        std::vector<fs::path> candidates{binary / "../Resources", binary, binary / ".."};
#ifdef _WIN32
        // Retain compatibility with the historical installed layout after trying portable paths.
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AdVisual\\AdVisual", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            wchar_t buffer[32768];
            DWORD size = sizeof(buffer), type = 0;
            if (RegQueryValueExW(key, L"", nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS && type == REG_SZ) {
                buffer[32767] = L'\0';
                candidates.emplace_back(buffer);
            }
            RegCloseKey(key);
        }
#endif
        candidates.emplace_back(INSTALL_PATH);
        for (const auto& candidate : candidates) {
            if (fs::is_regular_file(candidate / "share/advisual/advisual.ini")) return fs::weakly_canonical(candidate);
        }
        return fs::path(INSTALL_PATH);
    }();
    return prefix;
}
void replace_all(string& text, const string& token, const string& replacement) {
    size_t position = 0;
    while ((position = text.find(token, position)) != string::npos) {
        text.replace(position, token.size(), replacement);
        position += replacement.size();
    }
}
fs::path runtime_file(const fs::path& source, const string& suffix, bool xml) {
    std::ifstream input(source, std::ios::binary);
    string contents((std::istreambuf_iterator<char>(input)), {});
    string prefix = utf8_path(resource_prefix());
    gchar* escaped = xml ? g_markup_escape_text(prefix.c_str(), -1) : g_strescape(prefix.c_str(), nullptr);
    replace_all(contents, "@ADVISUAL_PREFIX@", escaped);
    g_free(escaped);
#ifdef _WIN32
    wchar_t windows[32768];
    const UINT length = GetWindowsDirectoryW(windows, 32768);
    if (length && length < 32768) {
        const auto fonts = utf8_path(fs::path(std::wstring(windows, length)) / "Fonts");
        escaped = g_markup_escape_text(fonts.c_str(), -1);
        replace_all(contents, "@SYSTEM_FONTS@", escaped);
        g_free(escaped);
    }
#endif
    gchar* digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, contents.c_str(), contents.size());
    const auto directory = fs::u8path(g_get_user_cache_dir()) / "advisual/runtime";
    fs::create_directories(directory);
    const auto destination = directory / (string(digest) + suffix);
    g_free(digest);
    if (!fs::exists(destination)) {
        std::ofstream output(destination, std::ios::binary);
        output << contents;
        if (!output) throw std::runtime_error("Could not prepare AdVisual runtime resources");
    }
    return destination;
}
void prepare_portable_runtime() {
    static const bool prepared = [] {
        const auto& prefix = resource_prefix();
        if (!fs::is_regular_file(prefix / "share/advisual/portable-runtime.json")) return true;
        auto environment = [](const char* name, const fs::path& path) {
            const auto value = utf8_path(path);
            g_setenv(name, value.c_str(), true);
        };
        environment("XDG_DATA_DIRS", prefix / "share");
        environment("GTK_DATA_PREFIX", prefix);
        environment("GTK_EXE_PREFIX", prefix);
        environment("GSETTINGS_SCHEMA_DIR", prefix / "share/glib-2.0/schemas");
        environment("GIO_MODULE_DIR", prefix / "lib/gio/modules");
        g_setenv("GIO_USE_VFS", "local", true);
        const auto loader_template = prefix / "lib/gdk-pixbuf-2.0/2.10.0/loaders.cache.in";
        if (fs::is_regular_file(loader_template)) {
            environment("GDK_PIXBUF_MODULE_FILE", runtime_file(loader_template, "-loaders.cache", false));
            environment("GDK_PIXBUF_MODULEDIR", loader_template.parent_path() / "loaders");
        }
        const auto fonts = prefix / "etc/fonts/fonts.conf.in";
        if (fs::is_regular_file(fonts)) {
            environment("FONTCONFIG_FILE", runtime_file(fonts, "-fonts.conf", true));
            environment("FONTCONFIG_PATH", fonts.parent_path());
        }
        return true;
    }();
    (void)prepared;
}
}

string get_advisual_dir() {
    prepare_portable_runtime();
    return utf8_path(resource_prefix()) + "/";
}
string get_icon_path(const string& file_name) {
    const auto icon = resource_prefix() / "share/advisual/icons" / fs::u8path(file_name);
    auto png = icon;
    if (icon.extension() == ".svg") {
        png.replace_extension(".png");
        if (fs::is_regular_file(png)) return utf8_path(png);
    }
    return utf8_path(icon);
}
string get_config_dir() {
    prepare_portable_runtime();
#ifdef _WIN32
    if (const char* appdata = g_getenv("APPDATA")) return utf8_path(fs::u8path(appdata) / "advisual") + "/";
#endif
    return utf8_path(fs::u8path(g_get_user_config_dir()) / "advisual") + "/";
}

int sign(int val) {
	return (val > 0) - (val < 0);
}

void string_to_upper(string& str) {
	transform(str.begin(), str.end(), str.begin(), ::toupper);
}

void line_exclusion(int p1, int l1, int p2, int l2, int& out_p, int& out_length) {
	if (p1 != p2 && p1 + l1 != p2 + l2) { // Rect is resizing in both directions.
		out_p = min(p1, p2);
		out_length = max(p1 + p1, p2 + l2) - out_p;
		return;
	}
	if (p1 != p2) { // Rect is resizing left.
		out_p = min(p1, p2);
		out_length = abs(p2 - p1);
	}
	else if (p1 + l1 != p2 + l2) { // Rect is resizing right.
		out_p = p1; // We already know new_rect.x and old_rect.x are equal.
		out_p += min(l1, l2);
		out_length = max(l1 - l2, l2 - l1);
	}
}

string note_number_to_letter(int note_number) {
	string note_symbol = "--1";
	// Add 3 to make up for the pitch start cuttof. Add an octave so it wraps from pitch start.
	int letter = (((note_number - 3) + full_octave) % full_octave); 
	char modifier = '-';
	if (letter >= 4) {
		letter += 1;
	}
	if (letter >= 10) {
		letter += 1;
	}
	modifier = (letter % 2 == 1 ? '-' : '#');
	letter = (letter / 2);
	note_symbol[0] = char(71 - letter);
	note_symbol[1] = modifier;
	note_symbol[2] = char(48 + (note_number / full_octave));
	return note_symbol;
}

bool is_note_flat(int note_number) {
	int semitone = (((note_number - 3) + full_octave) % full_octave);
	if (semitone >= 4) {
		semitone += 1;
	}
	if (semitone >= 10) {
		semitone += 1;
	}
	return (semitone % 2 == 1);
}

float get_float_from_string(string str_val, float min, float max) {
	if (str_val.empty()) return -1.0;
	float ret_float = 1.0;
	try {
		ret_float = stof(str_val);
		ret_float = clamp(ret_float, min, max);
	}
	catch (invalid_argument e) {
		cerr << str_val << " Not a float!\n";
		ret_float = -1.0;
	}
	catch (out_of_range e) {
		cerr << str_val << " Float out of range\n";
		ret_float = -1.0;
	}
	return ret_float;
}

Gtk::ToggleButton create_image_button(string image_name, bool large_icon) {
using namespace Gtk;
	ToggleButton bttn;
	Image bttn_img(ICON_PATH(image_name));
	bttn_img.add_css_class(large_icon ? "large-image" : "small-image");
	bttn_img.set_icon_size(IconSize::LARGE);
	bttn.set_child(bttn_img);
	return bttn;
}

Gtk::ToggleButton create_picture_button(string picture_name) {
using namespace Gtk;
	ToggleButton bttn;
	Gtk::Picture pic = Gtk::Picture(ICON_PATH(picture_name));
	pic.set_size_request(10, 10);
	pic.set_can_shrink(true);
	pic.set_content_fit(Gtk::ContentFit::SCALE_DOWN);
	bttn.set_child(pic);
	return bttn;
}
