// Run with: pal_editor_test /path/to/PAL/RIX /path/to/temporary/output
// Uses the production GTK widgets, importer, undo actions and ROL/BNK I/O.
#include <gtkmm.h>
#include <ArrangementView.h>
#include <CommonWidgets.h>
#include <ComposerPanel.h>
#include <FileAccess.h>
#include <Instrument.h>
#include <RixImport.h>
#include <Track.h>
#include <common.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
void require(bool condition, const std::string& message) {
    // Unlike assert(), these checks remain active in a Release build.
    if (!condition) throw std::runtime_error(message);
}

struct CaptureProductLog {
    explicit CaptureProductLog(std::streambuf* output) : previous(std::cout.rdbuf(output)) {}
    ~CaptureProductLog() { std::cout.rdbuf(previous); }
    std::streambuf* previous;
};

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot read " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void compare_notes(const NoteGroup& expected, const NoteGroup& actual, const std::string& label) {
    require(actual.size() == expected.size(), label + ": note count changed");
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto& a = expected.notes[i];
        const auto& b = actual.notes[i];
        const auto location = label + " note " + std::to_string(i);
        require(a.offset == b.offset, location + ": offset changed");
        require(a.pitch == b.pitch, location + ": pitch changed");
        require(a.length == b.length, location + ": length changed");
    }
}

void validate_notes(const Track& track, const std::string& label) {
    require(track.get_tick_count() > 0 && track.get_tick_count() <= 65535, label + ": invalid total ticks");
    for (size_t ch = 0; ch < track.channels.size(); ++ch) {
        int previous_end = 0;
        for (const auto& note : track.channels[ch].notes) {
            require(note.offset >= previous_end, label + ": overlapping/out-of-order mono notes");
            require(note.length > 0 && note.get_end_offset() <= 65535, label + ": invalid note length/end");
            require(note.pitch >= 0 && note.pitch < pitch_range, label + ": invalid note pitch");
            previous_end = note.get_end_offset();
        }
    }
}

void compare_track(const Track& expected, const Track& actual, const std::string& label, int edited_lane = -1) {
    require(actual.channels.size() == expected.channels.size(), label + ": channel count changed");
    require(actual.melodic_mode == expected.melodic_mode, label + ": rhythm mode changed");
    require(actual.ticks_per_beat == expected.ticks_per_beat, label + ": ticks per beat changed");
    require(actual.beats_per_measure == expected.beats_per_measure, label + ": beats per measure changed");
    require(actual.basic_tempo == expected.basic_tempo, label + ": basic tempo changed");
    require(actual.tempo_events == expected.tempo_events, label + ": tempo events changed");
    if (edited_lane < 0) require(actual.get_tick_count() == expected.get_tick_count(), label + ": trailing rests/total ticks changed");
    for (size_t ch = 0; ch < expected.channels.size(); ++ch) {
        const auto lane_label = label + " CH" + std::to_string(ch + 1);
        const auto& a = expected.channels[ch];
        const auto& b = actual.channels[ch];
        require(a.channel_number == b.channel_number, lane_label + ": channel identity changed");
        require(a.instrument_events == b.instrument_events, lane_label + ": instrument events changed");
        require(a.volume_events == b.volume_events, lane_label + ": volume events changed");
        require(a.pitch_events == b.pitch_events, lane_label + ": pitch events changed");
        if (int(ch) != edited_lane) compare_notes(a.notes, b.notes, lane_label);
    }
}

void compare_operator(const OPLFM& a, const OPLFM& b, const std::string& label) {
    constexpr std::array<std::string_view, 14> names = {
        "attack_rate", "decay_rate", "sustain_level", "release_rate", "frequency_multiplier",
        "feedback", "output_level", "ksl", "waveform", "sustain_sound", "ksr", "vibrato", "tremelo", "additive_synth"
    };
    auto fields = [](const OPLFM& op) {
        return std::array<unsigned, 14>{op.attack_rate, op.decay_rate, op.sustain_level, op.release_rate,
            op.frequency_multiplier, op.feedback, op.output_level, op.ksl, op.waveform,
            op.sustain_sound, op.ksr, op.vibrato, op.tremelo, op.additive_synth};
    };
    const auto expected = fields(a), actual = fields(b);
    for (size_t field = 0; field < names.size(); ++field) {
        require(expected[field] == actual[field], label + ": " + std::string(names[field]) + " changed");
    }
}

void compare_bank(const Bank& expected, const Bank& actual, const std::string& label) {
    require(expected.file_version_major == actual.file_version_major && expected.file_version_minor == actual.file_version_minor,
        label + ": bank version changed");
    require(expected.instruments.size() == actual.instruments.size(), label + ": instrument count changed");
    for (size_t i = 0; i < expected.instruments.size(); ++i) {
        const auto& a = expected.instruments[i];
        const auto& b = actual.instruments[i];
        const auto location = label + " instrument " + std::to_string(i);
        require(std::equal(std::begin(a.name), std::end(a.name), std::begin(b.name)), location + ": name changed");
        require(a.flags == b.flags, location + ": flags changed");
        require(a.percussion_mode == b.percussion_mode, location + ": percussion mode changed");
        require(a.voice_number == b.voice_number, location + ": percussion voice changed");
        compare_operator(a.modulator, b.modulator, location + " modulator");
        compare_operator(a.carrier, b.carrier, location + " carrier");
    }
}

void expect_saved(ComposerPanel& composer, bool saved, const std::string& label) {
    // Historical API name: is_unsaved() actually returns true when all changes are saved.
    require(composer.is_unsaved() == saved, label + ": incorrect saved/dirty flag");
}

struct Editor {
    Bank bank;
    std::unique_ptr<Track> owned_track;
    std::unique_ptr<ComposerPanel> composer;
    std::vector<std::unique_ptr<ChannelButton>> buttons;
    std::unique_ptr<Gtk::Window> window;
    Editor() {
        current_bank = &bank;
        status = std::make_shared<Gtk::Label>();
        composer = std::make_unique<ComposerPanel>();
        owned_track.reset(current_track);
        for (int ch = 0; ch < 11; ++ch) buttons.push_back(std::make_unique<ChannelButton>(ch));
        ChannelButton::set_pressed_channel(0);
        require(composer->get_arrangement_view(), "All-channels editor is not the default");
        window = std::make_unique<Gtk::Window>();
        window->set_default_size(1280, 780);
        window->set_title("PAL editor regression");
        app->add_window(*window);
        window->set_child(*composer);
        window->present();
        drain_events();
    }
    ~Editor() {
        if (window) { window->unset_child(); app->remove_window(*window); window.reset(); }
        ChannelButton::currently_pressed = nullptr;
        ChannelButton::buttons.clear();
        buttons.clear();
        composer.reset();
        owned_track.reset();
        current_track = nullptr;
        current_channel = nullptr;
        current_bank = nullptr;
        status.reset();
    }
    static void drain_events() {
        const auto context = Glib::MainContext::get_default();
        for (int iteration = 0; iteration < 100 && context->pending(); ++iteration) context->iteration(false);
    }
    void import(const fs::path& path) {
        RixImport::load_file(path.string());
        composer->mark_import_unsaved(); // Same transition as the real Load dialog.
        composer->set_arrangement_view(true);
        expect_saved(*composer, false, path.filename().string() + " imported");
        require(current_track->file_path.empty() && current_bank->file_path.empty(), "Import retained a source-overwriting save path");
    }
    void capture_all_channels(const fs::path& path, std::ostream& report) {
        composer->set_arrangement_view(true);
        composer->arrangement().fit_song();
        GtkWidget* widget = GTK_WIDGET(composer->gobj());
        GdkFrameClock* clock = gtk_widget_get_frame_clock(widget);
        require(clock != nullptr, "Composer has no realized GTK frame clock for capture");
        struct FrameWait {
            GMainLoop* loop;
            bool painted = false;
            bool timed_out = false;
        } frame{g_main_loop_new(nullptr, false)};
        const gulong after_paint = g_signal_connect(clock, "after-paint",
            G_CALLBACK(+[](GdkFrameClock*, gpointer data) {
                auto& frame = *static_cast<FrameWait*>(data);
                frame.painted = true;
                g_main_loop_quit(frame.loop);
            }), &frame);
        const guint timeout = g_timeout_add(3000, +[](gpointer data) -> gboolean {
            auto& frame = *static_cast<FrameWait*>(data);
            frame.timed_out = true;
            g_main_loop_quit(frame.loop);
            return G_SOURCE_REMOVE;
        }, &frame);
        gtk_widget_queue_draw(widget);
        gdk_frame_clock_request_phase(clock, GDK_FRAME_CLOCK_PHASE_PAINT);
        g_main_loop_run(frame.loop);
        g_signal_handler_disconnect(clock, after_paint);
        if (!frame.timed_out) g_source_remove(timeout);
        g_main_loop_unref(frame.loop);
        require(frame.painted, "Timed out waiting for the real Composer to paint " + path.filename().string());

        const int width = gtk_widget_get_width(widget), height = gtk_widget_get_height(widget);
        require(width > 0 && height > 0, "Composer has no allocated dimensions for capture");
        GtkNative* native = gtk_widget_get_native(widget);
        GskRenderer* renderer = native ? gtk_native_get_renderer(native) : nullptr;
        require(renderer != nullptr, "Composer has no native GTK renderer for capture");
        GdkPaintable* paintable = gtk_widget_paintable_new(widget);
        GtkSnapshot* snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
        GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
        g_object_unref(paintable);
        require(node != nullptr, "Composer produced an empty GTK render node");
        const graphene_rect_t viewport = GRAPHENE_RECT_INIT(0, 0, float(width), float(height));
        GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &viewport);
        gsk_render_node_unref(node);
        require(texture != nullptr, "GTK renderer did not produce a Composer texture");
        const bool correct_size = gdk_texture_get_width(texture) == width && gdk_texture_get_height(texture) == height;
        const bool saved = gdk_texture_save_to_png(texture, path.string().c_str());
        g_object_unref(texture);
        require(correct_size && saved, "Could not save the real Composer PNG: " + path.string());
        report << "PASS render " << path.filename().string() << " " << width << "x" << height << '\n';
    }
    void save_pair(const fs::path& rol) {
        auto bnk = rol;
        bnk.replace_extension(".BNK");
        current_track->save_file(rol.string());
        require(!FileAccess::ios_file.fail() && fs::file_size(rol) > 300, "ROL save failed: " + rol.string());
        current_bank->save_file(bnk.string());
        require(!FileAccess::ios_file.fail() && fs::file_size(bnk) >= 28, "BNK save failed: " + bnk.string());
        const auto header = read_bytes(bnk);
        require(header.size() >= 8 && std::string(header.begin() + 2, header.begin() + 8) == "ADLIB-",
            bnk.filename().string() + ": missing standard ADLIB- bank signature");
        composer->on_track_saved();
        expect_saved(*composer, true, rol.filename().string() + " saved");
    }
    void reload_pair(const fs::path& rol, const Track& expected_track, const Bank& expected_bank) {
        auto bnk = rol;
        bnk.replace_extension(".BNK");
        current_bank->load_file(bnk.string());
        current_track->load_file(rol.string());
        compare_track(expected_track, *current_track, rol.filename().string());
        compare_bank(expected_bank, *current_bank, bnk.filename().string());
        expect_saved(*composer, true, rol.filename().string() + " reloaded");
        drain_events();
    }
};

void assert_note(int channel, const Note& expected, const std::string& label) {
    const auto* actual = current_track->get_channel(channel)->get_note_on_tick(expected.offset);
    require(actual && actual->offset == expected.offset && actual->length == expected.length && actual->pitch == expected.pitch,
        label + ": edited note is missing or has incorrect properties");
}

void exercise_lane(Editor& editor, const fs::path& source, const fs::path& output, int channel, std::ostream& report) {
    editor.import(source);
    const std::string label = source.stem().string() + " CH" + std::to_string(channel + 1);
    auto baseline_file = output / (source.stem().string() + "-ch" + std::to_string(channel + 1) + "-baseline.ROL");
    editor.save_pair(baseline_file);
    const Track baseline = *current_track;
    const Bank original_bank = *current_bank;
    const auto& notes = baseline.channels[channel].notes;
    require(notes.size() >= 2, label + ": not enough real notes to test overlapping edits");
    const Note first = notes.notes[0], neighbour = notes.notes[1];
    auto& composer = *editor.composer;
    auto& view = composer.arrangement();
    auto select = [&] { require(view.select_note(channel, first.offset), label + ": could not select real note"); };
    auto undo_redo_undo = [&](const std::string& operation) {
        validate_notes(*current_track, label + operation);
        compare_track(baseline, *current_track, label + operation, channel);
        compare_bank(original_bank, *current_bank, label + operation);
        expect_saved(composer, false, label + operation);
        const Track changed = *current_track;
        composer.action_group->activate_action("undo");
        compare_track(baseline, *current_track, label + operation + " undo");
        expect_saved(composer, true, label + operation + " undo");
        composer.action_group->activate_action("redo");
        compare_track(changed, *current_track, label + operation + " redo");
        expect_saved(composer, false, label + operation + " redo");
        composer.action_group->activate_action("undo");
        compare_track(baseline, *current_track, label + operation + " final undo");
        expect_saved(composer, true, label + operation + " final undo");
    };

    select();
    const int semitone = first.pitch < pitch_range - 1 ? 1 : -1;
    view.move_selected(1, semitone);
    assert_note(channel, Note(first.offset + 1, first.pitch + semitone, first.length), label + " move/transpose");
    undo_redo_undo(" move/transpose");

    select();
    const int overwrite_pitch = first.pitch == neighbour.pitch ? semitone : 0;
    view.move_selected(neighbour.offset - first.offset, overwrite_pitch);
    assert_note(channel, Note(neighbour.offset, first.pitch + overwrite_pitch, first.length), label + " overwrite neighbour");
    require(current_track->get_channel(channel)->get_note_on_tick(first.offset) == nullptr, label + ": move retained its source note");
    undo_redo_undo(" overwrite neighbour"); // Exact comparison must restore every clipped/overwritten neighbour.

    select();
    view.move_selected(0, 0, 3);
    assert_note(channel, Note(first.offset, first.pitch, first.length + 3), label + " resize");
    undo_redo_undo(" resize");

    select();
    view.erase_selected();
    require(current_track->get_channel(channel)->notes.size() + 1 == notes.size(), label + ": erase did not remove exactly one note");
    require(current_track->get_channel(channel)->get_note_on_tick(first.offset) == nullptr, label + ": erase left the selected note");
    undo_redo_undo(" erase");

    select();
    view.move_selected(1000000, 1000000, 1000000);
    const auto& end_note = current_track->get_channel(channel)->notes.back();
    require(end_note.get_end_offset() == 65535 && end_note.pitch == pitch_range - 1, label + ": upper ROL bounds not enforced");
    undo_redo_undo(" upper bounds");

    select();
    view.move_selected(-1000000, -1000000, -1000000);
    assert_note(channel, Note(0, 0, 1), label + " lower bounds");
    undo_redo_undo(" lower bounds");

    composer.set_arrangement_view(false);
    require(!composer.get_arrangement_view(), label + ": cannot open the piano editor");
    composer.set_arrangement_view(true);
    compare_track(baseline, *current_track, label + " view switching");
    expect_saved(composer, true, label + " view switching");

    select();
    view.move_selected(1, semitone, 2);
    const Track edited = *current_track;
    const Bank edited_bank = *current_bank;
    const auto edited_file = output / (source.stem().string() + "-ch" + std::to_string(channel + 1) + "-edited.ROL");
    editor.save_pair(edited_file);
    editor.reload_pair(edited_file, edited, edited_bank);
    report << "PASS editor " << label << ": move/transpose, overwrite, resize, erase, bounds, undo/redo, dirty, save/reload\n";
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: pal_editor_test RIX_DIRECTORY TEMP_OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        const fs::path source = fs::canonical(argv[1]);
        const fs::path output = fs::absolute(argv[2]);
        fs::create_directories(output);
        require(fs::weakly_canonical(output) != source, "Output must be a temporary directory separate from the RIX source");
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(source)) {
            auto extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::toupper(c)); });
            if (entry.is_regular_file() && extension == ".RIX") files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        require(files.size() == 86, "Expected the 86 PAL RIX fixtures, found " + std::to_string(files.size()));
        std::ostream report(std::cout.rdbuf());
        std::ofstream log(output / "pal_editor_trace.log");
        require(bool(log), "Cannot create regression trace log");
        CaptureProductLog capture(log.rdbuf());
        app = Gtk::Application::create("org.advisual.PalEditorRegression", Gio::Application::Flags::NON_UNIQUE);
        require(app->register_application(), "Could not register the GTK regression application");
        // Use the actual bundled default theme without loading/writing user settings.
        auto theme = Gtk::CssProvider::create();
        theme->load_from_path(get_advisual_dir() + "share/advisual/themes/DefaultTheme.css");
        Gtk::StyleProvider::add_provider_for_display(Gdk::Display::get_default(), theme, GTK_STYLE_PROVIDER_PRIORITY_USER);
        {
            Editor editor;
            size_t melodic = 0, rhythm = 0, patch_count = 0;
            for (const auto& path : files) {
                const auto original_source = read_bytes(path);
                editor.import(path);
                validate_notes(*current_track, path.filename().string());
                const Track expected_track = *current_track;
                const Bank expected_bank = *current_bank;
                current_track->melodic_mode ? ++melodic : ++rhythm;
                patch_count += current_bank->instruments.size();
                const auto rol = output / (path.stem().string() + ".ROL");
                editor.save_pair(rol);
                editor.reload_pair(rol, expected_track, expected_bank);
                require(read_bytes(path) == original_source, path.filename().string() + ": source RIX was modified");
                report << "PASS roundtrip " << path.filename().string() << '\n';
            }
            require(melodic > 0 && rhythm > 0, "Fixtures did not cover both melodic and rhythm modes");
            auto find_fixture = [&](const std::string& name) {
                const auto found = std::find_if(files.begin(), files.end(), [&](const auto& path) { return path.stem() == name; });
                require(found != files.end(), "Missing representative fixture " + name);
                return *found;
            };
            for (const auto& name : {"MUSIC_36", "MUSIC_27"}) {
                const auto path = find_fixture(name);
                editor.import(path);
                editor.capture_all_channels(output / (std::string(name) == "MUSIC_36" ?
                    "music36-all-channels.png" : "music27-all-channels.png"), report);
                int melody_channel = -1, drum_channel = -1;
                for (int ch = 0; ch < (current_track->melodic_mode ? 9 : 11); ++ch) {
                    if (current_track->get_channel(ch)->notes.size() < 2) continue;
                    if (!current_track->melodic_mode && ch >= 6) { if (drum_channel < 0) drum_channel = ch; }
                    else if (melody_channel < 0) melody_channel = ch;
                }
                require(melody_channel >= 0, std::string(name) + ": no melodic lane with two notes");
                const bool percussion_song = !current_track->melodic_mode;
                exercise_lane(editor, path, output, melody_channel, report);
                if (percussion_song) {
                    require(drum_channel >= 0, std::string(name) + ": no percussion lane with two notes");
                    exercise_lane(editor, path, output, drum_channel, report);
                }
            }
            report << "PASS all: 86 roundtrips (" << melodic << " melodic / " << rhythm << " rhythm), "
                << patch_count << " complete OPL patches, three real arrangement lanes; output " << output << '\n';
        }
        app.reset();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL pal_editor_test: " << error.what() << '\n';
        return 1;
    }
}
